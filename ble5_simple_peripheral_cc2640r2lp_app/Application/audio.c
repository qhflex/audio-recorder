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

#include <xdc/runtime/Error.h>
#include <xdc/runtime/Log.h>
#include <xdc/runtime/System.h>

#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/gates/GateSwi.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Mailbox.h>
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

#include "audio.h"

/*********************************************************************
 *
 * Notification
 *
 * 1. User starts char notification.
 * 2. SimplePeripheral -> Audio_play(), this will set maxPlayingListSize
 *    and post an event.
 * 3. noticing that ...
 */

/*********************************************************************
 * MACROS
 */

// #define LOG_PCM_PACKET
// #define LOG_PCM_SUPPORT_QOS
// #define LOG_ADPCM_PACKET
// #define LOG_ADPCM_SUPPORT_QOS
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

#define interval_of(type, start, end)                           \
        (offsetof(type, end) - offsetof(type, start))

/*********************************************************************
 * CONSTANTS
 */
#define MAGIC                             (0xD970576A)

#define ADPCM_FORMAT                      ((uint32_t)0)
#define PCM_FORMAT                        ((uint32_t)1)

#define PREAMBLE                          ((uint64_t)0x7FFF80017FFF8001)

#define PACKET_TYPE_PCM                   0x01
#define PACKET_TYPE_ADPCM                 0x00

// Task configuration
#define MC_TASK_PRIORITY                  1

#ifndef MC_TASK_STACK_SIZE
#define MC_TASK_STACK_SIZE                1024
#endif

/* The higher the sampling frequency, the less time we have to process the data, but the higher the sound quality. */
#define SAMPLE_RATE                       8000   /* Supported values: 8kHz, 16kHz, 32kHz and 44.1kHz */

/* The more storage space we have, the more delay we have, but the more time we have to process the data. */
#define NUM_RECORD_PKT                    4
#define NUM_UART_PKT                      1
#define NUM_PLAY_PKT                      2

#define AUDIO_PCM_EVT                     Event_Id_00
#define UART_TX_RDY_EVT                   Event_Id_01
#define AUDIO_EVENTS                      (AUDIO_PCM_EVT)

#define FLASH_SIZE                        nvsAttrs.regionSize
#define SECT_SIZE                         nvsAttrs.sectorSize
#define SECT_COUNT                        (FLASH_SIZE / SECT_SIZE)
#define HISECT_INDEX                      (SECT_COUNT - 1)
#define HISECT_OFFSET                     (HISECT_INDEX * SECT_SIZE)
#define MAGIC_OFFSET                      (HISECT_OFFSET + 2048)
#define LOSECT_INDEX                      (SECT_COUNT - 2)
#define LOSECT_OFFSET                     (LOSECT_INDEX * SECT_SIZE)
#define DATA_SECT_COUNT                   (SECT_COUNT - 2)

#define SECT_OFFSET(index)                (index * SECT_SIZE)

/*
 * monotonic counter is used to record sectors used.
 */
#define MONOTONIC_COUNTER                 ((((uint32_t)(markedBitsHi >> 1) - 1) << 15) + (uint32_t)(markedBitsLo - 1))

#define BLOCK_SIZE                        256
#define BLOCKS_PER_SECT                   (SECT_SIZE / BLOCK_SIZE)
#define DATA_BLOCK_COUNT                  (DATA_SECT_COUNT * BLOCKS_PER_SECT)

/*
 * calculate everything from startSect and pcmCount
 */
#define TAILBUF_SIZE_IN_SECT              16                                    // must be power of 2
#define TAILBUF_SIZE_IN_ADDR              (TAILBUF_SIZE_IN_SECT << 4)

#define TAILBUF_STEP_IN_SECT              (TAILBUF_SIZE_IN_SECT / 4)            // 1/4
#define TAILBUF_STEP_IN_ADDR              (TAILBUF_STEP_IN_SECT << 4)

#define TAILBUF_START_ADDR                (recStartAddr + TAILBUF_STEP_IN_ADDR)

#define TAILBUF_MAX_VALID_BLOCKS          ((TAILBUF_SIZE_IN_ADDR * 3 / 4) - BLOCKS_PER_SECT)

#define TAILBUF_ADDR(index)               (TAILBUF_START_ADDR + \
                                           (index / TAILBUF_SIZE_IN_ADDR) * TAILBUF_STEP_IN_ADDR + \
                                           (index % TAILBUF_SIZE_IN_ADDR))

