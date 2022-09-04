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

#include "icall.h"

#include <ti/display/Display.h>

#include <board.h>

#include "simple_gatt_profile.h"
#include "simple_peripheral.h"

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

#if defined (LOG_ADPCM_DATA) && !defined (Display_DISABLE_ALL)
#error LOG_ADPCM_DATA conflicts with TI Display driver, Use Display_DISABLE_ALL to disable Display.
#endif

#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

#define interval_of(type, start, end)                           \
        (offsetof(type, end) - offsetof(type, start))

/*********************************************************************
 * CONSTANTS
 */

// hexdump -n 4 -e '"%08X\n"' /dev/urandom
#define MAGIC                             (0x5A344176)

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
#define UART_TX_RDY_EVT                   Event_Id_04
#define UART_RX_RDY_EVT                   Event_Id_05
#define AUDIO_INCOMING_MSG                Event_Id_06
#define AUDIO_OUTGOING_MSG                Event_Id_07
#define AUDIO_BLE_SUBSCRIBE               Event_Id_08
#define AUDIO_BLE_UNSUBSCRIBE             Event_Id_09

#define AUDIO_REC_AUTOSTOP                Event_Id_31 // used for debugging

#define AUDIO_EVENTS                                \
  (AUDIO_PCM_EVT | AUDIO_START_REC | AUDIO_STOP_REC | AUDIO_READ_EVT | \
   UART_TX_RDY_EVT | UART_RX_RDY_EVT | AUDIO_INCOMING_MSG | AUDIO_OUTGOING_MSG | \
   AUDIO_REC_AUTOSTOP | AUDIO_BLE_SUBSCRIBE | AUDIO_BLE_UNSUBSCRIBE )

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

#define MAX_RECORDING_SECTORS             20

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

typedef struct __attribute__ ((__packed__)) AdpcmState
{
  int16_t sample;
  uint8_t index;
  uint8_t dummy;
} AdpcmState_t;

_Static_assert(sizeof(AdpcmState_t)==4, "wrong size of adpcm state");

typedef struct ctx
{
  /*
   * The first 256 bytes of this struct is the first chunk in sectors.
   * The remaining 24 chunks are 160 bytes each.
   * 1. 256 + 24 * 160 = 4096.
   * 2. 25 * 160 = 4000 (adpcm data, exactly 0.5s for 16000 sample rate)
   */

  /*
   * There are 21 previous and 1 current segments.
   *
   *        A0      A1      A2      A3      A4
   *        ....
   *        0xffff  0xffff  0xffff  0xffff
   *        0xffff  0xffff  0xffff  0xffff
   *        0xffff  0xffff  0xffff  0xffff
   * -------------------------------------------------------------------------
   * start  0x0000  0x0000  0x0000  0x0000
   * pos    0x0000  0x0001  0x0002  0x0003
   *
   *                0xffff          0xffff  0xffff
   *                0xffff          0xffff  0xffff
   *                0x0000          0x0000  0xffff
   * -------------------------------------------------------------------------
   * start          0x0001          0x0003  0x0003
   * pos            0x0001          0x0003  0x0004
   *
   *                B1              B3      B4 (running)
   *
   * The initial state is A0. When A1 starts, the snapshot of A0 is saved in
   * sector 0. Similarly, if the recording continues, A1 will be written into
   * sector 1.
   *
   * If the recording stops at sector 1. stopRecording() should shift-up all
   * segment starts by one cell, which means, prevs[last] = curr; the both
   * current start and current pos are assigned to current sector index.
   */
  uint32_t recordings[NUM_RECS];
  uint32_t recStart;
  uint32_t recPos;
  AdpcmState_t recAdpcmStateInSect;                  // sector-wise adpcm state

  uint8_t adpcmBuf[ADPCMBUF_NUM][ADPCMBUF_SIZE];
  uint8_t pcmBuf[PCMBUF_TOTAL_SIZE];
  AdpcmState_t recAdpcmState;
  uint32_t recAdpcmCount;
  uint32_t recAdpcmCountInSect;

  I2S_Transaction i2sTransaction[PCMBUF_NUM];
  List_List recordingList;
  List_List processingList;
  bool recording;

  uint32_t readStart;
  uint32_t readEnd;
  uint32_t readPosMajor;
  uint32_t readPosMinor;
  AdpcmState_t readAdpcmState;

  bool reading;

  bool subscriptionOn;
} ctx_t;

_Static_assert(offsetof(ctx_t, adpcmBuf)==96,
               "wrong write context (header) layout");

_Static_assert(offsetof(ctx_t, pcmBuf)==256,
               "wrong write context (header) size");

