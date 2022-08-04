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

void Audio_subscribe();
void Audio_unsubscribe();

typedef struct Mail
{
  void * p;
  size_t len;
} Mail_t;

extern Mailbox_Handle incomingMailbox;
extern Mailbox_Handle outgoingMailbox;

#endif /* APPLICATION_AUDIO_H_ */
