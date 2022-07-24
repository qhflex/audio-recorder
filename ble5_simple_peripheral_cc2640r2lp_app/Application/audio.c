/*
 * i2s_mic.c
 *
 *  Created on: Jun 24, 2022
 *      Author: ma
 */
/*********************************************************************
 * INCLUDES
 */
#include <audio.h>
#include <string.h>

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
#define AUDIO_UART_REQ_EVT                Event_Id_02
#define MORE_SPACE_EVT                    Event_Id_03
#define LESS_SPACE_EVT                    Event_Id_04 // this is a pun. it's acturally a stop event

#define AUDIO_ALL_EVENTS                  (AUDIO_PCM_EVT | UART_TX_RDY_EVT | AUDIO_UART_REQ_EVT | MORE_SPACE_EVT | LESS_SPACE_EVT)

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

typedef struct __attribute__ ((__packed__)) DataPacket
{
  union {
    List_Elem elem;
    uint64_t preamble;
  };

  /* address is mainly required for debugging, but it is also essential for knowing
   * that the recorded audio data has rollover-ed several times in a single session.
   * that is, continuous recording for several hours or even days.
   */
  uint32_t address;

  /*
   * session is simply the first 'address' in a session. For data storage are sector
   * aligned, we can expect the low 4 bits are always zeros. We use these bits to
   * mark the data type. 0 for adpcm non-volatile storage, and 1 for (temp) pcm
   * tailing buffer.
   */
  uint32_t session;

  union __attribute__ ((__packed__)) {
    struct __attribute__ ((__packed__)) {
      /*
       * since 400 packets generated per 6 (@8000), 3 (@16000), or 2 (@24000) second,
       * we can expect a 24-hour continuous recording will overflow an index value without
       * enough space.
       */
      uint32_t index;
    } pcm;
    struct __attribute__ ((__packed__)) {
      /*
       * same order with ble interface format
       */
      uint8_t dummy;
      uint8_t previndex;
      int16_t prevsample;
    } adpcm;
  };

  /* maybe change to signed short and 4 bit struct union in future, but this influence
   * existing code.
   */
  uint8_t data[240];

  union {
    struct __attribute__ ((__packed__)) {
      /* written into flash ended here, crc not written, total 252 bytes
       * do we need a timestamp?
       */
      uint8_t cka;
      uint8_t ckb;
      uint16_t ckPadding;
    };
    uint32_t sampleCnt;
  };
} DataPacket_t;

/* total 268 bytes*/
_Static_assert(sizeof(DataPacket_t) == 8 + 256,
               "wrong data packet size");

/* nvs write 254 bytes TODO check write */
#define PKT_NVS_RECORD_START(pkt)       (&pkt->address)
#define PKT_NVS_RECORD_SIZE             (252 + 2)
_Static_assert(interval_of(DataPacket_t, address, ckPadding) == PKT_NVS_RECORD_SIZE,
               "wrong data record size");

/* adpcm data, state + nibbles */
#define PKT_ADPCM_START(pkt)            (&(pkt)->adpcm)
#define PKT_ADPCM_SIZE                  (244)
_Static_assert(interval_of(DataPacket_t, adpcm, cka) == PKT_ADPCM_SIZE,
               "wrong adpcm data size");

/* uart send 262 bytes, including preamble  (8), data (252) and crc (2) */
#define PKT_UART_START(pkt)             (&pkt->preamble)
#define PKT_UART_SIZE                   (8 + 252 + 2)
_Static_assert(interval_of(DataPacket_t, preamble, ckPadding) == PKT_UART_SIZE,
               "wrong uart packet size");

typedef struct PcmPair
{
  int16_t s0;
  int16_t s1;
} PcmPair_t;

typedef struct __attribute__ ((__packed__)) PcmPacket
{
  uint64_t preamble;
  uint32_t address;
  uint32_t session;
  uint32_t index;
  uint8_t data[240];
  uint8_t cka;
  uint8_t ckb;
  uint16_t padding;
} PcmPacket_t;

typedef struct __attribute__ ((__packed__)) adpcmState
{
  int16_t prevsample;
  uint8_t previndex;
  unsigned int reserved : 7;
  unsigned int end : 1;
} adpcmState_t;

typedef struct recordingContext
{
  adpcmState_t adpcmState;
  uint32_t currSect;
  uint32_t segmentList[20];

  uint32_t pcmPktCnt;
  uint32_t adpcmSize;
} recordingContext_t;

typedef struct footer
{
  uint32_t currSect;
  adpcmState_t adpcmState;
  uint32_t rsvd1;
  uint32_t rsvd2;
  uint32_t sectList[20];
} footer_t;

_Static_assert(sizeof(footer_t)==96, "wrong footer size");

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
const signed short Preamble[4] = { -32767, 32767, -32767, 32767 };  // TODO

