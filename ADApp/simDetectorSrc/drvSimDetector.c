/* drvSimDetector.c
 *
 * This is a driver for a simulated area detector.
 *
 * Author: Mark Rivers
 *         University of Chicago
 *
 * Created:  March 20, 2008
 *
 */
 
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <epicsFindSymbol.h>
#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsMutex.h>
#include <epicsString.h>
#include <epicsStdio.h>
#include <epicsMutex.h>
#include <iocsh.h>
#include <drvSup.h>
#include <epicsExport.h>

#define DEFINE_AREA_DETECTOR_PROTOTYPES 1
#include "ADParamLib.h"
#include "ADUtils.h"
#include "ADInterface.h"


/* Note that the file format enum must agree with the mbbo/mbbi records in the simDetector.template file */
typedef enum {
   SimFormatBinary,
   SimFormatASCII
} SimFormat_t;

/* If we have any private driver parameters they begin with ADFirstDriverParam and should end
   with ADLastDriverParam, which is used for setting the size of the parameter library table */
typedef enum {
   SimGainX = ADFirstDriverParam,
   SimGainY,
   SimResetImage,
   ADLastDriverParam
} DetParam_t;

typedef struct {
    DetParam_t command;
    char *commandString;
} DetCommandStruct;

/* The command strings are the input to FindParam, which returns the corresponding parameter enum value */
static DetCommandStruct DetCommands[] = {
    {SimGainX,      "SIM_GAINX"},  
    {SimGainY,      "SIM_GAINY"},  
    {SimResetImage, "RESET_IMAGE"}  
};

static char *driverName = "drvSimDetector";

ADDrvSet_t ADSimDetector = 
  {
    18,
    ADReport,            /* Standard EPICS driver report function (optional) */
    ADInit,              /* Standard EPICS driver initialisation function (optional) */
    ADSetLog,            /* Defines an external logging function (optional) */
    ADOpen,              /* Driver open function */
    ADClose,             /* Driver close function */
    ADFindParam,         /* Parameter lookup function */
    ADSetInt32Callback,     /* Provides a callback function the driver can call when an int32 value updates */
    ADSetFloat64Callback,   /* Provides a callback function the driver can call when a float64 value updates */
    ADSetStringCallback,    /* Provides a callback function the driver can call when a string value updates */
    ADSetImageDataCallback, /* Provides a callback function the driver can call when the image data updates */
    ADGetInteger,        /* Pointer to function to get an integer value */
    ADSetInteger,        /* Pointer to function to set an integer value */
    ADGetDouble,         /* Pointer to function to get a double value */
    ADSetDouble,         /* Pointer to function to set a double value */
    ADGetString,         /* Pointer to function to get a string value */
    ADSetString,         /* Pointer to function to set a string value */
    ADGetImage,          /* Pointer to function to read image data */
    ADSetImage           /* Pointer to function to write image data */
  };

epicsExportAddress(drvet, ADSimDetector);

typedef struct ADHandle {
    /* The first set of items in this structure will be needed by all drivers */
    int camera;                        /* Index of this camera in list of controlled cameras */
    epicsMutexId mutexId;              /* A mutex to lock access to data structures. */
    ADLogFunc logFunc;                 /* These are for error and debug logging.*/
    void *logParam;
    PARAMS params;
    ADImageDataCallbackFunc pImageDataCallback;
    void *imageDataCallbackParam;

    /* These items are specific to the Simulator driver */
    int framesRemaining;
    epicsEventId eventId;
    void *rawBuffer;
    void *imageBuffer;
    int bufferSize;
} simDetector_t;

#define PRINT   (pCamera->logFunc)

static int numCameras;

/* Pointer to array of controller strutures */
static simDetector_t *allCameras=NULL;


static int simAllocateBuffer(DETECTOR_HDL pCamera, int sizeX, int sizeY, int dataType)
{
    int status = AREA_DETECTOR_OK;
    int bytesPerPixel;
    int bufferSize;
    
    /* Make sure the buffers we have allocated are large enough.
     * rawBuffer is for the entire image, imageBuffer is for the subregion with binning
     * We allocated them both the same size for simplicity and efficiency */
    status |= ADUtils->bytesPerPixel(dataType, &bytesPerPixel);
    bufferSize = sizeX * sizeY * bytesPerPixel;
    if (bufferSize != pCamera->bufferSize) {
        free(pCamera->rawBuffer);
        free(pCamera->imageBuffer);
        pCamera->rawBuffer   = malloc(bytesPerPixel*sizeX*sizeY);
        pCamera->imageBuffer = malloc(bytesPerPixel*sizeX*sizeY);
        pCamera->bufferSize = bufferSize;
    }
    return(status);
}

