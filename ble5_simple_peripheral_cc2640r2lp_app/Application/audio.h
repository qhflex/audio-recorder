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
  RM_REQUESTED,
  RM_PROCESSING_REQUEST,
  RM_RESPONDED,
  RM_PROCESSING_RESPONSE,
} ReadStatus_t;

typedef struct ReadMessage
{
  ReadStatus_t status;
  int type;
  uint32_t session;     // a unique number for identifying request session
  uint32_t start;       // requested start sect
  uint32_t major;       // sector
  uint32_t minor;       // sub
  uint8_t buf[256];
  uint32_t readLen;
} ReadMessage_t;

extern ReadMessage_t readMessage; // singleton

void Audio_startRecording();
void Audio_stopRecording();

/** this function is implemented by audio */
void Audio_read();
/** this function is implemented by caller, ble or uart */
void Audio_readDone();

#endif /* APPLICATION_AUDIO_H_ */
