/*
 * i2s_mic.c
 *
 *  Created on: Jun 24, 2022
 *      Author: ma
 */
/*********************************************************************
 * INCLUDES
 */

#include <string.h>
#include <assert.h>

#include <xdc/runtime/Types.h>
#include <xdc/runtime/Error.h>
#include <xdc/runtime/Log.h>
#include <xdc/runtime/System.h>
#include <xdc/runtime/Timestamp.h>

#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/gates/GateSwi.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Event.h>

/* Driver Header files */
#include <ti/drivers/GPIO.h>
#include <ti/drivers/I2S.h>

#include <ti/drivers/UART.h>
#include <ti/drivers/uart/UARTCC26XX.h>

#include <ti/drivers/NVS.h>
#include <ti/drivers/nvs/NVSSPI25X.h>

#include <board.h>

#include "simple_gatt_profile.h"

#include "audio.h"


/*********************************************************************
 *
 * How data stored
 *
 * First, the last 16 sectors are reserved. Only DATA_SECT_COUNT sectors
 * are used for storing adpcm data (and per sector metadata).
 *
 * There is a strictly incremental MONOTONIC_COUNTER implemented using
 * "bit-creeping" trick and consumes last two sectors. This counter records
 * how much data sectors have been written.
 *
 * Since the data sectors are used as cyclic array, it is possible the
 * sector index is larger than DATA_SECT_COUNT. So each sector saves its
 * sector index (currSect) in metadata.
 *
 * Each sector stores exactly 4000 bytes in adpcm format, which counts for
 * 8000 samples.
 *
 * Each sector also has a 96-byte header, consisting of 21 sect indices,
 * a magic number (4 bytes), a currSect (4 bytes), and an adpcm state (4 bytes).
 * The last sect index is nextSect, which is technically redundant. But
 * this is convenient for data storing and retrieving. The 21 sect indices
 * are interpretted as 20 segments [start, end).
 *
 */

/*********************************************************************
 * MACROS
 */

#define LOG_ADPCM_DATA
// #define LOG_PCM_DATA

#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

#define interval_of(type, start, end)                           \
        (offsetof(type, end) - offsetof(type, start))

/*********************************************************************
 * CONSTANTS
 */
#define MAGIC                             (0xD970576A)

#define PREAMBLE                          ((uint64_t)0x7FFF80017FFF8001)

// Task configuration
#define MC_TASK_PRIORITY                  1

#ifndef MC_TASK_STACK_SIZE
#define MC_TASK_STACK_SIZE                1024
#endif

/* The higher the sampling frequency, the less time we have to process the data, but the higher the sound quality. */
#define SAMPLE_RATE                       16000   /* Supported values: 8kHz, 16kHz, 32kHz and 44.1kHz */

#define AUDIO_PCM_EVT                     Event_Id_00
#define AUDIO_START_REC                   Event_Id_01
#define AUDIO_STOP_REC                    Event_Id_02
#define AUDIO_READ_EVT                    Event_Id_03
#define AUDIO_EVENTS                      (AUDIO_PCM_EVT | AUDIO_START_REC | AUDIO_STOP_REC | AUDIO_READ_EVT )

#define FLASH_SIZE                        nvsAttrs.regionSize
#define SECT_SIZE                         nvsAttrs.sectorSize
#define SECT_COUNT                        (FLASH_SIZE / SECT_SIZE)
#define HISECT_INDEX                      (SECT_COUNT - 1)
#define HISECT_OFFSET                     (HISECT_INDEX * SECT_SIZE)
#define MAGIC_OFFSET                      (HISECT_OFFSET + 2048)
#define LOSECT_INDEX                      (SECT_COUNT - 2)
#define LOSECT_OFFSET                     (LOSECT_INDEX * SECT_SIZE)
#define DATA_SECT_COUNT                   (SECT_COUNT - 16)

#define SECT_OFFSET(index)                (index * SECT_SIZE)

/*
 * monotonic counter is used to record sectors used.
 */
#define MONOTONIC_COUNTER                 ((((uint32_t)(markedBitsHi >> 1) - 1) << 15) + (uint32_t)(markedBitsLo - 1))

/*********************************************************************
 * TYPEDEFS
 */
