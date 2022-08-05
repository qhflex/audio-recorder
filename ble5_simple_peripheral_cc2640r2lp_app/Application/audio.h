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

typedef struct __attribute__ ((__packed__)) BadpcmPacket
{
  uint32_t major;
  uint8_t minor;
  uint8_t index;
  int16_t sample;
  uint8_t data[200];
} BadpcmPacket_t;

typedef struct OutgoingMsg
{
  List_Elem listElem;
  size_t len;
  uint8_t data[208];
} OutgoingMsg_t;

void sendOutgoingMsg(OutgoingMsg_t *msg);
void freeOutgoingMsg(OutgoingMsg_t *msg);

#endif /* APPLICATION_AUDIO_H_ */