#ifdef LOG_ADPCM_DATA
typedef struct __attribute__ ((__packed__)) UartPacket
{
  uint64_t preamble;    // 0
  uint32_t startSect;// 8
  uint32_t index;// 12
  int16_t prevSample;// 16
  uint8_t prevIndex;// 18
  uint8_t dummy;// 19
  uint8_t adpcm[ADPCMBUF_SIZE];
#ifdef LOG_PCM_DATA
  uint8_t pcm[PCMBUF_SIZE];
#endif
  uint8_t cka;
  uint8_t ckb;
} UartPacket_t;

#ifdef LOG_PCM_DATA
_Static_assert(sizeof(UartPacket_t)== ADPCMBUF_SIZE + PCMBUF_SIZE + 22,
    "wrong uart packet size");
#else
_Static_assert(sizeof(UartPacket_t)== ADPCMBUF_SIZE + 22,
    "wrong uart packet size");
#endif
#endif

#ifdef LOG_BADPCM_DATA
typedef struct __attribute__ ((__packed__)) UartPacket
{
  uint64_t preamble;
  OutgoingMsgType type;
  union {
    BadpcmPacket_t bad;
    StatusPacket_t status;
  };
  uint8_t cka;
  uint8_t ckb;
} UartPacket_t;
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

#ifndef Display_DISABLE_ALL
Display_Handle    dispHandle;
#endif

Event_Handle audioEvent;

OutgoingMsg_t outmsg[1];
IncomingMsg_t inmsg[2];

static Semaphore_Handle semOutgoingMsgFreed;
static List_List freeOutgoingMsgs;

// static Semaphore_Handle semIncomingMsgPending;
static List_List pendingIncomingMsgs;
static List_List freeIncomingMsgs;


/*********************************************************************
 * LOCAL VARIABLES
 */

/* @formatter:off */

/* Table of index changes */
const static signed char IndexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8,
                                            -1, -1, -1, -1, 2, 4, 6, 8, };