#define ADPCMBUF_SIZE                     40
#define ADPCMBUF_NUM                      4
#define PCMBUF_SIZE                       (ADPCMBUF_SIZE * 4)
#define PCM_SAMPLES_PER_BUF               (PCMBUF_SIZE / sizeof(int16_t))
#define PCMBUF_NUM                        4
#define PCMBUF_TOTAL_SIZE                 (PCMBUF_SIZE * PCMBUF_NUM)

#define ADPCM_SIZE_PER_SECT               4000
#define ADPCM_BUF_COUNT_PER_SECT          (ADPCM_SIZE_PER_SECT / ADPCMBUF_SIZE)

#define SEGMENT_NUM                       20

typedef struct __attribute__ ((__packed__)) AdpcmState
{
  int16_t sample;
  uint8_t index;
  uint8_t dummy;
} AdpcmState_t;

_Static_assert(sizeof(AdpcmState_t)==4, "wrong size of adpcm state");

#define NUM_PREV_STARTS                   21
#define NUM_TOTAL_STARTS                  22

typedef struct WriteContext
{
  uint32_t prevStarts[NUM_PREV_STARTS];
  uint32_t currStart;
  uint32_t currPos;
  AdpcmState_t sectAdpcmState;  // sector-wise adpcm state
  uint8_t adpcmBuf[ADPCMBUF_NUM][ADPCMBUF_SIZE];

  uint8_t pcmBuf[PCMBUF_TOTAL_SIZE];

  AdpcmState_t adpcmState;
  uint32_t adpcmCount;
  uint32_t adpcmCountInSect;

  I2S_Transaction i2sTransaction[PCMBUF_NUM];

  List_List recordingList;
  List_List processingList;

  bool finished;
} WriteContext_t;

_Static_assert(offsetof(WriteContext_t, pcmBuf)==256, "wrong write context (header) size");

#ifdef LOG_ADPCM_DATA
typedef struct __attribute__ ((__packed__)) UartPacket
{
  uint64_t preamble;    // 0
  uint32_t startSect;   // 8
  uint32_t index;       // 12
  int16_t prevSample;   // 16
  uint8_t prevIndex;    // 18
  uint8_t dummy;        // 19
  uint8_t adpcm[ADPCMBUF_SIZE];
#ifdef LOG_PCM_DATA
  uint8_t pcm[PCMBUF_SIZE];
#endif
  uint8_t cka;
  uint8_t ckb;
} UartPacket_t;

#ifdef LOG_PCM_DATA
_Static_assert(sizeof(UartPacket_t)== ADPCMBUF_SIZE + PCMBUF_SIZE + 22,
               "wrong uart packet size"); //1022
#else
_Static_assert(sizeof(UartPacket_t)== ADPCMBUF_SIZE + 22,
               "wrong uart packet size");  // 222
#endif
#endif

/*********************************************************************
 * GLOBAL VARIABLES
 */

// Task configuration
Task_Struct mcTask;
#if defined __TI_COMPILER_VERSION__
#pragma DATA_ALIGN(mcTaskStack, 8)
#else
#pragma data_alignment=8
#endif
uint8_t mcTaskStack[MC_TASK_STACK_SIZE];

void (*audioMoreDataAvailable)(void) = NULL;

/*********************************************************************
 * LOCAL VARIABLES
 */
/* Table of index changes */
const static signed char IndexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1,
                                            -1, -1, 2, 4, 6, 8, };

/* Quantizer step size lookup table */
const static int StepSizeTable[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19,
                                       21, 23, 25, 28, 31, 34, 37, 41, 45, 50,
                                       55, 60, 66, 73, 80, 88, 97, 107, 118,
                                       130, 143, 157, 173, 190, 209, 230, 253,
                                       279, 307, 337, 371, 408, 449, 494, 544,
                                       598, 658, 724, 796, 876, 963, 1060, 1166,
                                       1282, 1411, 1552, 1707, 1878, 2066, 2272,
                                       2499, 2749, 3024, 3327, 3660, 4026, 4428,
                                       4871, 5358, 5894, 6484, 7132, 7845, 8630,
                                       9493, 10442, 11487, 12635, 13899, 15289,
                                       16818, 18500, 20350, 22385, 24623, 27086,
                                       29794, 32767 };

