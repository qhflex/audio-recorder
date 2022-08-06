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
extern Mailbox_Handle incomingMailbox;

#define BADPCM_DATA_SIZE                  160
#define NUM_PREV_RECS                     21
#define NUM_TOTAL_RECS                    22

typedef struct __attribute__ ((__packed__)) BadpcmPacket
{

  uint32_t major;
  uint8_t minor;
  uint8_t index;
  int16_t sample;
  uint8_t data[BADPCM_DATA_SIZE];

} BadpcmPacket_t;

_Static_assert(sizeof(BadpcmPacket_t)==BADPCM_DATA_SIZE + 8, "wrong badcpm packet size");

typedef struct __attribute__ ((__packed__)) StatusPacket
{
  uint32_t flags; /* 1 << 0 recording, 1 << 1 reading */
  uint32_t recordings[NUM_PREV_RECS];
  uint32_t recStart;
  uint32_t recPos;
  uint32_t readStart;
  uint32_t readPosMajor;
  uint32_t readPosMinor;
} StatusPacket_t;

_Static_assert(sizeof(StatusPacket_t)==108, "wrong status packet size");

#define OMT_STATUS                (0)
#define OMT_BADPCM                (1)

typedef uint32_t OutgoingMsgType;

typedef struct __attribute__ ((__packed__)) OutgoingMsg
{
  List_Elem listElem;
  OutgoingMsgType type;     // +   4 = 12
  union {                   // + 168 = 180
    uint8_t raw[0];
    BadpcmPacket_t bad;
    StatusPacket_t status;
  };
} OutgoingMsg_t;

void sendOutgoingMsg(OutgoingMsg_t *msg);
void freeOutgoingMsg(OutgoingMsg_t *msg);

#endif /* APPLICATION_AUDIO_H_ */