/* Quantizer step size lookup table */
const static int StepSizeTable[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
                                       19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
                                       50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
                                       130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
                                       337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
                                       876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
                                       2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
                                       5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
                                       15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

/* Used for calculating bitmap for monotonic counter */
static const uint8_t markedBytes[8] = { 0x7f, 0x3f, 0x1f, 0x0f, 0x07, 0x03, 0x01,
                                        0x00 };
/* @formatter:on */

/*
 * Synchronization
 */

static Semaphore_Handle semDataReadyForTreatment = NULL;

#if defined (LOG_ADPCM_DATA) || defined (LOG_BADPCM_DATA)
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

ctx_t ctx = { };

#if defined (LOG_ADPCM_DATA) || defined (LOG_BADPCM_DATA)
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

#if defined (LOG_ADPCM_DATA) || defined (LOG_BADPCM_DATA)
static void uartWriteCallbackFxn(UART_Handle handle, void *buf, size_t count);
#endif

static char adpcmEncoder(short sample, int16_t *prevSample, uint8_t *prevIndex);
static short adpcmDecoder(char code, int16_t* prevSample, uint8_t *prevIndex);
void checksum(void *p, uint32_t len, uint8_t *a, uint8_t *b);

extern void simple_peripheral_spin(void);

static int countMarkedBits(int sectIndex, size_t size);
static void incrementMarkedBits(int sectIndex, size_t current);
static void resetLowCounter(void);
static void resetCounter(void);
static uint32_t readMagic(void);
static void loadCounter(void);
static void incrementCounter(void);

static void loadRecordings(void);
static void sendStatusMsg(void);

static void startRecording(void);
static void stopRecording(void);

void Audio_subscribe(void)
{
  Event_post(audioEvent, AUDIO_BLE_SUBSCRIBE);
}

void Audio_unsubscribe(void)
{
  Event_post(audioEvent, AUDIO_BLE_UNSUBSCRIBE);
}

void freeOutgoingMsg(OutgoingMsg_t *msg)
{
  List_put(&freeOutgoingMsgs, (List_Elem*)msg);
  Semaphore_post(semOutgoingMsgFreed);
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

//  Mailbox_Params mboxParams;
//  Mailbox_Params_init(&mboxParams);
//  mboxParams.readerEvent = audioEvent;
//  mboxParams.readerEventId = AUDIO_INCOMING_MSG;
//  incomingMailbox = Mailbox_create(sizeof(uint32_t), 4, &mboxParams, Error_IGNORE);

//  Semaphore_Params_init(&semParams);
//  semParams.event = audioEvent;
//  semParams.eventId = AUDIO_INCOMING_MSG;
//  semIncomingMsgPending = Semaphore_create(0, &semParams, Error_IGNORE);

  Semaphore_Params_init(&semParams);
  semParams.event = audioEvent;
  semParams.eventId = AUDIO_OUTGOING_MSG;
  semOutgoingMsgFreed = Semaphore_create(1, &semParams, Error_IGNORE);

  List_clearList(&freeIncomingMsgs);
  List_put(&freeIncomingMsgs, (List_Elem*)&inmsg[0]);
  List_put(&freeIncomingMsgs, (List_Elem*)&inmsg[1]);

  List_clearList(&pendingIncomingMsgs);

  List_clearList(&freeOutgoingMsgs);
  List_put(&freeOutgoingMsgs, (List_Elem*)&outmsg[0]);
  // List_put(&freeOutgoingMsgs, (List_Elem*)&outmsg[1]);

#if defined (LOG_ADPCM_DATA) || defined (LOG_BADPCM_DATA)
  Semaphore_Params_init(&semParams);
//  semParams.event = audioEvent;
//  semParams.eventId = UART_TX_RDY_EVT;
  semParams.mode = Semaphore_Mode_BINARY;
  semUartTxReady = Semaphore_create(1, &semParams, Error_IGNORE);

  UART_init();

  UART_Params uartParams;
  UART_Params_init(&uartParams);
  uartParams.baudRate = 1500000;// 115200 * 8;
  uartParams.readMode = UART_MODE_BLOCKING;
  uartParams.readDataMode = UART_DATA_TEXT;
  uartParams.readReturnMode = UART_RETURN_NEWLINE;
  uartParams.readCallback = NULL;
  uartParams.readEcho = UART_ECHO_OFF;
  uartParams.writeMode = UART_MODE_CALLBACK;
  uartParams.writeDataMode = UART_DATA_BINARY;
  uartParams.writeCallback = uartWriteCallbackFxn;
  uartHandle = UART_open(Board_UART0, &uartParams);
#endif

  NVS_Params nvsParams;
  NVS_Params_init(&nvsParams);
  nvsHandle = NVS_open(Board_NVSEXTERNAL, &nvsParams);
  NVS_getAttrs(nvsHandle, &nvsAttrs);

  I2S_init();
}

/*********************************************************************
 * @fn      Audio_taskFxn
 *
 * @brief   Application task entry point for the Audio task.
 *
 * @param   a0, a1 - not used.
 */
static void Audio_taskFxn(UArg a0, UArg a1)
{
//  Types_FreqHz freq;
//  Timestamp_getFreq(&freq);

#ifndef Display_DISABLE_ALL
  Display_init();
  Display_Params dispParams;
  Display_Params_init(&dispParams);
  dispHandle = Display_open(Display_Type_ANY, &dispParams);
#endif

  Audio_init();

  loadCounter();
  Display_print1(dispHandle, 0xff, 0, "counter     : %08x", MONOTONIC_COUNTER);

  loadRecordings();
  Display_print5(dispHandle, 0xff, 0, "recordings  : %08x %08x %08x %08x %08x",
                 ctx.recordings[0], ctx.recordings[1], ctx.recordings[2],
                 ctx.recordings[3], ctx.recordings[4]);
  Display_print5(dispHandle, 0xff, 0, "              %08x %08x %08x %08x %08x",
                 ctx.recordings[5], ctx.recordings[6], ctx.recordings[7],
                 ctx.recordings[8], ctx.recordings[9]);
  Display_print5(dispHandle, 0xff, 0, "              %08x %08x %08x %08x %08x",
                 ctx.recordings[10], ctx.recordings[11], ctx.recordings[12],
                 ctx.recordings[13], ctx.recordings[14]);
  Display_print5(dispHandle, 0xff, 0, "              %08x %08x %08x %08x %08x",
                 ctx.recordings[15], ctx.recordings[16], ctx.recordings[17],
                 ctx.recordings[18], ctx.recordings[19]);
  Display_print1(dispHandle, 0xff, 0, "              %08x", ctx.recordings[20]);
  Display_print1(dispHandle, 0xff, 0, "restart     : %08x", ctx.recStart);
  Display_print1(dispHandle, 0xff, 0, "recPos      : %08x", ctx.recPos);

  Event_post(audioEvent, AUDIO_START_REC);

  for (int loop = 0;; loop++)
  {
    uint32_t event = Event_pend(audioEvent, NULL, AUDIO_EVENTS,
                                BIOS_WAIT_FOREVER);
    if (event & AUDIO_START_REC)
    {
      Display_print0(dispHandle, 0xff, 0, "event       : AUDIO_START_REC");
      startRecording();
    }

    if (event & AUDIO_STOP_REC)
    {
      Display_print0(dispHandle, 0xff, 0, "event       : AUDIO_STOP_REC");
      stopRecording();
    }

    if (event & AUDIO_PCM_EVT)
    {
      Semaphore_pend(semDataReadyForTreatment, BIOS_NO_WAIT);
      if (ctx.recording)
      {
        I2S_Transaction *ttt = (I2S_Transaction*) List_get(
            &ctx.processingList);
        if (ttt != NULL)
        {
#ifdef LOG_ADPCM_DATA
          /* save a copy */
          int16_t uartPrevSample = ctx.recAdpcmState.sample;
          uint8_t uartPrevIndex = ctx.recAdpcmState.index;
#endif
          uint16_t adpcmInUse = ctx.recAdpcmCount % ADPCMBUF_NUM;
          int16_t *samples = (int16_t*) ttt->bufPtr;
          for (int i = 0; i < PCM_SAMPLES_PER_BUF; i++)
          {
            uint8_t code = adpcmEncoder(samples[i], &ctx.recAdpcmState.sample,
                                        &ctx.recAdpcmState.index);
            if (i % 2 == 0)
            {
              ctx.adpcmBuf[adpcmInUse][i / 2] = code;
            }
            else
            {
              ctx.adpcmBuf[adpcmInUse][i / 2] |= (code << 4);
            }
          }
#ifdef LOG_ADPCM_DATA
          Semaphore_pend(semUartTxReady, BIOS_WAIT_FOREVER);
          uartPkt.preamble = PREAMBLE;
          uartPkt.startSect = ctx.recStart;
          uartPkt.index = ctx.recAdpcmCount;
          uartPkt.prevSample = uartPrevSample;
          uartPkt.prevIndex = uartPrevIndex;
#ifdef LOG_PCM_DATA
          uartPkt.dummy = 1;
          memcpy(uartPkt.pcm, ttt->bufPtr, PCMBUF_SIZE);
#else
          uartPkt.dummy = 0;
#endif
          memcpy(uartPkt.adpcm, ctx.adpcmBuf[adpcmInUse], ADPCMBUF_SIZE);
          checksum(&uartPkt.startSect,
                   offsetof(UartPacket_t, cka) - offsetof(UartPacket_t, startSect),
                   &uartPkt.cka, &uartPkt.ckb);
          UART_write(uartHandle, &uartPkt, sizeof(uartPkt));
#endif
          List_put(&ctx.recordingList, (List_Elem*) ttt);

          if (adpcmInUse == ADPCMBUF_NUM - 1)
          {
            if (ctx.recAdpcmCountInSect == adpcmInUse)
            {
              // uint32_t bufCount = 0;
              size_t offset = (ctx.recPos % DATA_SECT_COUNT) * SECT_SIZE;
              size_t size = 96 + ADPCMBUF_SIZE * ADPCMBUF_NUM;
              NVS_write(nvsHandle, offset, &ctx, size, 0); // NVS_WRITE_POST_VERIFY);

              Display_print5(
                  dispHandle, 0xff, 0,
                  " - nvs write, pos 0x%08x, cnt %06d, cntInSect %02d, offset 0x%08x (%%4k %04d), size 256",
                  ctx.recPos,
                  ctx.recAdpcmCount,
                  ctx.recAdpcmCountInSect, offset, offset % 4096);
            }
            else
            {
              uint32_t writtenBufCount = ctx.recAdpcmCountInSect / ADPCMBUF_NUM
                  * ADPCMBUF_NUM;
              size_t offset = (ctx.recPos % DATA_SECT_COUNT) * SECT_SIZE + 96
                  + writtenBufCount * ADPCMBUF_SIZE;
              size_t size = ADPCMBUF_SIZE * ADPCMBUF_NUM;
              NVS_write(nvsHandle, offset, &ctx.adpcmBuf[0], size, 0); // NVS_WRITE_POST_VERIFY);

              Display_print5(
                  dispHandle, 0xff, 0,
                  " - nvs write, pos 0x%08x, cnt %06d, cntInSect %02d, offset 0x%08x (%%4k %04d), size 160",
                  ctx.recPos,
                  ctx.recAdpcmCount,
                  ctx.recAdpcmCountInSect, offset, offset % 4096);
            }
          }

          ctx.recAdpcmCount++;
          ctx.recAdpcmCountInSect = ctx.recAdpcmCount % ADPCM_BUF_COUNT_PER_SECT;

          // This generates too much output
          //        Display_print2(dispHandle, 0xff, 0, "-- cnt %d, cntInSect %d",
          //                       ctx.adpcmCount, ctx.adpcmCountInSect);

          // last adpcm buf in sect
          if (ctx.recAdpcmCountInSect == 0)
          {
            ctx.recPos++;
            ctx.recAdpcmStateInSect = ctx.recAdpcmState;
            incrementCounter();

//            Display_print4(dispHandle, 0xff, 0,
//                           "increment sect, pos: %d (%08x), counter %d (%08x)",
//                           ctx.currPos, ctx.currPos, MONOTONIC_COUNTER,
//                           MONOTONIC_COUNTER);

            Display_print2(
                dispHandle, 0xff, 0,
                "new sector  : pos 0x%08x, counter 0x%08x",
                ctx.recPos, MONOTONIC_COUNTER);


//            Display_print2(dispHandle, 0xff, 0, "start %d ()", ctx.recStart, ctx.recStart);
//            Display_print2(dispHandle, 0xff, 0, "pos   %d (0x%08x)", ctx.recPos, ctx.recPos);
//            Display_print2(dispHandle, 0xff, 0, "count %d (0x%08x)", MONOTONIC_COUNTER, MONOTONIC_COUNTER);

            /* it is important to do this here */
            size_t offset = (ctx.recPos % DATA_SECT_COUNT) * SECT_SIZE;
            NVS_erase(nvsHandle, offset, SECT_SIZE);

            Display_print2(dispHandle, 0xff, 0,
                           " - nvs erase,     0x%08x (%%4k %d)", offset,
                           offset % 4096);

            if (ctx.recPos - ctx.recStart == MAX_RECORDING_SECTORS)
            {
              Display_print2(
                  dispHandle,
                  0xff,
                  0,
                  "max rec sect reached, before stopRecording(). start 0x%08x, pos 0x%08x",
                  ctx.recStart, ctx.recPos);
              stopRecording();
              Display_print2(
                  dispHandle,
                  0xff,
                  0,
                  "                      after  stopRecording(). start 0x%08x, pos 0x%08x",
                  ctx.recStart, ctx.recPos);

              Event_post(audioEvent, AUDIO_REC_AUTOSTOP);
            }
          }
        } /* end of if ttt != NULL */
      } /* end of if ctx */
    } /* end of AUDIO PCM EVENT */

    if (event & AUDIO_BLE_SUBSCRIBE)
    {
      ctx.subscriptionOn = true;
      Display_print0(dispHandle, 0xff, 0, "subscription on ");
    }

    if (event & AUDIO_BLE_UNSUBSCRIBE)
    {
      ctx.subscriptionOn = false;
      if (ctx.reading)
      {
        ctx.reading = false;
        Display_print0(dispHandle, 0xff, 0, "subscription off and stop reading");
      }
      else
      {
        Display_print0(dispHandle, 0xff, 0, "subscription off");
      }
    }

//    if (event & AUDIO_INCOMING_MSG)
//    {
//      Semaphore_pend(semIncomingMsgPending, 0);
//    }

    if (event & AUDIO_OUTGOING_MSG)
    {
      Semaphore_pend(semOutgoingMsgFreed, 0);
    }

    if (ctx.subscriptionOn)
    {
      /* when space available */
      while (List_head(&freeOutgoingMsgs))
      {
        IncomingMsg_t *msg = (IncomingMsg_t*)List_get(&pendingIncomingMsgs);
        if (msg)
        {
          Display_print1(dispHandle, 0xff, 0, "incoming msg (command) %d", msg->type);

          if (msg->type == IMT_START_REC)
          {
            Display_print0(dispHandle, 0xff, 0, "start recording");
            startRecording();
          }
          else if (msg->type == IMT_STOP_REC)
          {
            Display_print0(dispHandle, 0xff, 0, "stop recording");
            stopRecording();
          }
          else if (msg->type == IMT_START_READ)
          {
            Display_print0(dispHandle, 0xff, 0, "start reading");
            ctx.readStart = msg->start;
            ctx.readEnd = msg->end;
            ctx.readPosMajor  = ctx.readStart;
            ctx.readPosMinor = 0;
            ctx.reading = true;
          }
          else if (msg->type == IMT_STOP_READ)
          {
            Display_print0(dispHandle, 0xff, 0, "stop reading");
            ctx.reading = false;
          }
          List_put(&freeIncomingMsgs, (List_Elem*)msg);

          sendStatusMsg();
        }
        else if (ctx.reading)
        {
          if (ctx.readStart >= ctx.recStart)  // live
          {
            if (ctx.readPosMajor >= ctx.recPos) // break if blocked, stop if rec stopped
            {
              if (!ctx.recording)
              {
                Display_print0(dispHandle, 0xff, 0, "stop (forward) reading when recording stopped.");
                ctx.reading = false;
                sendStatusMsg();
              }
              break;
            }
          }
          else
          {
            if (ctx.readPosMajor >= ctx.recStart || ctx.readPosMajor >= ctx.readEnd)
            {
              Display_print0(dispHandle, 0xff, 0, "stop reading when reaching rec start or read end.");
              ctx.reading = false;
              sendStatusMsg();
              break;
            }
          }

          /*
           * in-range means:
           * upper bound: readPosMajor < recPos
           * lower bound: readPosMajor + DATA_SECT_COUNT > recPos
           */
          if (!(ctx.readPosMajor + DATA_SECT_COUNT > ctx.recPos))
          {
            // adjusted
            uint32_t major = ctx.recPos - DATA_SECT_COUNT + 1;

            Display_print2(dispHandle, 0xff, 0, "read major %d adjusted to %d",
                           ctx.readPosMajor, major);

            ctx.readPosMajor = major;
            ctx.readPosMinor = 0;
          }

          OutgoingMsg_t *outmsg = (OutgoingMsg_t*) List_get(&freeOutgoingMsgs);

          if (ctx.readPosMinor == 0)
          {
            size_t offset = (ctx.readPosMajor % DATA_SECT_COUNT) * SECT_SIZE + 92;
            NVS_read(nvsHandle, offset, &ctx.readAdpcmState, sizeof(AdpcmState_t));
          }

          size_t offset = (ctx.readPosMajor % DATA_SECT_COUNT) * SECT_SIZE + 96
              + ctx.readPosMinor * BADPCM_DATA_SIZE;

          Display_print5(
              dispHandle, 0xff, 0,
              "read offset %d (%08x) @ major %d (%08x) minor %d", offset,
              offset, ctx.readPosMajor, ctx.readPosMajor, ctx.readPosMinor);

          NVS_read(nvsHandle, offset, &outmsg->bad.data[0], BADPCM_DATA_SIZE);

          outmsg->bad.major = ctx.readPosMajor;
          outmsg->bad.minor = ctx.readPosMinor;
          outmsg->bad.index = ctx.readAdpcmState.index;
          outmsg->bad.sample = ctx.readAdpcmState.sample;

          // update adpcm state for next read
          for (int i = 0; i < BADPCM_DATA_SIZE; i++)
          {
            char x = outmsg->bad.data[i];
            adpcmDecoder(x & 0x0f, &ctx.readAdpcmState.sample, &ctx.readAdpcmState.index);
            adpcmDecoder((x >> 4) & 0x0f, &ctx.readAdpcmState.sample, &ctx.readAdpcmState.index);
          }

          outmsg->type = OMT_BADPCM;

#ifdef LOG_BADPCM_DATA
          Semaphore_pend(semUartTxReady, BIOS_WAIT_FOREVER);
          memset(&uartPkt, 0, sizeof(uartPkt));
          uartPkt.preamble = PREAMBLE;
          uartPkt.type = outmsg->type;
          uartPkt.bad = outmsg->bad;

          checksum(&uartPkt.type,
                   offsetof(UartPacket_t, cka) - offsetof(UartPacket_t, type),
                   &uartPkt.cka, &uartPkt.ckb);

          UART_write(uartHandle, &uartPkt, sizeof(uartPkt));
#endif

          sendOutgoingMsg(outmsg);

          ctx.readPosMinor++;
          if (ctx.readPosMinor == 4000 / BADPCM_DATA_SIZE)
          {
            ctx.readPosMajor++;
            ctx.readPosMinor = 0;
          }
        }
        else
        {
          break;  // no incoming message, no read operation wip
        }
      }
    }
    else  // subscriptionOn == false
    {
      List_Elem* msg;
      while (msg = List_get(&pendingIncomingMsgs))
      {
        List_put(&freeIncomingMsgs, msg);
        Display_print2(dispHandle, 0xff, 0, "incoming msg %d (%08x) discarded",
                       ((IncomingMsg_t* )msg)->type,
                       ((IncomingMsg_t* )msg)->type);
      }

    }

#if defined(LOG_ADPCM_DATA) && defined (LOG_NVS_AFTER_AUTOSTOP)
    if (event & AUDIO_REC_AUTOSTOP)
    {
      uint32_t start = ctx.recordings[NUM_RECS - 1];
      uint32_t end = ctx.recStart;
      for (uint32_t pos = start; pos != end; pos++)
      {
        for (int i = 0; i < 100; i++)
        {
          Semaphore_pend(semUartTxReady, BIOS_WAIT_FOREVER);
          uartPkt.preamble = PREAMBLE;
          uartPkt.startSect = start;
          uartPkt.index = (pos - start) * 100 + i;
          if (i == 0)
          {
            size_t offset = pos % DATA_SECT_COUNT * SECT_SIZE
                + offsetof(ctx_t, recAdpcmStateInSect);
            NVS_read(nvsHandle, offset, &uartPkt.prevSample, 40 + 4);
            uartPkt.dummy = 0;
          }
          else
          {
            size_t offset = pos % DATA_SECT_COUNT * SECT_SIZE
                + offsetof(ctx_t, adpcmBuf)
                + i * 40;
            NVS_read(nvsHandle, offset, &uartPkt.adpcm[0], 40);
            uartPkt.prevSample = 0;
            uartPkt.prevIndex = 0;
            uartPkt.dummy = 2;
          }

          checksum(&uartPkt.startSect,
                   offsetof(UartPacket_t, cka) - offsetof(UartPacket_t, startSect),
                   &uartPkt.cka, &uartPkt.ckb);

          UART_write(uartHandle, &uartPkt, sizeof(uartPkt));
        }
      }
    }
#endif


  } /* end of loop */
}

static void startRecording(void)
{
  if (ctx.recording)
    return;

  ctx.recStart = MONOTONIC_COUNTER;
  ctx.recPos = ctx.recStart;
  ctx.recAdpcmState.sample = 0;
  ctx.recAdpcmState.index = 0;
  ctx.recAdpcmState.dummy = 0;
  ctx.recAdpcmStateInSect = ctx.recAdpcmState;
  ctx.recAdpcmCount = 0;
  ctx.recAdpcmCountInSect = 0;

  ctx.recording = true;

  /** ahead of writing erasure */
  size_t offset = ctx.recPos % DATA_SECT_COUNT * SECT_SIZE;
  NVS_erase(nvsHandle, offset, SECT_SIZE);

  Task_sleep(10 * 1000 / Clock_tickPeriod);

  List_clearList(&ctx.recordingList);
  List_clearList(&ctx.processingList);

  for (int k = 0; k < PCMBUF_NUM; k++)
  {
    I2S_Transaction_init(&ctx.i2sTransaction[k]);
    ctx.i2sTransaction[k].bufPtr = &ctx.pcmBuf[k * PCMBUF_SIZE];
    ctx.i2sTransaction[k].bufSize = PCMBUF_SIZE;
    List_put(&ctx.recordingList, (List_Elem*) &ctx.i2sTransaction[k]);
  }

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
                       (I2S_Transaction*) List_head(&ctx.recordingList));

  /*
   * Start I2S streaming
   */
  I2S_startClocks(i2sHandle);
  I2S_startRead(i2sHandle);
}