/* Used for calculating bitmap for monotonic counter */
static const uint8_t markedBytes[8] = { 0x7f, 0x3f, 0x1f, 0x0f, 0x07, 0x03,
                                        0x01, 0x00 };

/*
 * Synchronization
 */
static Event_Handle audioEvent = NULL;
static Semaphore_Handle semDataReadyForTreatment = NULL;

#if defined (LOG_ADPCM_DATA)
static Semaphore_Handle semUartTxReady = NULL;
#endif

/*
 * Device driver handles and attributes
 */
UART_Handle uartHandle;
I2S_Handle i2sHandle;
NVS_Handle nvsHandle;
NVS_Attrs nvsAttrs;

/*
 * markedBitsLo and markedBitsHi are 'root' counters recording how many sectors have been used in
 * ringbuffer.
 * 1. they are loaded in readCounter() during initialization.
 * 2. they are updated right after the last record is written into current sector.
 */
int markedBitsLo = -1;
int markedBitsHi = -1;

/*
 * segments
 */
uint32_t segments[SEGMENT_NUM + 1] = { [0 ... SEGMENT_NUM]=(uint32_t) -1 };

WriteContext_t writeContext = { };
WriteContext_t *wctx = NULL;

#if defined (LOG_ADPCM_DATA)
UartPacket_t uartPkt;
#endif

/*********************************************************************
 * LOCAL FUNCTIONS
 */

static void Audio_init(void);
static void Audio_taskFxn(UArg a0, UArg a1);

static void errCallbackFxn(I2S_Handle handle, int_fast16_t status,
                           I2S_Transaction *transactionPtr);
static void readCallbackFxn(I2S_Handle handle, int_fast16_t status,
                            I2S_Transaction *transactionPtr);

#if defined (LOG_ADPCM_DATA)
static void uartReadCallbackFxn(UART_Handle handle, void *buf, size_t count);
static void uartWriteCallbackFxn(UART_Handle handle, void *buf, size_t count);
#endif

static char adpcmEncoder(short sample, int16_t *prevSample, uint8_t *prevIndex);
void checksum(void *p, uint32_t len, uint8_t *a, uint8_t *b);

extern void simple_peripheral_spin(void);

static int countMarkedBits(int sectIndex, size_t size);
static void incrementMarkedBits(int sectIndex, size_t current);
static void resetLowCounter(void);
static void resetCounter(void);
static uint32_t readMagic(void);
static void loadCounter(void);
static void incrementCounter(void);

static void loadPrevStarts(void);
// static void updateSegments(void);

static void startRecording(void);
static void stopRecording(void);

void Audio_startRecording(void)
{

}

void Audio_stopRecording(void)
{

}

/*********************************************************************
 * @fn      Audio_read
 *
 * @brief   Task creation function for the Simple Peripheral.
 */
void Audio_read()
{
  readMessage.status = RM_REQUESTED;
  Event_post(audioEvent, AUDIO_READ_EVT);
}

/*********************************************************************
 * @fn      I2sMic_createTask
 *
 * @brief   Task creation function for the I2s Mic.
 */
void Audio_createTask(void)
{
  Task_Params taskParams;

  // Configure task
  Task_Params_init(&taskParams);
  taskParams.stack = mcTaskStack;
  taskParams.stackSize = MC_TASK_STACK_SIZE;
  taskParams.priority = MC_TASK_PRIORITY;

  Task_construct(&mcTask, Audio_taskFxn, &taskParams, NULL);
}

/*********************************************************************
 * @fn      Audio_init
 *
 * @brief   Called during initialization and contains application
 *          specific initialization (ie. hardware initialization/setup,
 *          table initialization, power up notification, etc), and
 *          profile initialization/setup.
 */
