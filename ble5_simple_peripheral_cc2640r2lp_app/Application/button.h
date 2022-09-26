/*
 * button.h
 *
 *  Created on: Jun 26, 2022
 *      Author: ma
 */

#ifndef APPLICATION_BUTTON_H_
#define APPLICATION_BUTTON_H_

#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Semaphore.h>

/**
 * @brief   Util Queue Event ID
 *
 * In order to wake an application when a message is inserted into its
 * queue, an event must be posted.  Util reserved Event Id 30 for a generic
 * queue event.
 */
#define BTN_SHUTDOWN_EVT    Event_Id_30

/*
 * Task creation function for the power button
 */
void Button_createTask(void);

extern Semaphore_Handle launchAudioSem;
extern Semaphore_Handle launchBleSem;

/*
 * audio.c has its own recording state in ctx. this one is used for synchronization
 * between button.c and audio.c
 */
extern bool recordingState;

#endif /* APPLICATION_BUTTON_H_ */