static void stopRecording(void)
{
  if (!ctx.recording)
    return;

  if (i2sHandle)
  {
    I2S_stopRead(i2sHandle);
    I2S_stopClocks(i2sHandle);
    I2S_close(i2sHandle);
  }

  for (int i = 0; i < NUM_RECS; i++)
  {
    ctx.recordings[i] = ctx.recordings[i + 1];
  }
  ctx.recStart = ctx.recPos;
  ctx.recording = false;
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
    List_remove(&ctx.recordingList, (List_Elem*) transactionFinished);
    List_put(&ctx.processingList, (List_Elem*) transactionFinished);

    /* Start the treatment of the data */
    Semaphore_post(semDataReadyForTreatment);
  }
}

#if defined (LOG_ADPCM_DATA) || defined (LOG_BADPCM_DATA)
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

static short adpcmDecoder(char code, int16_t* prevSample, uint8_t *prevIndex)
{
  int predsample;
  int index;
  int step;
  int diffq;

  /* Restore previous values of predicted sample and quantizer step
   size index
   */
  predsample = *prevSample;
  index = *prevIndex;
  /* Find quantizer step size from lookup table using index
   */
  step = StepSizeTable[index];
  /* Inverse quantize the ADPCM code into a difference using the
   quantizer step size
   */
  diffq = step >> 3;
  if (code & 4)
    diffq += step;
  if (code & 2)
    diffq += step >> 1;
  if (code & 1)
    diffq += step >> 2;
  /* Add the difference to the predicted sample
   */
  if (code & 8)
    predsample -= diffq;
  else
    predsample += diffq;
  /* Check for overflow of the new predicted sample
   */
  if (predsample > 32767)
    predsample = 32767;
  else if (predsample < -32768)
    predsample = -32768;
  /* Find new quantizer step size by adding the old index and a
   table lookup using the ADPCM code
   */
  index += IndexTable[code];
  /* Check for overflow of the new quantizer step size index
   */
  if (index < 0)
    index = 0;
  if (index > 88)
    index = 88;
  /* Save predicted sample and quantizer step size index for next
   iteration
   */
  *prevSample = predsample;
  *prevIndex = index;
  /* Return the new speech sample */
  return (int16_t)(predsample);
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

/**
 * @fn loadPrevStarts
 *
 */
static void loadRecordings(void)
{
  uint32_t counter = MONOTONIC_COUNTER;
  if (counter == 0)
  {
    for (int i = 0; i < NUM_RECS; i++)
    {
      ctx.recordings[i] = 0xFFFFFFFF;
    }
  }
  else
  {
    // skip earlist one
    size_t offset = (counter - 1) % DATA_SECT_COUNT * SECT_SIZE + 4;
    NVS_read(nvsHandle, offset, &ctx, sizeof(uint32_t) * NUM_RECS);
  }

  ctx.recStart = counter;
  ctx.recPos = counter;
}

/*
 * @fn sendStatusMsg
 */
static void sendStatusMsg(void)
{
  OutgoingMsg_t *outmsg = (OutgoingMsg_t*) List_get(&freeOutgoingMsgs);

  memcpy(outmsg->status.recordings, &ctx.recordings,
         sizeof(uint32_t) * NUM_RECS);
  outmsg->status.recStart = ctx.recStart;
  outmsg->status.recPos = ctx.recPos;
  outmsg->status.readStart = ctx.readStart;
  outmsg->status.readEnd = ctx.readEnd;
  outmsg->status.readPosMajor = ctx.readPosMajor;
  outmsg->status.readPosMinor = ctx.readPosMinor;
  outmsg->status.flags = ctx.recording ? 1 : 0 + ctx.reading ? 2 : 0;
  outmsg->type = OMT_STATUS;


#ifndef Display_DISABLE_ALL
  Display_print5(dispHandle, 0xff, 0, "status: recordings %08x %08x %08x %08x %08x",
                 outmsg->status.recordings[0], outmsg->status.recordings[1],
                 outmsg->status.recordings[2], outmsg->status.recordings[3],
                 outmsg->status.recordings[4]);
  Display_print5(dispHandle, 0xff, 0, "                   %08x %08x %08x %08x %08x",
                 outmsg->status.recordings[5], outmsg->status.recordings[6],
                 outmsg->status.recordings[7], outmsg->status.recordings[8],
                 outmsg->status.recordings[9]);
  Display_print5(dispHandle, 0xff, 0, "                   %08x %08x %08x %08x %08x",
                 outmsg->status.recordings[10], outmsg->status.recordings[11],
                 outmsg->status.recordings[12], outmsg->status.recordings[13],
                 outmsg->status.recordings[14]);
  Display_print5(dispHandle, 0xff, 0, "                   %08x %08x %08x %08x %08x",
                 outmsg->status.recordings[15], outmsg->status.recordings[16],
                 outmsg->status.recordings[17], outmsg->status.recordings[18],
                 outmsg->status.recordings[19]);
  Display_print1(dispHandle, 0xff, 0, "                   %08x",
                 outmsg->status.recordings[20]);
  Display_print3(dispHandle, 0xff, 0,
                 "        recording: %d, recStart %08x, recPos %08x",
                 outmsg->status.flags & 0x00000001, outmsg->status.recStart,
                 outmsg->status.recPos);
  Display_print5(dispHandle, 0xff, 0,
                 "        reading: %d, readStart %08x, "
                 "readEnd %08x, major: %08x, minor %08x",
                 outmsg->status.flags & 0x00000002,
                 outmsg->status.readStart,
                 outmsg->status.readEnd,
                 outmsg->status.readPosMajor,
                 outmsg->status.readPosMinor);
#endif

#ifdef LOG_BADPCM_DATA
  Semaphore_pend(semUartTxReady, BIOS_WAIT_FOREVER);
  memset(&uartPkt, 0, sizeof(uartPkt));
  uartPkt.preamble = PREAMBLE;
  uartPkt.type = outmsg->type;
  uartPkt.bad = outmsg->bad;

  checksum(&uartPkt.type,
           offsetof(UartPacket_t, cka) - offsetof(UartPacket_t, type),
           &uartPkt.cka, &uartPkt.ckb);

  UART_write(uartHandle, &uartPkt, sizeof(uartPkt));
#endif

  sendOutgoingMsg(outmsg);
}

void recvIncomingMsg(IncomingMsg_t* msg)
{
  List_put(&pendingIncomingMsgs, (List_Elem*)msg);
  Event_post(audioEvent, AUDIO_INCOMING_MSG);
}

IncomingMsg_t* allocIncomingMsg(void)
{
  return (IncomingMsg_t*)List_get(&freeIncomingMsgs);
}