#ifdef LOG_PCM_PACKET
#define ADPCM_MAX_VALID_BLOCKS            ((DATA_SECT_COUNT - 1 - TAILBUF_SIZE_IN_SECT - TAILBUF_STEP_IN_SECT) * BLOCKS_PER_SECT)
#else
#define ADPCM_MAX_VALID_BLOCKS            ((DATA_SECT_COUNT - 1) * BLOCKS_PER_SECT)
#endif

#define UARTREQ_INVALID                   ((uint32_t)-1)
#define UARTREQ_LAST                      ((uint32_t)-2)

/*********************************************************************
 * TYPEDEFS
 */
#define ADPCM_BUF_SIZE                    80
#define PCMBUF_SIZE                       (ADPCM_BUF_SIZE * 4)
#define PCM_SAMPLES_PER_BUF               (PCMBUF_SIZE / sizeof(int16_t))
#define PCMBUF_NUM                        4
#define PCMBUF_TOTAL_SIZE                 (PCMBUF_SIZE * PCMBUF_NUM)

#define ADPCM_SIZE_PER_SECT               4000
#define ADPCM_BUF_COUNT_PER_SECT          (ADPCM_SIZE_PER_SECT / ADPCM_BUF_SIZE)

#define SEGMENT_NUM                       20

typedef struct WriteContext
{
  I2S_Transaction i2sTransaction[PCMBUF_NUM];

  List_List recordingList;
  List_List processingList;

  uint8_t pcmBuf[PCMBUF_TOTAL_SIZE];
  uint8_t adpcmBuf[ADPCM_BUF_SIZE];

  uint32_t adpcmCount;
  uint32_t adpcmCountInSect;

  uint32_t startSect;
  uint32_t currSect;

  /* these are working adpcm state */
  int16_t prevSample;
  uint8_t prevIndex;

  /* these records initial adpcm state for a sector */
  int16_t prevSampleInSect;
  uint8_t prevIndexInSect;

  bool finished;
} WriteContext_t;

typedef struct __attribute__ ((__packed__)) footnote
{
  uint16_t prevSample;
  uint8_t prevIndex;
  uint8_t dummy;
  uint32_t segments[SEGMENT_NUM + 1];
  uint8_t cka;
  uint8_t ckb;
} footnote_t;

_Static_assert(sizeof(footnote_t)==90, "wrong footnote size");

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

#if defined (LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET)

/* both level and edge triggered */
static Semaphore_Handle semUartTxReady = NULL;
static bool uartTxReady = true;

/* requests with lock */
GateSwi_Handle pcmRequestGate = NULL;
uint32_t uartPcmRequest;

GateSwi_Handle adpcmRequestGate = NULL;
uint32_t uartAdpcmRequest;
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

/* Transactions will successively be part of the i2sReadList, the treatmentList and the i2sWriteList */

#if defined (LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET)
uint8_t uartTxBuf[PKT_UART_SIZE * NUM_UART_PKT];
uint8_t uartRxBuf[32];
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

#if defined (LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET)
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
static void loadSegments(void);
static void updateSegments(void);
static void startRecording(void);
static void stopRecording(void);

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

#if defined (LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET)
  Semaphore_Params_init(&semParams);
  semParams.event = audioEvent;
  semParams.eventId = UART_TX_RDY_EVT;
  semParams.mode = Semaphore_Mode_BINARY;
  semUartTxReady = Semaphore_create(1, &semParams, Error_IGNORE);

  GateSwi_Params gateParams;

#if defined (LOG_PCM_PACKET)
  GateSwi_Params_init(&gateParams);
  pcmRequestGate = GateSwi_create(&gateParams, Error_IGNORE);
#endif

#if defined (LOG_ADPCM_PACKET)
  GateSwi_Params_init(&gateParams);
  adpcmRequestGate = GateSwi_create(&gateParams, Error_IGNORE);
#endif

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

#if defined (LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET)
  int sent = 0;