_Static_assert(sizeof(Preamble)==8, "wrong preamble size");

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

/* Transactions will successively be part of the i2sReadList, the treatmentList and the i2sWriteList */
I2S_Transaction i2sTransaction[NUM_RECORD_PKT];
DataPacket_t recordingPkt[NUM_RECORD_PKT];
AdpcmPacket_t playingPkt[NUM_PLAY_PKT];

/* a dedicated adpcm packet buffer */
DataPacket_t adpcmBuf;

#if defined (LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET)
uint8_t uartTxBuf[PKT_UART_SIZE * NUM_UART_PKT];
uint8_t uartRxBuf[32];
#endif

/*
 * Lists containing transactions. Each transaction is in turn in these three lists.
 * Noticing that
 *
 */
List_List recordingList;
int recordingListSize;
List_List processingList;
int processingListSize;

List_List playingList;
List_List freeList;

bool playingState = false;
/*
 * markedBitsLo and markedBitsHi are 'root' counters recording how many sectors have been used in
 * ringbuffer.
 * 1. they are loaded in readCounter() during initialization.
 * 2. they are updated right after the last record is written into current sector.
 */
int markedBitsLo = -1;
int markedBitsHi = -1;

/*
 * This is (block) address for writing data to flash. It is set right after monotonic counter
 * initialized, and thereafter incremented by nv_write() only.
 */
uint32_t currWritingAddr;
uint32_t currReadingAddr;   // this is (block) address for playing

/*
 * start address for current recording session. set to currWritingAddr in startRecording()
 * it should be 4k aligned if stopRecording() is properly implemented. TODO
 */
uint32_t recStartAddr;

/*
 * adpcm state, reset to zeros in startRecording()
 */
uint8_t previndex;
int16_t prevsample;

#if defined (LOG_PCM_PACKET)
uint32_t recPcmIndex;
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
static void readCounter(void);
static void incrementCounter(void);

static void startRecording(void);
static void stopRecording(void);

static void writePacket(DataPacket_t *pkt);

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

