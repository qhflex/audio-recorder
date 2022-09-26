/*
 * i2s_mic.h
 *
 *  Created on: Jun 24, 2022
 *      Author: ma
 */

#ifndef APPLICATION_AUDIO_H_
#define APPLICATION_AUDIO_H_

#include <stdint.h>
#include <stdbool.h>

#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Mailbox.h>

#include <ti/drivers/utils/List.h>

void Audio_createTask(void);



void Audio_subscribe();
void Audio_unsubscribe();
void Audio_updateDuration(uint8_t dur);
void Audio_stopRec(void);

#define IMT_NOOP                        (0)
#define IMT_STOP_REC                    (1)
#define IMT_START_REC                   (2)
#define IMT_STOP_READ                   (3)
#define IMT_START_READ                  (4)

typedef uint32_t IncomingMsgType;

typedef struct IncomingMsg
{
  List_Elem elem;
  IncomingMsgType type;
  uint32_t start;
  uint32_t end;
} IncomingMsg_t;

void recvIncomingMsg(IncomingMsg_t* msg);
IncomingMsg_t* allocIncomingMsg(void);

/*
 * Incomming message is actually a uint32_t
 *
 *
 * 0 - 0xFFFFFFEE Start Reading
 *     0xFFFFFFEF Start Recording
 *     0xFFFFFFF0 Status
 *     0xFFFFFFF1 Stop Recording
 *     0xFFFFFFF2 Stop Reading
 */
// extern Mailbox_Handle incomingMailbox;

#define BADPCM_DATA_SIZE                  160
#define NUM_RECS                          21

typedef struct __attribute__ ((__packed__)) BadpcmPacket
{
  uint32_t major;
  uint8_t minor;
  uint8_t index;
  int16_t sample;
  uint8_t data[BADPCM_DATA_SIZE];
} BadpcmPacket_t;

_Static_assert(sizeof(BadpcmPacket_t)==BADPCM_DATA_SIZE + 8,
               "wrong badcpm packet size");

typedef struct __attribute__ ((__packed__)) StatusPacket
{
  uint32_t flags; /* 1 << 0 recording, 1 << 1 reading */
  uint32_t recordings[NUM_RECS];
  uint32_t recStart;
  uint32_t recPos;
  uint32_t readStart;
  uint32_t readEnd;
  uint32_t readPosMajor;
  uint32_t readPosMinor;
} StatusPacket_t;

_Static_assert(sizeof(StatusPacket_t) == 112, "wrong status packet size");

/*
 * for alignment inside struct, OutgoingMsgType is defined to uint32_t,
 * rather than being defined as enum.
 */
#define OMT_STATUS                        (0)
#define OMT_BADPCM                        (1)

typedef uint32_t OutgoingMsgType;

typedef struct __attribute__ ((__packed__)) OutgoingMsg
{
  List_Elem listElem;
  OutgoingMsgType type;     // +   4 = 12
  union
  {                   // + 168 = 180
    uint8_t raw[0];
    BadpcmPacket_t bad;
    StatusPacket_t status;
  };
} OutgoingMsg_t;

void sendOutgoingMsg(OutgoingMsg_t *msg);
void freeOutgoingMsg(OutgoingMsg_t *msg);

#endif /* APPLICATION_AUDIO_H_ */