static void Audio_init(void)
{
  Semaphore_Params semParams;

  audioEvent = Event_create(NULL, Error_IGNORE);

  Semaphore_Params_init(&semParams);
  semParams.event = audioEvent;
  semParams.eventId = AUDIO_PCM_EVT;
  semDataReadyForTreatment = Semaphore_create(0, &semParams, Error_IGNORE);

#if defined (LOG_ADPCM_DATA)
  Semaphore_Params_init(&semParams);
//  semParams.event = audioEvent;
//  semParams.eventId = UART_TX_RDY_EVT;
  semParams.mode = Semaphore_Mode_BINARY;
  semUartTxReady = Semaphore_create(1, &semParams, Error_IGNORE);
  UART_init();
#endif

  NVS_Params nvsParams;
  NVS_Params_init(&nvsParams);
  nvsHandle = NVS_open(Board_NVSEXTERNAL, &nvsParams);
  NVS_getAttrs(nvsHandle, &nvsAttrs);

  I2S_init();
}

/*********************************************************************
 * @fn      I2sMic_taskFxn
 *
 * @brief   Application task entry point for the I2s Mic.
 *
 * @param   a0, a1 - not used.
 */
static void Audio_taskFxn(UArg a0, UArg a1)
{
  Types_FreqHz freq;
  Timestamp_getFreq(&freq);

  Audio_init();
  loadCounter();
  loadPrevStarts();
  Event_post(audioEvent, AUDIO_START_REC);

  for (int loop = 0;; loop++)
  {
    uint32_t event = Event_pend(audioEvent, NULL, AUDIO_EVENTS,
                                BIOS_WAIT_FOREVER);
    if (event & AUDIO_START_REC)
    {
      startRecording();
    }

    if (event & AUDIO_STOP_REC)
    {
      stopRecording();
    }

    if (event & AUDIO_PCM_EVT)
    {
      Semaphore_pend(semDataReadyForTreatment, BIOS_NO_WAIT);

      if (!wctx->finished)
      {
        I2S_Transaction *ttt = (I2S_Transaction*) List_get(&wctx->processingList);
        if (ttt != NULL)
        {

  #ifdef LOG_ADPCM_DATA
          /* save a copy */
          int16_t uartPrevSample = wctx->adpcmState.sample;
          uint8_t uartPrevIndex = wctx->adpcmState.index;
  #endif

          uint16_t adpcmInUse = wctx->adpcmCount % ADPCMBUF_NUM;

          int16_t *samples = (int16_t*) ttt->bufPtr;
          for (int i = 0; i < PCM_SAMPLES_PER_BUF; i++)
          {
            uint8_t code = adpcmEncoder(samples[i], &wctx->adpcmState.sample,
                                        &wctx->adpcmState.index);
            if (i % 2 == 0)
            {
              wctx->adpcmBuf[adpcmInUse][i / 2] = code;
            }
            else
            {
              wctx->adpcmBuf[adpcmInUse][i / 2] |= (code << 4);
            }
          }

  #ifdef LOG_ADPCM_DATA
          Semaphore_pend(semUartTxReady, BIOS_WAIT_FOREVER);
          uartPkt.preamble = PREAMBLE;
          uartPkt.startSect = wctx->currStart;
          uartPkt.index = wctx->adpcmCount;
          uartPkt.prevSample = uartPrevSample;
          uartPkt.prevIndex = uartPrevIndex;
  #ifdef LOG_PCM_DATA
          uartPkt.dummy = 1;
          memcpy(uartPkt.pcm, ttt->bufPtr, PCMBUF_SIZE);
  #else
          uartPkt.dummy = 0;
  #endif
          memcpy(uartPkt.adpcm, wctx->adpcmBuf[adpcmInUse], ADPCMBUF_SIZE);
          checksum(&uartPkt.startSect,
          offsetof(UartPacket_t, cka) - offsetof(UartPacket_t, startSect),
                   &uartPkt.cka, &uartPkt.ckb);
          UART_write(uartHandle, &uartPkt, sizeof(uartPkt));
  #endif

          List_put(&wctx->recordingList, (List_Elem*) ttt);

          if (adpcmInUse == ADPCMBUF_NUM - 1)
          {
            if (wctx->adpcmCountInSect == adpcmInUse)
            {
              // uint32_t bufCount = 0;
              size_t offset = (wctx->currPos % DATA_SECT_COUNT) * SECT_SIZE;
              NVS_write(nvsHandle, offset, wctx, 96 + ADPCMBUF_SIZE * ADPCMBUF_NUM, 0);
            }
            else
            {
              uint32_t writtenBufCount = wctx->adpcmCountInSect % ADPCMBUF_NUM * ADPCMBUF_NUM;
              size_t offset = (wctx->currPos % DATA_SECT_COUNT) * SECT_SIZE
                  + 96 + writtenBufCount * ADPCMBUF_SIZE;
              NVS_write(nvsHandle, offset, &wctx->adpcmBuf[0],
                        ADPCMBUF_SIZE * ADPCMBUF_NUM, 0); // NVS_WRITE_POST_VERIFY);
            }
          }

          wctx->adpcmCount++;
          wctx->adpcmCountInSect = wctx->adpcmCount % ADPCM_BUF_COUNT_PER_SECT;

          // last adpcm buf in sect
          if (wctx->adpcmCountInSect == 0)
          {
            wctx->currPos++;
            wctx->sectAdpcmState = wctx->adpcmState;
            incrementCounter();
            /* it is important to do this here */
            NVS_erase(nvsHandle, (wctx->currPos % DATA_SECT_COUNT) * SECT_SIZE,
                      SECT_SIZE);
          }
        } /* end of if ttt != NULL */
      } /* end of if wctx */
    } /* end of AUDIO PCM EVENT */

    if (event & AUDIO_READ_EVT)
    {
      readMessage.status = RM_PROCESSING_REQUEST;

      uint32_t counter = MONOTONIC_COUNTER;
      if (counter >= DATA_SECT_COUNT && readMessage.major <= counter - DATA_SECT_COUNT)
      {
        readMessage.major = counter - DATA_SECT_COUNT + 1;
      }

      if (readMessage.major < counter)
      {
        if (readMessage.minor == 0)
        {
          uint8_t head[12];
          size_t offset = readMessage.major % DATA_SECT_COUNT + ADPCM_BUF_COUNT_PER_SECT * ADPCMBUF_SIZE;
          NVS_read(nvsHandle, offset, head, sizeof(head));
          readMessage.buf[0] = head[4];
          readMessage.buf[1] = head[5];
          readMessage.buf[2] = head[6];
          readMessage.buf[3] = head[7];
          readMessage.buf[4] = head[8];
          readMessage.buf[5] = head[9];
          readMessage.buf[6] = head[10];
          readMessage.buf[7] = head[11];
          readMessage.buf[8] = head[0];
          readMessage.buf[9] = head[1];
          readMessage.buf[10] = head[2];
          readMessage.buf[11] = head[3];
          offset = readMessage.major % DATA_SECT_COUNT;
          NVS_read(nvsHandle, offset, &readMessage.buf[12], 236 - 12);
          readMessage.readLen = 236;
        }
        else
        {
          size_t offset = readMessage.major % DATA_SECT_COUNT + 224 + readMessage.minor * 236;
          NVS_read(nvsHandle, offset, readMessage.buf, 236);
          readMessage.readLen = 236;
        }

        Audio_readDone();
      }
    }

    if (loop > 5000)
    {
      while (1)
      {
        Task_sleep(1000 / Clock_tickPeriod);
      }
    }
  } /* end of loop */
}