#endif

  // Initialize application
  Audio_init();
  loadCounter();
  loadSegments();

  // TODO change this to an event
  startRecording();

  // Application main loop
  for (int loop = 0;; loop++)
  {
    uint32_t event = Event_pend(audioEvent, NULL, AUDIO_EVENTS,
                                BIOS_WAIT_FOREVER);
    if (event & AUDIO_PCM_EVT)
    {
      Semaphore_pend(semDataReadyForTreatment, BIOS_NO_WAIT);
      I2S_Transaction *ttt = (I2S_Transaction*) List_get(&wctx->processingList);
      if (ttt != NULL)
      {
        int16_t *samples = (int16_t*) ttt->bufPtr;

        for (int i = 0; i < PCM_SAMPLES_PER_BUF; i++)
        {
          uint8_t code = adpcmEncoder(samples[i], &wctx->prevSample,
                                      &wctx->prevIndex);
          if (i % 2 == 0)
          {
            wctx->adpcmBuf[i / 2] = code;
          }
          else
          {
            wctx->adpcmBuf[i / 2] |= (code << 4);
          }
        }

        size_t offset = (wctx->currSect % DATA_SECT_COUNT) * SECT_SIZE
            + wctx->adpcmCountInSect * ADPCM_BUF_SIZE;
        if (wctx->adpcmCountInSect == 0)
        {
          NVS_erase(nvsHandle, offset, SECT_SIZE);
        }

        NVS_write(nvsHandle, offset, wctx->adpcmBuf, ADPCM_BUF_SIZE,
                  NVS_WRITE_POST_VERIFY);

        // last adpcm buf in sect
        if (wctx->adpcmCountInSect + 1 == ADPCM_BUF_COUNT_PER_SECT)
        {
          footnote_t fn = {};
          fn.prevSample = wctx->prevSampleInSect;
          fn.prevIndex = wctx->prevIndexInSect;
          fn.dummy = 0;

          updateSegments();
          memcpy(fn.segments, segments, sizeof(segments));
          checksum(&fn, sizeof(fn) - 2, &fn.cka, &fn.ckb);

          offset += ADPCM_BUF_SIZE;
          NVS_write(nvsHandle, offset, &fn, sizeof(fn), NVS_WRITE_POST_VERIFY);

          incrementCounter();

          wctx->currSect = MONOTONIC_COUNTER;
          wctx->prevIndexInSect = wctx->prevIndex;
          wctx->prevSampleInSect = wctx->prevSample;
        }

        wctx->adpcmCount++;
        wctx->adpcmCountInSect = wctx->adpcmCount / ADPCM_BUF_COUNT_PER_SECT;

        List_put(&wctx->recordingList, (List_Elem*) ttt);
      } /* end of if ttt != NULL */
    } /* end of AUDIO PCM EVENT */

#if defined (LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET)
    if (uartTxReady)
    {
#if defined (LOG_PCM_PACKET) && defined(LOG_PCM_SUPPORT_QOS)
      if (pktcnt < NUM_UART_PKT && pcm)
      {
        memcpy(&uartTxBuf[PKT_UART_SIZE * pktcnt], pcm, PKT_UART_SIZE);
        pktcnt++;
      }
#endif

      if (uartPktCnt > 0)
      {
        UART_write(uartHandle, uartTxBuf, uartPktCnt * PKT_UART_SIZE);
        uartTxReady = false;
        sent++;
      }

      while (sent == 5000)
      {
        Task_sleep(1000 * 1000 / Clock_tickPeriod);
      }
    } /* end of uartTxReady */
#endif /* defined (LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET) */

  } /* end of loop */
}