static int simWriteFile(DETECTOR_HDL pCamera)
{
    /* Writes current frame to disk in simple binary or ASCII format.
     * In either case the data written are imageSizeX, imageSizeY, dataType, data */
    int status = AREA_DETECTOR_OK;
    char fullFileName[MAX_FILENAME_LEN];
    int fileFormat;
    int fileNumber;
    int imageSizeX, imageSizeY, imageSize, dataType, bytesPerPixel;
    int i, autoIncrement;
    FILE *fp;

    /* Get the current parameters */
    ADParam->getInteger(pCamera->params, ADImageSizeX, &imageSizeX);
    ADParam->getInteger(pCamera->params, ADImageSizeY, &imageSizeY);
    ADParam->getInteger(pCamera->params, ADImageSize,  &imageSize);
    ADParam->getInteger(pCamera->params, ADDataType,   &dataType);
    ADParam->getInteger(pCamera->params, ADFileNumber, &fileNumber);
    ADParam->getInteger(pCamera->params, ADAutoIncrement, &autoIncrement);
    ADUtils->bytesPerPixel(dataType, &bytesPerPixel);

    status |= ADUtils->createFileName(pCamera->params, MAX_FILENAME_LEN, fullFileName);
    if (status) { 
        PRINT(pCamera->logParam, ADTraceError, 
              "%s:SimWriteFile error creating full file name, fullFileName=%s, status=%d\n", 
              driverName, fullFileName, status);
        return(status);
    }
    status |= ADParam->getInteger(pCamera->params, ADFileFormat, &fileFormat);
    switch (fileFormat) {
    case SimFormatBinary:
        fp = fopen(fullFileName, "wb");
        if (!fp) {
            PRINT(pCamera->logParam, ADTraceError, 
                  "%s:SimWriteFile error creating file, fullFileName=%s, errno=%d\n", 
                  driverName, fullFileName, errno);
            return(AREA_DETECTOR_ERROR);
        }
        fwrite(&imageSizeX, sizeof(imageSizeX), 1, fp);
        fwrite(&imageSizeY, sizeof(imageSizeY), 1, fp);
        fwrite(&dataType, sizeof(dataType), 1, fp);
        fwrite(pCamera->imageBuffer, bytesPerPixel, imageSizeX*imageSizeY, fp);
        fclose(fp);
        break;
    case SimFormatASCII:
        fp = fopen(fullFileName, "w");
        if (!fp) {
            PRINT(pCamera->logParam, ADTraceError, 
                  "%s:SimWriteFile error creating file, fullFileName=%s, errno=%d\n", 
                  driverName, fullFileName, errno);
            return(AREA_DETECTOR_ERROR);
        }
        fprintf(fp, "%d\n", imageSizeX);
        fprintf(fp, "%d\n", imageSizeY);
        fprintf(fp, "%d\n", dataType);
        switch (dataType) {
            case ADInt8: {
                epicsInt8 *pData = (epicsInt8 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fprintf(fp, "%d\n", pData[i]); }
                break;
            case ADUInt8: {
                epicsUInt8 *pData = (epicsUInt8 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fprintf(fp, "%u\n", pData[i]); }
                break;
            case ADInt16: {
                epicsInt16 *pData = (epicsInt16 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fprintf(fp, "%d\n", pData[i]); }
                break;
            case ADUInt16: {
                epicsUInt16 *pData = (epicsUInt16 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fprintf(fp, "%u\n", pData[i]); }
                break;
            case ADInt32: {
                epicsInt32 *pData = (epicsInt32 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fprintf(fp, "%d\n", pData[i]); }
                break;
            case ADUInt32: {
                epicsUInt32 *pData = (epicsUInt32 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fprintf(fp, "%u\n", pData[i]); }
                break;
            case ADFloat32: {
                epicsFloat32 *pData = (epicsFloat32 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fprintf(fp, "%f\n", pData[i]); }
                break;
            case ADFloat64: {
                epicsFloat64 *pData = (epicsFloat64 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fprintf(fp, "%f\n", pData[i]); }
                break;
        }
        fclose(fp);
        break;
    }

    /* If we got an error then return */
    if (status) return(status);
    
    /* Update the full file name */
    ADParam->setString(pCamera->params, ADFullFileName, fullFileName);

    /* If autoincrement is set then increment file number */
    if (autoIncrement) {
        fileNumber++;
        ADParam->setInteger(pCamera->params, ADFileNumber, fileNumber);
    }
    return(status);
}

static int simReadFile(DETECTOR_HDL pCamera)
{
    /* Reads a file written by simWriteFile from disk in either binary or ASCII format. */
    int status = AREA_DETECTOR_OK;
    char fullFileName[MAX_FILENAME_LEN];
    int fileFormat, fileNumber;
    int imageSizeX, imageSizeY, dataType, bytesPerPixel;
    int i, autoIncrement;
    FILE *fp;

    /* Get the current parameters */
    ADParam->getInteger(pCamera->params, ADAutoIncrement, &autoIncrement);
    ADParam->getInteger(pCamera->params, ADFileNumber,    &fileNumber);

    status |= ADUtils->createFileName(pCamera->params, MAX_FILENAME_LEN, fullFileName);
    if (status) { 
        PRINT(pCamera->logParam, ADTraceError, 
              "%s:SimReadFile error creating full file name, fullFileName=%s, status=%d\n", 
              driverName, fullFileName, status);
        return(status);
    }
    status |= ADParam->getInteger(pCamera->params, ADFileFormat, &fileFormat);
    switch (fileFormat) {
    case SimFormatBinary:
        fp = fopen(fullFileName, "rb");
        if (!fp) {
            PRINT(pCamera->logParam, ADTraceError, 
                  "%s:SimReadFile error opening file, fullFileName=%s, errno=%d\n", 
                  driverName, fullFileName, errno);
            return(AREA_DETECTOR_ERROR);
        }
        fread(&imageSizeX, sizeof(imageSizeX), 1, fp);
        fread(&imageSizeY, sizeof(imageSizeY), 1, fp);
        fread(&dataType, sizeof(dataType), 1, fp);
        ADUtils->bytesPerPixel(dataType, &bytesPerPixel);
        simAllocateBuffer(pCamera, imageSizeX, imageSizeY, dataType);
        fread(pCamera->imageBuffer, bytesPerPixel, imageSizeX*imageSizeY, fp);
        fclose(fp);
        break;
    case SimFormatASCII:
        fp = fopen(fullFileName, "r");
        if (!fp) {
            PRINT(pCamera->logParam, ADTraceError, 
                  "%s:SimReadFile error opening file, fullFileName=%s, errno=%d\n", 
                  driverName, fullFileName, errno);
            return(AREA_DETECTOR_ERROR);
        }
        fscanf(fp, "%d", &imageSizeX);
        fscanf(fp, "%d", &imageSizeY);
        fscanf(fp, "%d", &dataType);
        simAllocateBuffer(pCamera, imageSizeX, imageSizeY, dataType);
        switch (dataType) {
            case ADInt8: {
                int tmp;
                epicsInt8 *pData = (epicsInt8 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) {fscanf(fp, "%d", &tmp); pData[i]=tmp;}}
                break;
            case ADUInt8: {
                unsigned int tmp;
                epicsUInt8 *pData = (epicsUInt8 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) {fscanf(fp, "%u", &tmp); pData[i]=tmp;}}
                break;
            case ADInt16: {
                epicsInt16 *pData = (epicsInt16 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fscanf(fp, "%hd", &pData[i]); }
                break;
            case ADUInt16: {
                epicsUInt16 *pData = (epicsUInt16 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fscanf(fp, "%hu", &pData[i]); }
                break;
            case ADInt32: {
                epicsInt32 *pData = (epicsInt32 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fscanf(fp, "%d", &pData[i]); }
                break;
            case ADUInt32: {
                epicsUInt32 *pData = (epicsUInt32 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fscanf(fp, "%u", &pData[i]); }
                break;
            case ADFloat32: {
                epicsFloat32 *pData = (epicsFloat32 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fscanf(fp, "%f", &pData[i]); }
                break;
            case ADFloat64: {
                epicsFloat64 *pData = (epicsFloat64 *)pCamera->imageBuffer;
                for (i=0; i<imageSizeX*imageSizeY; i++) fscanf(fp, "%lf", &pData[i]); }
                break;
        }
        fclose(fp);
        break;
    }

    /* If we got an error then return */
    if (status) return(status);
    
    /* Update the full file name */
    ADParam->setString(pCamera->params, ADFullFileName, fullFileName);

    /* If autoincrement is set then increment file number */
    if (autoIncrement) {
        fileNumber++;
        ADParam->setInteger(pCamera->params, ADFileNumber, fileNumber);
    }
    
    /* Update the new values of imageSizeX, imageSizeY, dataType and the image data */
    ADParam->setInteger(pCamera->params, ADImageSizeX, imageSizeX);
    ADParam->setInteger(pCamera->params, ADImageSizeY, imageSizeY);
    ADParam->setInteger(pCamera->params, ADDataType, dataType);
    pCamera->pImageDataCallback(pCamera->imageDataCallbackParam, 
                                pCamera->imageBuffer,
                                dataType, imageSizeX, imageSizeY);
    
    return(status);
}

static int simComputeImage(DETECTOR_HDL pCamera)
{
    int status = AREA_DETECTOR_OK;
    int dataType;
    int binX, binY, minX, minY, sizeX, sizeY, maxSizeX, maxSizeY, resetImage;
    int imageSizeX, imageSizeY, imageSize, bytesPerPixel;
    double exposureTime, gain, gainX, gainY, scaleX, scaleY;
    double increment;
    int i, j;

    /* NOTE: The caller of this function must have taken the mutex */
    
    status |= ADParam->getInteger(pCamera->params, ADBinX,        &binX);
    status |= ADParam->getInteger(pCamera->params, ADBinY,        &binY);
    status |= ADParam->getInteger(pCamera->params, ADMinX,        &minX);
    status |= ADParam->getInteger(pCamera->params, ADMinY,        &minY);
    status |= ADParam->getInteger(pCamera->params, ADSizeX,       &sizeX);
    status |= ADParam->getInteger(pCamera->params, ADSizeY,       &sizeY);
    status |= ADParam->getInteger(pCamera->params, ADMaxSizeX,    &maxSizeX);
    status |= ADParam->getInteger(pCamera->params, ADMaxSizeY,    &maxSizeY);
    status |= ADParam->getInteger(pCamera->params, ADDataType,    &dataType);
    status |= ADParam->getInteger(pCamera->params, SimResetImage, &resetImage);
    status |= ADParam->getDouble (pCamera->params, ADAcquireTime, &exposureTime);
    status |= ADParam->getDouble (pCamera->params, ADGain,        &gain);
    status |= ADParam->getDouble (pCamera->params, SimGainX,      &gainX);
    status |= ADParam->getDouble (pCamera->params, SimGainY,      &gainY);
    status |= ADUtils->bytesPerPixel(dataType, &bytesPerPixel);

    /* Make sure parameters are consistent, fix them if they are not */
    if (binX < 0) {binX = 0; status |= ADParam->setInteger(pCamera->params, ADBinX, binX);}
    if (binY < 0) {binY = 0; status |= ADParam->setInteger(pCamera->params, ADBinY, binY);}
    if (minX < 0) {minX = 0; status |= ADParam->setInteger(pCamera->params, ADMinX, minX);}
    if (minY < 0) {minY = 0; status |= ADParam->setInteger(pCamera->params, ADMinY, minY);}
    if (minX > maxSizeX-1) {minX = maxSizeX-1; status |= ADParam->setInteger(pCamera->params, ADMinX, minX);}
    if (minY > maxSizeY-1) {minY = maxSizeY-1; status |= ADParam->setInteger(pCamera->params, ADMinY, minY);}
    if (minX+sizeX > maxSizeX) {sizeX = maxSizeX-minX; status |= ADParam->setInteger(pCamera->params, ADSizeX, sizeX);}
    if (minY+sizeY > maxSizeY) {sizeY = maxSizeY-minY; status |= ADParam->setInteger(pCamera->params, ADSizeY, sizeY);}

    /* Make sure the buffers we have allocated are large enough. */
    status |= simAllocateBuffer(pCamera, maxSizeX, maxSizeY, dataType);

    /* The intensity at each pixel[i,j] is:
     * (i * gainX + j* gainY) + frameCounter * gain * exposureTime * 1000. */
    increment = gain * exposureTime * 1000.;
    scaleX = 0.;
    scaleY = 0.;
    
    /* The following macro simplifies the code */

    #define COMPUTE_ARRAY(DATA_TYPE) {                      \
        DATA_TYPE *pData = (DATA_TYPE *)pCamera->rawBuffer; \
        DATA_TYPE inc = (DATA_TYPE)increment;               \
        if (resetImage) {                                   \
            for (i=0; i<maxSizeY; i++) {                    \
                scaleX = 0.;                                \
                for (j=0; j<maxSizeX; j++) {                \
                    (*pData++) = (DATA_TYPE)(scaleX + scaleY + inc);  \
                    scaleX += gainX;                        \
                }                                           \
                scaleY += gainY;                            \
            }                                               \
        } else {                                            \
            for (i=0; i<maxSizeY; i++) {                    \
                for (j=0; j<maxSizeX; j++) {                \
                     *pData++ += inc;                       \
                }                                           \
            }                                               \
        }                                                   \
    }
        

    switch (dataType) {
        case ADInt8: 
            COMPUTE_ARRAY(epicsInt8);
            break;
        case ADUInt8: 
            COMPUTE_ARRAY(epicsUInt8);
            break;
        case ADInt16: 
            COMPUTE_ARRAY(epicsInt16);
            break;
        case ADUInt16: 
            COMPUTE_ARRAY(epicsUInt16);
            break;
        case ADInt32: 
            COMPUTE_ARRAY(epicsInt32);
            break;
        case ADUInt32: 
            COMPUTE_ARRAY(epicsUInt32);
            break;
        case ADFloat32: 
            COMPUTE_ARRAY(epicsFloat32);
            break;
        case ADFloat64: 
            COMPUTE_ARRAY(epicsFloat64);
            break;
    }
    
    /* Extract the region of interest with binning.  
     * If the entire image is being used (no ROI or binning) that's OK because
     * convertImage detects that case and is very efficient */
    status |= ADUtils->convertImage(pCamera->rawBuffer, 
                                    dataType,
                                    maxSizeX, maxSizeY,
                                    pCamera->imageBuffer,
                                    dataType,
                                    binX, binY,
                                    minX, minY,
                                    sizeX, sizeY,
                                    &imageSizeX, &imageSizeY);
    
    imageSize = imageSizeX * imageSizeY * bytesPerPixel;
    status |= ADParam->setInteger(pCamera->params, ADImageSize,  imageSize);
    status |= ADParam->setInteger(pCamera->params, ADImageSizeX, imageSizeX);
    status |= ADParam->setInteger(pCamera->params, ADImageSizeY, imageSizeY);
    status |= ADParam->setInteger(pCamera->params,SimResetImage, 0);
    return(status);
}

static void simTask(DETECTOR_HDL pCamera)
{
    /* This thread computes new frame data and does the callbacks to send it to higher layers */
    int status = AREA_DETECTOR_OK;
    int dataType;
    int imageSizeX, imageSizeY, imageSize;
    int acquire, autoSave;
    ADStatus_t acquiring;
    double acquireTime, acquirePeriod, delay;
    epicsTimeStamp startTime, endTime;
    double computeTime;

    /* Loop forever */
    while (1) {
    
        epicsMutexLock(pCamera->mutexId);

        /* Is acquisition active? */
        ADParam->getInteger(pCamera->params, ADAcquire, &acquire);
        
        /* If we are not acquiring then wait for a semaphore that is given when acquisition is started */
        if (!acquire) {
            ADParam->setInteger(pCamera->params, ADStatus, ADStatusIdle);
            ADParam->callCallbacks(pCamera->params);
            /* Release the lock while we wait for an event that says acquire has started, then lock again */
            epicsMutexUnlock(pCamera->mutexId);
            PRINT(pCamera->logParam, ADTraceFlow, 
                "%s:simTask: waiting for acquire to start\n", driverName);
            status = epicsEventWait(pCamera->eventId);
            epicsMutexLock(pCamera->mutexId);
        }
        
        /* We are acquiring. */
        /* Get the current time */
        epicsTimeGetCurrent(&startTime);
        
        acquiring = ADStatusAcquire;
        ADParam->setInteger(pCamera->params, ADStatus, acquiring);
        
        /* Update the image */
        simComputeImage(pCamera);
        
        /* Get the current parameters */
        ADParam->getInteger(pCamera->params, ADImageSizeX, &imageSizeX);
        ADParam->getInteger(pCamera->params, ADImageSizeY, &imageSizeY);
        ADParam->getInteger(pCamera->params, ADImageSize,  &imageSize);
        ADParam->getInteger(pCamera->params, ADDataType,   &dataType);
        ADParam->getInteger(pCamera->params, ADAutoSave,   &autoSave);

        /* Call the imageData callback */
        PRINT(pCamera->logParam, ADTraceFlow, 
             "%s:simTask: calling imageData callback\n", driverName);
        pCamera->pImageDataCallback(pCamera->imageDataCallbackParam, 
                                    pCamera->imageBuffer,
                                    dataType, imageSizeX, imageSizeY);

        /* See if acquisition is done */
        if (pCamera->framesRemaining > 0) pCamera->framesRemaining--;
        if (pCamera->framesRemaining == 0) {
            acquiring = ADStatusIdle;
            ADParam->setInteger(pCamera->params, ADAcquire, acquiring);
            PRINT(pCamera->logParam, ADTraceFlow, 
                  "%s:simTask: acquisition completed\n", driverName);
        }
        
        /* If autosave is enabled then save the file */
        if (autoSave) simWriteFile(pCamera);
        
        /* Call the callbacks to update any changes */
        ADParam->callCallbacks(pCamera->params);
        
        ADParam->getDouble(pCamera->params, ADAcquireTime, &acquireTime);
        ADParam->getDouble(pCamera->params, ADAcquirePeriod, &acquirePeriod);
        
        /* We are done accessing data structures, release the lock */
        epicsMutexUnlock(pCamera->mutexId);
        
        /* If we are acquiring then wait for the larger of the exposure time or the exposure period,
           minus the time we have already spent computing this image. */
        if (acquiring) {
            epicsTimeGetCurrent(&endTime);
            delay = acquireTime;
            computeTime = epicsTimeDiffInSeconds(&endTime, &startTime);
            if (acquirePeriod > delay) delay = acquirePeriod;
            delay -= computeTime;
            PRINT(pCamera->logParam, ADTraceFlow, 
                  "%s:simTask: computeTime=%f, delay=%f\n",
                  driverName, computeTime, delay);            
            if (delay > 0) status = epicsEventWaitWithTimeout(pCamera->eventId, delay);
        }
    }
}

static void ADReport(int level)
{
    int i;
    DETECTOR_HDL pCamera;

    for(i=0; i<numCameras; i++) {
        pCamera = &allCameras[i];
        printf("Simulation detector %d\n", i);
        if (level > 0) {
            int nx, ny, dataType;
            ADParam->getInteger(pCamera->params, ADSizeX, &nx);
            ADParam->getInteger(pCamera->params, ADSizeY, &ny);
            ADParam->getInteger(pCamera->params, ADDataType, &dataType);
            printf("  NX, NY:            %d  %d\n", nx, ny);
            printf("  Data type:         %d\n", dataType);
        }
        if (level > 5) {
            printf("\nParameter library contents:\n");
            ADParam->dump(pCamera->params);
        }
    }   
}


static int ADInit(void)
{
    return AREA_DETECTOR_OK;
}

static DETECTOR_HDL ADOpen(int card, char * param)
{
    DETECTOR_HDL pCamera;

    if (card >= numCameras) return(NULL);
    pCamera = &allCameras[card];
    return pCamera;
}

static int ADClose(DETECTOR_HDL pCamera)
{
    return AREA_DETECTOR_OK;
}

/* Note: ADSetLog, ADFindParam, ADSetInt32Callback, ADSetFloat64Callback, 
 * and ADSetImageDataCallback can usually be used with no modifications in new drivers. */
 
static int ADSetLog( DETECTOR_HDL pCamera, ADLogFunc logFunc, void * param )
{
    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    if (logFunc == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);
    pCamera->logFunc=logFunc;
    pCamera->logParam = param;
    epicsMutexUnlock(pCamera->mutexId);
    return AREA_DETECTOR_OK;
}

static int ADFindParam( DETECTOR_HDL pCamera, const char *paramString, int *function )
{
    int i;
    int status = AREA_DETECTOR_ERROR;
    int ncommands = sizeof(DetCommands)/sizeof(DetCommands[0]);

    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    for (i=0; i < ncommands; i++) {
        if (epicsStrCaseCmp(paramString, DetCommands[i].commandString) == 0) {
            *function = DetCommands[i].command;
            status = AREA_DETECTOR_OK;
            break;
        }
    }
    if (status) 
        PRINT(pCamera->logParam, ADTraceIODriver, 
              "%s:ADFindParam: not a valid string=%s\n", 
              driverName, paramString);
    else        
        PRINT(pCamera->logParam, ADTraceIODriver, 
              "%s:ADFindParam: found value string=%s, function=%d\n",
              driverName, paramString, *function);
    return status;
}

static int ADSetInt32Callback(DETECTOR_HDL pCamera, ADInt32CallbackFunc callback, void * param)
{
    int status = AREA_DETECTOR_OK;
    
    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);
    status = ADParam->setIntCallback(pCamera->params, callback, param);
    epicsMutexUnlock(pCamera->mutexId);
    return(status);
}

static int ADSetFloat64Callback(DETECTOR_HDL pCamera, ADFloat64CallbackFunc callback, void * param)
{
    int status = AREA_DETECTOR_OK;
    
    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);
    status = ADParam->setDoubleCallback(pCamera->params, callback, param);
    epicsMutexUnlock(pCamera->mutexId);
    return(status);
}

static int ADSetStringCallback(DETECTOR_HDL pCamera, ADStringCallbackFunc callback, void * param)
{
    int status = AREA_DETECTOR_OK;
    
    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);
    status = ADParam->setStringCallback(pCamera->params, callback, param);
    epicsMutexUnlock(pCamera->mutexId);
    return(status);
}

static int ADSetImageDataCallback(DETECTOR_HDL pCamera, ADImageDataCallbackFunc callback, void * param)
{
    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);
    pCamera->pImageDataCallback = callback;
    pCamera->imageDataCallbackParam = param;
    epicsMutexUnlock(pCamera->mutexId);
    return AREA_DETECTOR_OK;
}

static int ADGetInteger(DETECTOR_HDL pCamera, int function, int * value)
{
    int status = AREA_DETECTOR_OK;
    
    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);
    
    /* We just read the current value of the parameter from the parameter library.
     * Those values are updated whenever anything could cause them to change */
    status = ADParam->getInteger(pCamera->params, function, value);
    if (status) 
        PRINT(pCamera->logParam, ADTraceError, 
              "%s:ADGetInteger error, status=%d function=%d, value=%d\n", 
              driverName, status, function, *value);
    else        
        PRINT(pCamera->logParam, ADTraceIODriver, 
              "%s:ADGetInteger: function=%d, value=%d\n", 
              driverName, function, *value);
    epicsMutexUnlock(pCamera->mutexId);
    return(status);
}

static int ADSetInteger(DETECTOR_HDL pCamera, int function, int value)
{
    int status = AREA_DETECTOR_OK;
    int reset=0;

    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);

    /* Set the parameter in the parameter library.  This may be overwritten when we read back the
     * status at the end, but that's OK */
    status |= ADParam->setInteger(pCamera->params, function, value);

    /* For a real detector this is where the parameter is sent to the hardware */
    switch (function) {
    case ADAcquire:
        if (value) {
            /* We need to set the number of frames we expect to collect, so the frame callback function
               can know when acquisition is complete.  We need to find out what mode we are in and how
               many frames have been requested.  If we are in continuous mode then set the number of
               remaining frames to -1. */
            int frameMode, numFrames;
            status |= ADParam->getInteger(pCamera->params, ADFrameMode, &frameMode);
            status |= ADParam->getInteger(pCamera->params, ADNumFrames, &numFrames);
            switch(frameMode) {
            case ADFrameSingle:
                pCamera->framesRemaining = 1;
                break;
            case ADFrameMultiple:
                pCamera->framesRemaining = numFrames;
                break;
            case ADFrameContinuous:
                pCamera->framesRemaining = -1;
                break;
            }
            reset = 1;
            /* Send an event to wake up the simulation task.  
             * It won't actually start generating new images until we release the lock below */
            epicsEventSignal(pCamera->eventId);
        } 
        break;
    case ADBinX:
    case ADBinY:
    case ADMinX:
    case ADMinY:
    case ADSizeX:
    case ADSizeY:
    case ADDataType:
        reset = 1;
        break;
    case SimResetImage:
        if (value) reset = 1;
        break;
    case ADFrameMode: 
        /* The frame mode may have changed while we are acquiring, 
         * set the frames remaining appropriately. */
        switch (value) {
        case ADFrameSingle:
            pCamera->framesRemaining = 1;
            break;
        case ADFrameMultiple: {
            int numFrames;
            ADParam->getInteger(pCamera->params, ADNumFrames, &numFrames);
            pCamera->framesRemaining = numFrames; }
            break;
        case ADFrameContinuous:
            pCamera->framesRemaining = -1;
            break;
        }
        break;
    case ADWriteFile:
        status = simWriteFile(pCamera);
        break; 
    case ADReadFile:
        status = simReadFile(pCamera);
        break; 
    }
    
    /* Reset the image if the reset flag was set above */
    if (reset) {
        status |= ADParam->setInteger(pCamera->params, SimResetImage, 1);
        /* Compute the image when parameters change.  
         * This won't post data, but will cause any parameter changes to be computed and readbacks to update.
         * Don't compute the image if this is an accquire command, since that will be done next. */
        if (function != ADAcquire) simComputeImage(pCamera);
    }
    
    /* Do callbacks so higher layers see any changes */
    ADParam->callCallbacks(pCamera->params);
    
    if (status) 
        PRINT(pCamera->logParam, ADTraceError, 
              "%s:ADSetInteger error, status=%d function=%d, value=%d\n", 
              driverName, status, function, value);
    else        
        PRINT(pCamera->logParam, ADTraceIODriver, 
              "%s:ADSetInteger: function=%d, value=%d\n", 
              driverName, function, value);
    epicsMutexUnlock(pCamera->mutexId);
    return status;
}


static int ADGetDouble(DETECTOR_HDL pCamera, int function, double * value)
{
    int status = AREA_DETECTOR_OK;
    
    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);
    /* We just read the current value of the parameter from the parameter library.
     * Those values are updated whenever anything could cause them to change */
    status = ADParam->getDouble(pCamera->params, function, value);
    if (status) 
        PRINT(pCamera->logParam, ADTraceError, 
              "%s:ADGetDouble error, status=%d function=%d, value=%f\n", 
              driverName, status, function, *value);
    else        
        PRINT(pCamera->logParam, ADTraceIODriver, 
              "%s:ADGetDouble: function=%d, value=%f\n", 
              driverName, function, *value);
    epicsMutexUnlock(pCamera->mutexId);
    return(status);
}

static int ADSetDouble(DETECTOR_HDL pCamera, int function, double value)
{
    int status = AREA_DETECTOR_OK;

    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);

    /* Set the parameter in the parameter library.  This may be overwritten when we read back the
     * status at the end, but that's OK */
    status |= ADParam->setDouble(pCamera->params, function, value);

    /* Changing any of the following parameters requires recomputing the base image */
    switch (function) {
    case ADAcquireTime:
    case ADGain:
    case SimGainX:
    case SimGainY:
        status |= ADParam->setInteger(pCamera->params, SimResetImage, 1);
        /* Compute the image.  This won't post data, but will cause any readbacks to update */
        simComputeImage(pCamera);
        break;
    }

    /* Do callbacks so higher layers see any changes */
    ADParam->callCallbacks(pCamera->params);
    if (status) 
        PRINT(pCamera->logParam, ADTraceError, 
              "%s:ADSetDouble error, status=%d function=%d, value=%f\n", 
              driverName, status, function, value);
    else        
        PRINT(pCamera->logParam, ADTraceIODriver, 
              "%s:ADSetDouble: function=%d, value=%f\n", 
              driverName, function, value);
    epicsMutexUnlock(pCamera->mutexId);
    return status;
}

static int ADGetString(DETECTOR_HDL pCamera, int function, int maxChars, char * value)
{
    int status = AREA_DETECTOR_OK;
   
    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);
    /* We just read the current value of the parameter from the parameter library.
     * Those values are updated whenever anything could cause them to change */
    status = ADParam->getString(pCamera->params, function, maxChars, value);
    if (status) 
        PRINT(pCamera->logParam, ADTraceError, 
              "%s:ADGetString error, status=%d function=%d, value=%s\n", 
              driverName, status, function, value);
    else        
        PRINT(pCamera->logParam, ADTraceIODriver, 
              "%s:ADGetString: function=%d, value=%s\n", 
              driverName, function, value);
    epicsMutexUnlock(pCamera->mutexId);
    return(status);
}

static int ADSetString(DETECTOR_HDL pCamera, int function, const char *value)
{
    int status = AREA_DETECTOR_OK;

    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);
    /* Set the parameter in the parameter library.  This may be overwritten when we read back the
     * status at the end, but that's OK */
    status |= ADParam->setString(pCamera->params, function, (char *)value);
    /* Do callbacks so higher layers see any changes */
    ADParam->callCallbacks(pCamera->params);
    if (status) 
        PRINT(pCamera->logParam, ADTraceError, 
              "%s:ADSetString error, status=%d function=%d, value=%s\n", 
              driverName, status, function, value);
    else        
        PRINT(pCamera->logParam, ADTraceIODriver, 
              "%s:ASGetString: function=%d, value=%s\n", 
              driverName, function, value);
    epicsMutexUnlock(pCamera->mutexId);
    return status;
}

static int ADGetImage(DETECTOR_HDL pCamera, int maxBytes, void *buffer)
{
    int imageSize;
    int status = AREA_DETECTOR_OK;
    
    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);
    status |= ADParam->getInteger(pCamera->params, ADImageSize, &imageSize);
    if (imageSize > maxBytes) imageSize = maxBytes;
    memcpy(buffer, pCamera->imageBuffer, imageSize);
    if (status) 
        PRINT(pCamera->logParam, ADTraceError, 
              "%s:ADGetImage error, status=%d maxBytes=%d, buffer=%p\n", 
              driverName, status, maxBytes, buffer);
    else        
        PRINT(pCamera->logParam, ADTraceIODriver, 
              "%s:ADGetImage error, maxBytes=%d, buffer=%p\n", 
              driverName, maxBytes, buffer);
    epicsMutexUnlock(pCamera->mutexId);
    return status;
}

static int ADSetImage(DETECTOR_HDL pCamera, int maxBytes, void *buffer)
{
    int status = AREA_DETECTOR_OK;
    
    if (pCamera == NULL) return AREA_DETECTOR_ERROR;
    epicsMutexLock(pCamera->mutexId);

    /* The simDetector does not allow downloading image data */    
    PRINT(pCamera->logParam, ADTraceIODriver, 
          "%s:ADSetImage not currently supported\n", driverName);
    status = AREA_DETECTOR_ERROR;
    epicsMutexUnlock(pCamera->mutexId);
    return status;
}


static int simLogMsg(void * param, const ADLogMask_t mask, const char *pFormat, ...)
{

    va_list     pvar;
    int         nchar;

    va_start(pvar, pFormat);
    nchar = vfprintf(stdout,pFormat,pvar);
    va_end (pvar);
    printf("\n");
    return(nchar);
}


int simDetectorSetup(int num_cameras)   /* number of simulated cameras in system.  */
{

    if (num_cameras < 1) {
        printf("simDetectorSetup, num_cameras must be > 0\n");
        return AREA_DETECTOR_ERROR;
    }
    numCameras = num_cameras;
    allCameras = (simDetector_t *)calloc(numCameras, sizeof(simDetector_t)); 
    return AREA_DETECTOR_OK;
}


int simDetectorConfig(int camera, int maxSizeX, int maxSizeY, int dataType)     /* Camera number */

{
    DETECTOR_HDL pCamera;
    int status = AREA_DETECTOR_OK;

    if (numCameras < 1) {
        printf("simDetectorConfig: no simDetector cameras allocated, call simDetectorSetup first\n");
        return AREA_DETECTOR_ERROR;
    }
    if ((camera < 0) || (camera >= numCameras)) {
        printf("simDetectorConfig: camera must in range 0 to %d\n", numCameras-1);
        return AREA_DETECTOR_ERROR;
    }
    pCamera = &allCameras[camera];
    pCamera->camera = camera;
    
    /* Create the epicsMutex for locking access to data structures from other threads */
    pCamera->mutexId = epicsMutexCreate();
    if (!pCamera->mutexId) {
        printf("simDetectorConfig: epicsMutexCreate failure\n");
        return AREA_DETECTOR_ERROR;
    }
    
    /* Create the epicsEvent for signaling to the simulate task when acquisition starts */
    pCamera->eventId = epicsEventCreate(epicsEventEmpty);
    if (!pCamera->eventId) {
        printf("simDetectorConfig: epicsEventCreate failure\n");
        return AREA_DETECTOR_ERROR;
    }
    
    /* Set the local log function, may be changed by higher layers */
    ADSetLog(pCamera, simLogMsg, NULL);

    /* Initialize the parameter library */
    pCamera->params = ADParam->create(0, ADLastDriverParam);
    if (!pCamera->params) {
        printf("simDetectorConfig: unable to create parameter library\n");
        return AREA_DETECTOR_ERROR;
    }
    
    /* Use the utility library to set some defaults */
    status = ADUtils->setParamDefaults(pCamera->params);
    
    /* Set some default values for parameters */
    status =  ADParam->setString (pCamera->params, ADManufacturer, "Simulated detector");
    status |= ADParam->setString (pCamera->params, ADModel, "Basic simulator");
    status |= ADParam->setInteger(pCamera->params, ADMaxSizeX, maxSizeX);
    status |= ADParam->setInteger(pCamera->params, ADMaxSizeY, maxSizeY);
    status |= ADParam->setInteger(pCamera->params, ADSizeX, maxSizeX);
    status |= ADParam->setInteger(pCamera->params, ADSizeY, maxSizeY);
    status |= ADParam->setInteger(pCamera->params, ADImageSizeX, maxSizeX);
    status |= ADParam->setInteger(pCamera->params, ADImageSizeY, maxSizeY);
    status |= ADParam->setInteger(pCamera->params, ADDataType, dataType);
    status |= ADParam->setInteger(pCamera->params, ADFrameMode, ADFrameContinuous);
    status |= ADParam->setDouble (pCamera->params, ADAcquireTime, .001);
    status |= ADParam->setDouble (pCamera->params, ADAcquirePeriod, .005);
    status |= ADParam->setInteger(pCamera->params, ADNumFrames, 100);
    status |= ADParam->setInteger(pCamera->params, SimResetImage, 1);
    status |= ADParam->setDouble (pCamera->params, SimGainX, 1);
    status |= ADParam->setDouble (pCamera->params, SimGainY, 1);
    if (status) {
        printf("simDetectorConfig: unable to set camera parameters\n");
        return AREA_DETECTOR_ERROR;
    }
    
    /* Create the thread that updates the images */
    status = (epicsThreadCreate("SimDetTask",
                                epicsThreadPriorityMedium,
                                epicsThreadGetStackSize(epicsThreadStackMedium),
                                (EPICSTHREADFUNC)simTask,
                                pCamera) == NULL);
    if (status) {
        printf("simDetectorConfig: epicsThreadCreate failure\n");
        return AREA_DETECTOR_ERROR;
    }

    /* Compute the first image */
    simComputeImage(pCamera);
    
    return AREA_DETECTOR_OK;
}

/* Code for iocsh registration */

/* simDetectorSetup */
static const iocshArg simDetectorSetupArg0 = {"Number of simulated detectors", iocshArgInt};
static const iocshArg * const simDetectorSetupArgs[1] =  {&simDetectorSetupArg0};
static const iocshFuncDef setupsimDetector = {"simDetectorSetup", 1, simDetectorSetupArgs};
static void setupsimDetectorCallFunc(const iocshArgBuf *args)
{
    simDetectorSetup(args[0].ival);
}


/* simDetectorConfig */
static const iocshArg simDetectorConfigArg0 = {"Camera # being configured", iocshArgInt};
static const iocshArg simDetectorConfigArg1 = {"Max X size", iocshArgInt};
static const iocshArg simDetectorConfigArg2 = {"Max Y size", iocshArgInt};
static const iocshArg simDetectorConfigArg3 = {"Data type", iocshArgInt};
static const iocshArg * const simDetectorConfigArgs[4] = {&simDetectorConfigArg0,
                                                          &simDetectorConfigArg1,
                                                          &simDetectorConfigArg2,
                                                          &simDetectorConfigArg3};
static const iocshFuncDef configsimDetector = {"simDetectorConfig", 4, simDetectorConfigArgs};
static void configsimDetectorCallFunc(const iocshArgBuf *args)
{
    simDetectorConfig(args[0].ival, args[1].ival, args[2].ival, args[3].ival);
}


static void simDetectorRegister(void)
{

    iocshRegister(&setupsimDetector,  setupsimDetectorCallFunc);
    iocshRegister(&configsimDetector, configsimDetectorCallFunc);
}

epicsExportRegistrar(simDetectorRegister);