static void startRecording(void)
{
  simpleProfileChar2 = 1;

  wctx = &writeContext; // is here the right place? TODO

  wctx->currStart = MONOTONIC_COUNTER;
  wctx->currPos = wctx->currStart;
  wctx->adpcmState.sample = 0;
  wctx->adpcmState.index = 0;
  wctx->adpcmState.dummy = 0;
  wctx->sectAdpcmState = wctx->adpcmState;
  wctx->adpcmCount = 0;
  wctx->adpcmCountInSect = 0;

  wctx->finished = false;

  NVS_erase(nvsHandle, (wctx->currPos % DATA_SECT_COUNT) * SECT_SIZE,
            SECT_SIZE);

  Task_sleep(10 * 1000 / Clock_tickPeriod);

  List_clearList(&wctx->recordingList);
  List_clearList(&wctx->processingList);

  for (int k = 0; k < PCMBUF_NUM; k++)
  {
    I2S_Transaction_init(&wctx->i2sTransaction[k]);
    wctx->i2sTransaction[k].bufPtr = &wctx->pcmBuf[k * PCMBUF_SIZE];
    wctx->i2sTransaction[k].bufSize = PCMBUF_SIZE;
    List_put(&wctx->recordingList, (List_Elem*) &wctx->i2sTransaction[k]);
  }

#if defined(LOG_ADPCM_DATA)

  if (uartHandle == NULL)
  {
    UART_Params uartParams;
    UART_Params_init(&uartParams);
    uartParams.baudRate = 1500000; // 115200 * 8;
    uartParams.readMode = UART_MODE_CALLBACK;
    uartParams.readDataMode = UART_DATA_TEXT;
    uartParams.readReturnMode = UART_RETURN_NEWLINE;
    uartParams.readCallback = uartReadCallbackFxn;
    uartParams.readEcho = UART_ECHO_OFF;
    uartParams.writeMode = UART_MODE_CALLBACK;
    uartParams.writeDataMode = UART_DATA_BINARY;
    uartParams.writeCallback = uartWriteCallbackFxn;
    uartHandle = UART_open(Board_UART0, &uartParams);
  }
#endif

  /*
   * In board file, I2S_SELECT must be HIGH.
   *
   * invertWS   SD1Channels             data
   * false      I2S_CHANNELS_STEREO     xxxx, 0, xxxx, 0, ....
   * true       I2S_CHANNELS_STEREO     0, xxxx, 0, xxxx, ....
   * false      I2S_CHANNELS_MONO       xxxx, xxxx, xxxx, .... (left channel only)
   * false      I2S_CHANNELS_MONO_INV   0, 0, 0, 0, ....
   * true       I2S_CHANNELS_MONO       0, 0, 0, 0, ....
   * true       I2S_CHANNELS_MONO_INV   xxxx, xxxx, xxxx, .... (right channel only)
   *
   * The last one is used.
   */
  I2S_Params i2sParams;
  I2S_Params_init(&i2sParams);
  i2sParams.trueI2sFormat = false;
  i2sParams.invertWS = true;
  i2sParams.isMSBFirst = true;
  i2sParams.isDMAUnused = true;
  i2sParams.memorySlotLength = I2S_MEMORY_LENGTH_16BITS;
  i2sParams.beforeWordPadding = 2;
  i2sParams.afterWordPadding = 6;
  i2sParams.bitsPerWord = 24;
  i2sParams.moduleRole = I2S_MASTER;
  i2sParams.samplingEdge = I2S_SAMPLING_EDGE_FALLING;
  i2sParams.SD0Use = I2S_SD0_INPUT;
  i2sParams.SD1Use = I2S_SD1_DISABLED;
  i2sParams.SD0Channels = I2S_CHANNELS_MONO_INV;
  i2sParams.SD1Channels = I2S_CHANNELS_NONE;
  i2sParams.phaseType = I2S_PHASE_TYPE_DUAL;
  i2sParams.fixedBufferLength = PCMBUF_SIZE;
  i2sParams.startUpDelay = 0;
  i2sParams.MCLKDivider = 2;
  i2sParams.samplingFrequency = SAMPLE_RATE;
  i2sParams.readCallback = readCallbackFxn;
  i2sParams.writeCallback = NULL;
  i2sParams.errorCallback = errCallbackFxn;

  i2sHandle = I2S_open(Board_I2S0, &i2sParams);
  I2S_setReadQueueHead(i2sHandle,
                       (I2S_Transaction*) List_head(&wctx->recordingList));

  /*
   * Start I2S streaming
   */
  I2S_startClocks(i2sHandle);
  I2S_startRead(i2sHandle);
}