//void Audio_moreSpace(void)
//{
//  Event_post(audioEvent, MORE_SPACE_EVT);
//}
//
//void Audio_play(long position)
//{
//  Event_post(audioEvent, MORE_SPACE_EVT);
//}
//
//void Audio_stopPlay()
//{
//  Event_post(audioEvent, LESS_SPACE_EVT);
//}
//
//bool Audio_moreData(AdpcmPacket_t **pData)
//{
//  AdpcmPacket_t *adpkt = (AdpcmPacket_t*) List_head(&playingList);
//  if (!adpkt)
//    return false;
//
//  if (*pData != adpkt)
//    goto end;
//
//  // remove head
//  List_put(&freeList, List_get(&playingList));
//  Event_post(audioEvent, MORE_SPACE_EVT);
//
//  adpkt = (AdpcmPacket_t*) List_head(&playingList);
//  if (!adpkt)
//    return false;
//
//end:
//  *pData = adpkt;
//  return true;
//}

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
  readCounter();

  currWritingAddr = (MONOTONIC_COUNTER << 4);

  startRecording();

  // Application main loop
  for (int loop = 0;; loop++)
  {
    I2S_Transaction *ttt = NULL;
    DataPacket_t *pcm = NULL;

#if defined(LOG_PCM_PACKET) || defined(LOG_ADPCM_PACKET)
    int uartPktCnt = 0;
#endif

    uint32_t event = Event_pend(audioEvent, NULL, AUDIO_ALL_EVENTS,
    BIOS_WAIT_FOREVER);

    if (event & MORE_SPACE_EVT)
    {
      if (playingState == false)  // start play
      {
        // clear list
        List_clearList(&freeList);
        List_clearList(&playingList);

        // add all packets to free list
        for (int i = 0; i < NUM_PLAY_PKT; i++)
        {
          List_put(&freeList, (List_Elem*)&playingPkt[i]);
        }

        // set reading addr
        currReadingAddr = currWritingAddr;
        playingState = true;
      }
    }

    if (event & LESS_SPACE_EVT)
    {
      if (playingState == true)
      {
        // clear list
        List_clearList(&freeList);
        List_clearList(&playingList);

        playingState = false;
      }
    }

    if (event & AUDIO_PCM_EVT)
    {
      Semaphore_pend(semDataReadyForTreatment, BIOS_NO_WAIT);
      ttt = (I2S_Transaction*) List_get(&processingList);
      processingListSize--;
      if (ttt != NULL)
      {
        pcm = container_of(ttt->bufPtr, DataPacket_t, data);

#ifdef LOG_PCM_PACKET
        pcm->address = TAILBUF_ADDR(recPcmIndex);
        pcm->session = recStartAddr | PCM_FORMAT;
        pcm->pcm.index = recPcmIndex;
#ifdef LOG_PCM_SUPPORT_QOS
        writePacket(pcm);
#endif
        recPcmIndex++;
#endif

        int16_t *samples = (int16_t*) pcm->data;
        for (int i = 0; i < 120; i++)
        {
          // start a new adpcm packet
          if (adpcmBuf.sampleCnt == 0)
          {
            adpcmBuf.address = currWritingAddr;
            adpcmBuf.session = recStartAddr | ADPCM_FORMAT;
            adpcmBuf.adpcm.dummy = 0;
            adpcmBuf.adpcm.previndex = previndex;
            adpcmBuf.adpcm.prevsample = prevsample;
          }

          uint8_t code = adpcmEncoder(samples[i], &prevsample, &previndex);

          if (adpcmBuf.sampleCnt % 2 == 0)
          {
            adpcmBuf.data[adpcmBuf.sampleCnt / 2] = code;
          }
          else
          {
            adpcmBuf.data[adpcmBuf.sampleCnt / 2] |= (code << 4);
          }

          adpcmBuf.sampleCnt++;

          if (adpcmBuf.sampleCnt == 480)
          {
            writePacket(&adpcmBuf);

            if (playingState == true) // && currReadingAddr == adpcmBuf.address)
            {
              AdpcmPacket_t *adpkt = (AdpcmPacket_t*) List_get(&freeList);
              if (adpkt)
              {
                // send packet to (live) player
                memcpy(adpkt, PKT_ADPCM_START(&adpcmBuf),
                       sizeof(AdpcmPacket_t));
                List_put(&playingList, (List_Elem*) adpkt);
                currReadingAddr++;
                if (audioMoreDataAvailable)
                  audioMoreDataAvailable();
              }
            }

#if defined (LOG_ADPCM_PACKET)
            if (uartTxReady && uartPktCnt < NUM_UART_PKT)
            {
              memcpy(&uartTxBuf[PKT_UART_SIZE * uartPktCnt], &adpcmBuf,
              PKT_UART_SIZE);
              uartPktCnt++;
            }
#endif

            adpcmBuf.sampleCnt = 0;
          }
        }

        List_put(&recordingList, (List_Elem*) ttt);
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
  recStartAddr = currWritingAddr;
  previndex = 0;
  prevsample = 0;

#ifdef LOG_PCM_PACKET
  recPcmIndex = 0;
#endif

#if defined(LOG_PCM_PACKET) || defined (LOG_ADPCM_PACKET)
  uartPcmRequest = UARTREQ_INVALID;
  uartAdpcmRequest = UARTREQ_INVALID;
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

  memset(&adpcmBuf, 0, sizeof(DataPacket_t));
  adpcmBuf.preamble = PREAMBLE;

  List_clearList(&recordingList);
  List_clearList(&processingList);
  recordingListSize = 0;
  processingListSize = 0;

  for (int k = 0; k < NUM_RECORD_PKT; k++)
  {
    recordingPkt[k].preamble = PREAMBLE;
    I2S_Transaction_init(&i2sTransaction[k]);
    i2sTransaction[k].bufPtr = &recordingPkt[k].data[0];
    i2sTransaction[k].bufSize = 240;
    List_put(&recordingList, (List_Elem*) &i2sTransaction[k]);
    recordingListSize++;
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
  i2sParams.fixedBufferLength = 240;
  i2sParams.startUpDelay = 0;
  i2sParams.MCLKDivider = 2;
  i2sParams.samplingFrequency = SAMPLE_RATE;
  i2sParams.readCallback = readCallbackFxn;
  i2sParams.writeCallback = NULL;
  i2sParams.errorCallback = errCallbackFxn;

  i2sHandle = I2S_open(Board_I2S0, &i2sParams);
  I2S_setReadQueueHead(i2sHandle, (I2S_Transaction*) List_head(&recordingList));

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
    List_remove(&recordingList, (List_Elem*) transactionFinished);
    recordingListSize--;
    List_put(&processingList, (List_Elem*) transactionFinished);
    processingListSize++;

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

static void readCounter(void)
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



// write packet to flash
static void writePacket(DataPacket_t *pkt)
{
  // TODO
  checksum(&pkt->address, interval_of(DataPacket_t, address, cka), &pkt->cka,
           &pkt->ckb);

  size_t offset = pkt->address % DATA_BLOCK_COUNT * BLOCK_SIZE;

  if ((offset & 0x0fff) == 0) // 4K aligned
  {
    NVS_erase(nvsHandle, offset, SECT_SIZE);
  }

  NVS_write(nvsHandle, offset, PKT_NVS_RECORD_START(pkt), PKT_NVS_RECORD_SIZE,
  NVS_WRITE_POST_VERIFY);

  if (pkt->address == currWritingAddr)
  {
    currWritingAddr++;
    if (currWritingAddr % 0xf == 0)
    {
      incrementCounter();
    }
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
