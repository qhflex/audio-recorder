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

void Audio_createTask(void);

typedef struct __attribute__ ((__packed__))
{
  uint8_t dummy;
  uint8_t previndex;
  int16_t prevsample;
  uint8_t data[240];
} AdpcmPacket_t;

_Static_assert(sizeof(AdpcmPacket_t)==244, "wrong adpcm packet size");

typedef enum {
  RM_IDLE = 0,
  RM_REQUESTED,           // sent to audio
  RM_PROCESSING_REQUEST,  // audio handling (unnecessary)
  RM_RESPONDED,           // sent to SimplePeripheral
  RM_PROCESSING_RESPONSE, // aka notifying.
} ReadMessageStatus_t;

typedef struct ReadMessage
{
  ReadMessageStatus_t status;
  int notiSession;        // notification session copied from context
  int readSession;        // read session copied from context
  int readType;           // defaults to 0, not used
  int start;              // read start (sect)
  int end;                // read end (sect)
  int major;              // sector
  int minor;              // sub
  uint8_t buf[256];
  int readLen;
  int error;              // used for return; stop reading if error
} ReadMessage_t;

extern ReadMessage_t readMessage; // singleton

/** this function is implemented by audio */
void Audio_read();
/** this function is implemented by caller, ble or uart */
void Audio_readDone();

void Audio_subscribe();
void Audio_unsubscribe();

typedef struct Mail
{
  size_t size;  // when size == 0, this is a command
  void * p;
} Mail_t;

#define AUDIO_BLE_SUBSCRIBE               Event_Id_20
#define AUDIO_BLE_UNSUBSCRIBE             Event_Id_21

extern Event_Handle audioEvent;

extern Mailbox_Handle incomingMailbox;
extern Mailbox_Handle outgoingMailbox;

#endif /* APPLICATION_AUDIO_H_ */