static void stopRecording(void)
{
  if (i2sHandle)
  {
    I2S_stopRead(i2sHandle);
    I2S_stopClocks(i2sHandle);
    I2S_close(i2sHandle);
  }

  for (int i = 0; i < NUM_PREV_STARTS; i++)
  {
    wctx->prevStarts[i] = wctx->prevStarts[i+1];
  }
  wctx->currStart = wctx->currPos;
  wctx->finished = true;

  simpleProfileChar2 = 0;
}

static void errCallbackFxn(I2S_Handle handle, int_fast16_t status,
                           I2S_Transaction *transactionPtr)
{
  /*
   * The content of this callback is executed if an I2S error occurs
   */
  I2S_stopClocks(handle);
  I2S_close(handle);
}

static void readCallbackFxn(I2S_Handle handle, int_fast16_t status,
                            I2S_Transaction *transactionPtr)
{
  /*
   * The content of this callback is executed every time a read-transaction is started
   */

  /* We must consider the previous transaction (the current one is not over) */
  I2S_Transaction *transactionFinished = (I2S_Transaction*) List_prev(
      &transactionPtr->queueElement);

  if (transactionFinished != NULL)
  {

    /* The finished transaction contains data that must be treated */
    List_remove(&wctx->recordingList, (List_Elem*) transactionFinished);
    List_put(&wctx->processingList, (List_Elem*) transactionFinished);

    /* Start the treatment of the data */
    Semaphore_post(semDataReadyForTreatment);
  }
}

