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

void Audio_createTask(void);

typedef struct __attribute__ ((__packed__))
{
  uint8_t dummy;
  uint8_t previndex;
  int16_t prevsample;
  uint8_t data[240];
} AdpcmPacket_t ;

_Static_assert(sizeof(AdpcmPacket_t)==244, "wrong adpcm packet size");

/*
 * 'play' means start play, or seek. It could be called again when playing.
 * 'stopPlay' stops playing, flushing data, releasing resources.
 *
 * Noting that audio data producer has no knowledge on whether ble is
 * connected,or whether notification or indication is used. That's up to
 * ble module and is transparent to audio data producer.
 */
void Audio_play(long position);
void Audio_stopPlay();

#define AUDIO_POSITION_LIVE                         LONG_MAX
#define AUDIO_POSITOIN_EARLIEST                     0

/*
 * There are two lists inside, A and B.
 * I gave up to find a proper verb for the batch operation: List_get(&A),
 * List_put(&B), and finally List_head(&A) again. Alternatively, we use
 * a more policy-oriented name, and only one function, instead of two.
 */
bool Audio_moreData(AdpcmPacket_t** pData);
extern void (*audioMoreDataAvailable)(void);

void Audio_moreSpace(void);

void Audio_startRecording();
void Audio_stopRecording();

#endif /* APPLICATION_AUDIO_H_ */