static void startRecording(void)
{
  wctx = &writeContext;
  memset(wctx, 0, sizeof(writeContext));

  wctx->adpcmCount = 0;
  wctx->adpcmCountInSect = 0;
  wctx->startSect = MONOTONIC_COUNTER;
  wctx->currSect = wctx->startSect;
  wctx->prevIndex = 0;
  wctx->prevSample = 0;
  wctx->prevIndexInSect = 0;
  wctx->prevSampleInSect = 0;
  wctx->finished = false;

  List_clearList(&wctx->recordingList);
  List_clearList(&wctx->processingList);

  for (int k = 0; k < PCMBUF_NUM; k++)
  {
    I2S_Transaction_init(&wctx->i2sTransaction[k]);
    wctx->i2sTransaction[k].bufPtr = &wctx->pcmBuf[k * PCMBUF_SIZE];
    wctx->i2sTransaction[k].bufSize = PCMBUF_SIZE;
    List_put(&wctx->recordingList, (List_Elem*) &wctx->i2sTransaction[k]);
  }

#ifdef LOG_PCM_PACKET
  wctx->pcmIndex = 0;
#endif

#if defined(LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET)
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
  i2sParams.fixedBufferLength = 240;
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
  I2S_stopRead(i2sHandle);
  I2S_stopClocks(i2sHandle);
  I2S_close(i2sHandle);
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

#if defined (LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET)
static void uartReadCallbackFxn(UART_Handle handle, void *buf, size_t count)
{
  if (strcmp(buf, "start") == 0)
  {

  }
  else if (strcmp(buf, "stop") == 0)
  {

  }
  else
  {
  }
}

static void uartWriteCallbackFxn(UART_Handle handle, void *buf, size_t count)
{
  uartTxReady = true;
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

static void loadSegments(void)
{
  if (MONOTONIC_COUNTER == 0) return;

  footnote_t fn = {};
  size_t offset = (MONOTONIC_COUNTER - 1) % DATA_SECT_COUNT * SECT_SIZE + ADPCM_SIZE_PER_SECT;
  NVS_read(nvsHandle, offset, &fn, sizeof(fn));

  uint8_t cka, ckb;
  checksum(&fn, sizeof(fn) - 2, &cka, &ckb);
  if (fn.cka == cka && fn.ckb == ckb && fn.segments[0] == MONOTONIC_COUNTER)
  {
    memcpy(segments, &fn.segments, sizeof(segments));
  }
}

static void updateSegments(void)
{
  if (segments[0] == (uint32_t) -1) // uninitialized
  {
    segments[1] = wctx->startSect;
    segments[0] = wctx->currSect + 1;
  }
  else
  {
    if (segments[0] == wctx->startSect)
    {  // first sector
      for (int i = SEGMENT_NUM; i > 0; i--)
      {
        segments[i] = segments[i - 1];
      }
    }
    segments[0] = wctx->currSect + 1;
  }
}

#if 0
      if (pktcnt < NUM_UART_PKT)
      {
        uint32_t key = GateSwi_enter(pcmRequestGate);
        uint32_t index = uartPcmRequest;
        uartPcmRequest = UARTREQ_INVALID;
        GateSwi_leave(pcmRequestGate, key);

        // now we calculate whether the request can be fulfilled solely according to pcmCount
        // if it can be fulfilled, and it 'happens' to be latest packet pointed by pcm. skip.
        // the max index we can serve is pcmCount - 1 (last one)
        // the min index we can serve is pcmCount - (TAILBUF_SIZE_IN_ADDR  * 3/4 - BLOCKS_PER_SECT)
        // if req index is UARTREQ_LAST, it is automatically pcmCount - 1
        uint32_t max = recPcmIndex == 0 ? 0 : recPcmIndex - 1;
        uint32_t min = (recPcmIndex < TAILBUF_MAX_VALID_BLOCKS) ? 0 : recPcmIndex - TAILBUF_MAX_VALID_BLOCKS;
        if (index == UARTREQ_LAST)
        {
          index = max;
        }

        if (min <= index && index <= max && !(pcm && index == max))
        {
          uint32_t pos = pktcnt * PKT_UART_SIZE;
          memcpy(&uartTxBuf[pos], Preamble, sizeof(Preamble));
          pos += sizeof(Preamble);

          uint32_t addr = TAILBUF_ADDR(index);
          uint32_t offset = addr % DATA_BLOCK_COUNT * BLOCK_SIZE;
          NVS_read(nvsHandle, offset, &uartTxBuf[pos], PKT_UART_SIZE - sizeof(Preamble));
          pktcnt++;
        }
      }
#endif

#if 0
      if (pktcnt < NUM_UART_PKT)
      {
        uint32_t key = GateSwi_enter(adpcmRequestGate);
        uint32_t index = uartAdpcmRequest;
        uartAdpcmRequest = UARTREQ_INVALID;
        GateSwi_leave(adpcmRequestGate, key);

        uint32_t adpcmCount = recPcmIndex / 4;
        uint32_t max = adpcmCount == 0 ? 0 : adpcmCount - 1;
        uint32_t min = adpcmCount < ADPCM_MAX_VALID_BLOCKS ? 0 : adpcmCount - ADPCM_MAX_VALID_BLOCKS;

        if (min <= index && index <= max && !(adpcm && index == max))
        {
          uint32_t pos = pktcnt * PKT_UART_SIZE;
          memcpy(&uartTxBuf[pos], Preamble, sizeof(Preamble));
          pos += sizeof(Preamble);

          uint32_t addr = recStartAddr + index;
          uint32_t offset = addr & DATA_BLOCK_COUNT * BLOCK_SIZE;
          NVS_read(nvsHandle, offset, &uartTxBuf[pos], PKT_UART_SIZE - sizeof(Preamble));
          pktcnt++;
        }
      }
#endif