#if defined (LOG_ADPCM_DATA)
static void uartReadCallbackFxn(UART_Handle handle, void *buf, size_t count)
{
}

static void uartWriteCallbackFxn(UART_Handle handle, void *buf, size_t count)
{
  Semaphore_post(semUartTxReady);
}
#endif

static char adpcmEncoder(short sample, int16_t *prevSample, uint8_t *prevIndex)
{
  int code; /* ADPCM output value */
  int diff; /* Difference between sample and the predicted sample */
  int step; /* Quantizer step size */
  int predSample; /* Output of ADPCM predictor */
  int diffq; /* Dequantized predicted difference */
  int index; /* Index into step size table */

  /* Restore previous values of predicted sample and quantizer step size index */
  predSample = (int) (*prevSample);
  index = *prevIndex;
  step = StepSizeTable[index];

  /* Compute the difference between the acutal sample (sample) and the
   * the predicted sample (predsample)
   */
  diff = sample - predSample;
  if (diff >= 0)
    code = 0;
  else
  {
    code = 8;
    diff = -diff;
  }
  /* Quantize the difference into the 4-bit ADPCM code using the
   * the quantizer step size
   */
  /* Inverse quantize the ADPCM code into a predicted difference
   * using the quantizer step size
   */
  diffq = step >> 3;
  if (diff >= step)
  {
    code |= 4;
    diff -= step;
    diffq += step;
  }
  step >>= 1;
  if (diff >= step)
  {
    code |= 2;
    diff -= step;
    diffq += step;
  }
  step >>= 1;
  if (diff >= step)
  {
    code |= 1;
    diffq += step;
  }
  /* Fixed predictor computes new predicted sample by adding the
   * old predicted sample to predicted difference
   */
  if (code & 8)
    predSample -= diffq;
  else
    predSample += diffq;
  /* Check for overflow of the new predicted sample */
  if (predSample > 32767)
    predSample = 32767;
  else if (predSample < -32767)
    predSample = -32767;
  /* Find new quantizer stepsize index by adding the old index
   * to a table lookup using the ADPCM code
   */
  index += IndexTable[code];
  /* Check for overflow of the new quantizer step size index */
  if (index < 0)
    index = 0;
  if (index > 88)
    index = 88;
  /* Save the predicted sample and quantizer step size index for next iteration */
  *prevSample = (short) predSample;
  *prevIndex = index;

  /* Return the new ADPCM code */
  return (code & 0x0f);
}

/*
 * Calculate checksum, ublox
 */
void checksum(void *p, uint32_t len, uint8_t *a, uint8_t *b)
{
  uint8_t *arr = (uint8_t*) p;
  *a = 0;
  *b = 0;
  for (int i = 0; i < len; i++)
  {
    *a += arr[i];
    *b += *a;
  }
}

/*
 * Count marked (cleared) bits in given sector and size
 * size is 4096 for low counter and 2048 for high counter
 */
static int countMarkedBits(int sectIndex, size_t size)
{
  uint8_t buf[256];
  bool countComplete = false;
  int count = 0;

  for (int i = 0; i < size / 256; i++)
  {
    NVS_read(nvsHandle, SECT_OFFSET(sectIndex) + i * 256, buf, 256);
    for (int j = 0; j < 256; j++)
    {
      if (countComplete)
      {
        if (buf[j] != 0xff)
          return -1;
      }
      else
      {
        if (buf[j] == 0)
        {
          count += 8;
        }
        else
        {
          switch (buf[j])
          {
          // @formatter:off
          case 0x01: count += 7; break;
          case 0x03: count += 6; break;
          case 0x07: count += 5; break;
          case 0x0f: count += 4; break;
          case 0x1f: count += 3; break;
          case 0x3f: count += 2; break;
          case 0x7f: count += 1; break;
          case 0xff: count += 0; break;
          default: return -1;
          // @formatter:on
}          countComplete = true;
        }
      }
    }
  }
  return count;
}

/*
 * clear one more bit on given sector, and position
 */
static void incrementMarkedBits(int sectIndex, size_t current)
{
  int offset = SECT_OFFSET(sectIndex) + current / 8;
  NVS_write(nvsHandle, offset, (void*) &markedBytes[current % 8], 1,
  NVS_WRITE_POST_VERIFY);
}

static uint32_t readMagic(void)
{
  uint32_t magic;
  NVS_read(nvsHandle, HISECT_OFFSET + 2048, &magic, sizeof(uint32_t));
  return magic;
}

static void resetLowCounter(void)
{
  NVS_erase(nvsHandle, LOSECT_OFFSET, SECT_SIZE);
  markedBitsLo = 0;
  incrementMarkedBits(LOSECT_INDEX, markedBitsLo++);
}

static void resetCounter(void)
{
  // erase high sect
  NVS_erase(nvsHandle, HISECT_OFFSET, SECT_SIZE);

  // erase and init low counter
  resetLowCounter();

  // init high counter
  markedBitsHi = 0;
  incrementMarkedBits(HISECT_INDEX, markedBitsHi++);
  incrementMarkedBits(HISECT_INDEX, markedBitsHi++);  // twice

  // write magic
  uint32_t magic = MAGIC;
  NVS_write(nvsHandle, HISECT_OFFSET + 2048, &magic, sizeof(magic),
  NVS_WRITE_POST_VERIFY);
}

static void loadCounter(void)
{
  static bool initialized = false;
  if (!initialized)
  {
    if (MAGIC != readMagic())
    {
      resetCounter();
      return;
    }

    markedBitsHi = countMarkedBits(HISECT_INDEX, 2048);
    if (markedBitsHi <= 1)
    {
      resetCounter();
      return;
    }

    if (markedBitsHi % 2 == 1) // unfinished carrying bit
    {
      resetLowCounter();
      incrementMarkedBits(HISECT_INDEX, markedBitsHi++);
    }

    markedBitsLo = countMarkedBits(LOSECT_INDEX, 4096);
    initialized = true;
  }
}

/*
 * increment counter including flip
 */
static void incrementCounter(void)
{
  if (markedBitsLo < SECT_SIZE * 8)
  {
    incrementMarkedBits(LOSECT_INDEX, markedBitsLo++);
  }
  else
  {
    incrementMarkedBits(HISECT_INDEX, markedBitsHi++);
    resetLowCounter();
    incrementMarkedBits(HISECT_INDEX, markedBitsHi++);
  }
}

static void loadPrevStarts(void)
{
  uint32_t counter = MONOTONIC_COUNTER;
  if (counter == 0)
  {
    for (int i = 0; i < NUM_PREV_STARTS; i++)
    {
      writeContext.prevStarts[i] = 0xFFFFFFFF;
    }
  }
  else
  {
    // skip earlist one
    size_t offset = (counter - 1) % DATA_SECT_COUNT * SECT_SIZE + 4;
    NVS_read(nvsHandle, offset, &writeContext,
             sizeof(uint32_t) * NUM_PREV_STARTS);
  }

  writeContext.currStart = counter;
  writeContext.currPos = counter;
}

//static void updateSegments(void)
//{
//  if (segments[0] == (uint32_t) -1) // uninitialized
//  {
//    segments[1] = wctx->currStart;
//    segments[0] = wctx->currPos + 1;
//  }
//  else
//  {
//    if (segments[0] == wctx->currStart)
//    {  // first sector
//      for (int i = SEGMENT_NUM; i > 0; i--)
//      {
//        segments[i] = segments[i - 1];
//      }
//    }
//    segments[0] = wctx->currPos + 1;
//  }
//}
