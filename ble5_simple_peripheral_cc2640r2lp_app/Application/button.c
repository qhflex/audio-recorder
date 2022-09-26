/*
 * button.c
 *
 *  Created on: Jun 26, 2022
 *      Author: ma
 */
#include <stdbool.h>

#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>

#include <ti/drivers/Power.h>
#include <ti/drivers/PIN.h>
#include <ti/drivers/pin/PINCC26XX.h>
#include <ti/drivers/GPIO.h>

#include <ti/display/Display.h>

#include "board.h"
#include "audio.h"
#include "button.h"

#define BUTTON_TASK_PRIORITY                1
#define BUTTON_TASK_STACK_SIZE              384

/*********************************************************************
 * How to integrate this file (and header)
 *
 * 1. check your board-specific .c file, ensure all pin used, except
 *    BTN_PIN are PIN_INPUT_EN  | PIN_NOPULL
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */

// Task configuration
Task_Struct buttonTask;

#if defined __TI_COMPILER_VERSION__
#pragma DATA_ALIGN(buttonTaskStack, 8)
#else
#pragma data_alignment=8
#endif
uint8_t buttonTaskStack[BUTTON_TASK_STACK_SIZE];

Semaphore_Handle launchAudioSem;
Semaphore_Handle launchBleSem;

bool recordingState = false;
bool shuttingDown = false;

/* Wake-up Button pin table */
//PIN_Config ButtonTableWakeUp[] = {
//    Board_PIN_BUTTON0 | PIN_INPUT_EN | PIN_PULLUP | PINCC26XX_WAKEUP_NEGEDGE,
//    PIN_TERMINATE                                 /* Terminate list */
//};

//const PIN_Config ButtonTableWakeUp[] = {
//    CC2640R2DK_CXS_PIN_BTN1     | PIN_INPUT_EN  | PIN_NOPULL | PINCC26XX_WAKEUP_NEGEDGE,   /* Button is active low */
//    CC2640R2DK_CXS_PERIPH_EN    | PIN_INPUT_EN  | PIN_NOPULL,
//
//#ifndef NO_PERIPH
//    CC2640R2DK_CXS_SPI_FLASH_WP | PIN_INPUT_EN  | PIN_NOPULL,                       /* External flash chip select */
//    CC2640R2DK_CXS_UART_RX      | PIN_INPUT_EN  | PIN_NOPULL,                       /* UART RX via debugger back channel */
//    CC2640R2DK_CXS_UART_TX      | PIN_INPUT_EN  | PIN_NOPULL,                       /* UART TX via debugger back channel */
//    CC2640R2DK_CXS_SPI0_MOSI    | PIN_INPUT_EN  | PIN_NOPULL,                       /* SPI master out - slave in */
//    CC2640R2DK_CXS_SPI0_MISO    | PIN_INPUT_EN  | PIN_NOPULL,                       /* SPI master in - slave out */
//    CC2640R2DK_CXS_SPI0_CLK     | PIN_INPUT_EN  | PIN_NOPULL,                       /* SPI clock */
//    CC2640R2DK_CXS_SPI0_CSN     | PIN_INPUT_EN  | PIN_NOPULL,                       /* External flash chip select */
//#endif
//
//    PIN_TERMINATE
//};

const PIN_Config ButtonTableWakeUp[] = {

//  CC2640R2DK_5MM_KEY_POWER | PIN_INPUT_EN | PIN_PULLUP | PIN_HYSTERESIS,                    /* Power button */
//  CC2640R2DK_5MM_I2S_SELECT | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL,            /* MEMS microphone select */
//  CC2640R2DK_5MM_SPI_FLASH_CS | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL,          /* SPI flash chip select */
//  CC2640R2DK_5MM_SPI_FLASH_RESET | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL,       /* SPI flash reset pin */
//  CC2640R2DK_5MM_SPI_FLASH_WP | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL,          /* SPI flash write protection */
//  CC2640R2DK_5MM_1V8_EN | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL,                /* 1.8V PMIC load output */
//  CC2640R2DK_5MM_UART_RX | PIN_INPUT_EN | PIN_PULLDOWN,                                     /* UART RX via debugger back channel */
//  CC2640R2DK_5MM_UART_TX | PIN_GPIO_OUTPUT_EN | PIN_GPIO_HIGH | PIN_PUSHPULL | PIN_DRVSTR_MAX,              /* UART TX via debugger back channel */
//  CC2640R2DK_5MM_SPI0_MOSI | PIN_INPUT_EN | PIN_PULLDOWN,                                   /* SPI master out - slave in */
//  CC2640R2DK_5MM_SPI0_MISO | PIN_INPUT_EN | PIN_PULLDOWN,                                   /* SPI master in - slave out */
//  CC2640R2DK_5MM_SPI0_CLK | PIN_INPUT_EN | PIN_PULLDOWN,                                    /* SPI clock */

    CC2640R2DK_5MM_KEY_POWER        | PIN_INPUT_EN | PIN_PULLUP | PINCC26XX_WAKEUP_NEGEDGE,   /* Power button */
    CC2640R2DK_5MM_I2S_SELECT       | PIN_INPUT_EN | PIN_NOPULL,                              /* MEMS microphone select */
    CC2640R2DK_5MM_I2S_ADI          | PIN_INPUT_EN | PIN_NOPULL,
    CC2640R2DK_5MM_I2S_BCLK         | PIN_INPUT_EN | PIN_NOPULL,
    CC2640R2DK_5MM_I2S_WCLK         | PIN_INPUT_EN | PIN_NOPULL,
    CC2640R2DK_5MM_SPI_FLASH_CS     | PIN_INPUT_EN | PIN_NOPULL,                              /* SPI flash chip select */
    CC2640R2DK_5MM_SPI_FLASH_RESET  | PIN_INPUT_EN | PIN_NOPULL,                              /* SPI flash reset pin */
    CC2640R2DK_5MM_SPI_FLASH_WP     | PIN_INPUT_EN | PIN_NOPULL,                              /* SPI flash write protection */
    CC2640R2DK_5MM_1V8_EN           | PIN_INPUT_EN | PIN_NOPULL,                              /* 1.8V PMIC load output */
    CC2640R2DK_5MM_UART_RX          | PIN_INPUT_EN | PIN_NOPULL,                              /* UART RX via debugger back channel */
    CC2640R2DK_5MM_UART_TX          | PIN_INPUT_EN | PIN_NOPULL,                              /* UART TX via debugger back channel */
    CC2640R2DK_5MM_SPI0_MOSI        | PIN_INPUT_EN | PIN_NOPULL,                              /* SPI master out - slave in */
    CC2640R2DK_5MM_SPI0_MISO        | PIN_INPUT_EN | PIN_NOPULL,                              /* SPI master in - slave out */
    CC2640R2DK_5MM_SPI0_CLK         | PIN_INPUT_EN | PIN_NOPULL,                              /* SPI clock */

    PIN_TERMINATE
};

/*********************************************************************
 * EXTERNAL VARIABLES
 */
extern bool isWakingFromShutdown;
extern void simple_peripheral_spin(void);

extern Display_Handle dispHandle;

extern Event_Handle audioEvent;

extern bool subscriptionOn;

/*********************************************************************
 * EXTERNAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */
static int btn[6] = {0, 0, 0, 0, 1, 0};

#define LONG_PRESS      (                                 btn[4] ==  200)
#define SINGLE_CLICK    (                btn[2] < -30  && \
                           8 < btn[3] && btn[3] <  30  && btn[4] == -1  )

#define DOUBLE_CLICK    ((0 == btn[0] || btn[0] < -30) && \
                           8 < btn[1] && btn[1] <  30  && \
                         -30 < btn[2] && btn[2] < -8   && \
                           8 < btn[3] && btn[3] <  30  && btn[4] == -1  )

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void Button_taskFxn(UArg a0, UArg a1);
static bool Button_pollingPowerOn(void);
static void Button_pollingPowerOff(void);
static void Button_shutdown(void);

static void Button_read(void);
static void Button_enablePeriph(void);

/*********************************************************************
 * @fn      Button_createTask
 *
 * @brief   Task creation function for Gps.
 */
void Button_createTask(void)
{
  Task_Params taskParams;

  // Configure task
  Task_Params_init(&taskParams);
  taskParams.stack = buttonTaskStack;
  taskParams.stackSize = BUTTON_TASK_STACK_SIZE;
  taskParams.priority = BUTTON_TASK_PRIORITY;

  Task_construct(&buttonTask, Button_taskFxn, &taskParams, NULL);
}

/*************
 * @fn      Button_read
 *
 * @brief   return 1 if button pressed, -1 if button released, 0 if uncertain.
 */
static void Button_read(void)
{
  static uint32_t samples = 0x0000000f;
  int s = 0;

  samples = (samples << 1) & 0x0000000f;
  if (!GPIO_read(Board_GPIO_BTN1))
  {
    samples |= 0x00000001;
  }

  if (samples == 0x0f)
  {
    s = 1;
  }
  else if (samples == 0x00)
  {
    s= -1;
  }

  if (s == 0)
  {
    if (btn[4] > 0)
    {
      btn[4]++;
    }
    else
    {
      btn[4]--;
    }
  }
  else if (s * btn[4] > 0)
  {
    btn[4] += s;
  }
  else {
    btn[0] = btn[1];
    btn[1] = btn[2];
    btn[2] = btn[3];
    btn[3] = btn[4];
    btn[4] = s;
  }
}

static void Button_enablePeriph(void)
{
  GPIO_setConfig(Board_GPIO_1V8_EN, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_HIGH);
  GPIO_setConfig(Board_GPIO_I2S_SELECT, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_HIGH);
  GPIO_setConfig(Board_GPIO_FLASH_CS, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_HIGH);
  GPIO_setConfig(Board_GPIO_FLASH_RESET, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_HIGH);
  GPIO_setConfig(Board_GPIO_FLASH_WP, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_HIGH);

  // TODO could this be shorter?
  for (int i = 0; i < 20; i++) {
    Button_read();
    Task_sleep(10 * 1000 / Clock_tickPeriod);
  }
}

/*********************************************************************
 * @fn      Button_taskFxn
 *
 * @brief   Application task entry point for (Power) Button.
 *
 * @param   a0, a1 - not used.
 */
static void Button_taskFxn(UArg a0, UArg a1)
{
  volatile int t = 1;       // used for debug
  int timeout = 0;          // for idle state

  // shutdown if not wake up from pin
  if (!isWakingFromShutdown)
  {
    Button_shutdown();
  }

  // initial state loop
  for (;;)
  {
    Button_read();

    if (LONG_PRESS)
    {
      Display_print5(dispHandle, 0xff, 0, "(0) long press  : %d %d %d %d %d",
                     btn[0], btn[1], btn[2], btn[3], btn[4]);

      Button_enablePeriph();
      recordingState = false;
      Semaphore_post(launchAudioSem);
      Semaphore_post(launchBleSem);
      goto idle;
    }

    if (DOUBLE_CLICK)
    {
      Display_print5(dispHandle, 0xff, 0, "(0) double click: %d %d %d %d %d",
                     btn[0], btn[1], btn[2], btn[3], btn[4]);

      if (t == 0)
      {
        Task_sleep(10 * 1000 / Clock_tickPeriod);
        continue;
      }

      Button_enablePeriph();
      recordingState = true;
      Semaphore_post(launchAudioSem);
      goto recording;
    }

    int *head = btn[3] == 0 ? &btn[4] : btn[2] == 0 ? &btn[3] :
                btn[1] == 0 ? &btn[2] : btn[0] == 0 ? &btn[1] : &btn[0];

#define HL      (  0 < head[0] && head[0] <  200)
#define H0A     (  0 < head[0] && head[0] <  30 )
#define H0      (  8 < head[0] && head[0] <  30 )
#define Z1      (                 head[1] == 0  )
#define H1A     (-30 < head[1] && head[1] <  0  )
#define H1      (-30 < head[1] && head[1] < -8  )
#define Z2      (                 head[2] == 0  )
#define H2A     (  0 < head[2] && head[2] <  30 )
#define H2      (  8 < head[2] && head[2] <  30 )
#define Z3      (                 head[3] == 0  )

    bool longPressWip = HL && Z1;
    bool doubleClickWip = (H0A && Z1) || (H0 && H1A && Z2) || (H0 && H1 && H2A && Z3);
    bool wip = longPressWip || doubleClickWip;

#undef HL
#undef H0A
#undef H0
#undef Z1
#undef H1A
#undef H1
#undef Z2
#undef H2A
#undef H2
#undef Z3

    if (t && !wip)
    {
      Display_print5(dispHandle, 0xff, 0, "(0) invalid wake: %d %d %d %d %d",
                     btn[0], btn[1], btn[2], btn[3], btn[4]);

      Button_shutdown();
    }

    Task_sleep(10 * 1000 / Clock_tickPeriod);
  }

recording:

  for (;;)
  {
    Button_read();

    if (recordingState == false)
    {
      Display_print0(dispHandle, 0xff, 0, "(1) recording finished.");
      Button_shutdown();
    }

    if (DOUBLE_CLICK)
    {
      Display_print5(dispHandle, 0xff, 0, "(1) double click: %d %d %d %d %d",
                     btn[0], btn[1], btn[2], btn[3], btn[4]);
      Audio_stopRec();
      Button_shutdown();
    }

    if (LONG_PRESS)
    {
      Display_print5(dispHandle, 0xff, 0, "(1) long press  : %d %d %d %d %d",
                     btn[0], btn[1], btn[2], btn[3], btn[4]);

      Audio_stopRec();
      Semaphore_post(launchBleSem);
      goto idle;
    }

    Task_sleep(10 * 1000 / Clock_tickPeriod);
  }

idle:

  for (;;)
  {
    timeout = subscriptionOn ? 0 : (timeout + 1);
    if (timeout > 180 * 100) {
      Display_print0(dispHandle, 0xff, 0, "(2) ble idle timeout.");
      Button_shutdown();
    }

    Button_read();


    if (SINGLE_CLICK)
    {
      Display_print5(dispHandle, 0xff, 0, "(2) single click: %d %d %d %d %d",
                     btn[0], btn[1], btn[2], btn[3], btn[4]);
      Button_shutdown();
      break;  // to preserve code after loop TODO
    }

    Task_sleep(10 * 1000 / Clock_tickPeriod);
  }

  /*
   * The following code is not going to be executed
   */

  // debouncing key hold/key release
  if (t & !Button_pollingPowerOn())
  {
    // key release
    Button_shutdown();
  }

  Button_pollingPowerOff();

  shuttingDown = true;
  Event_post(audioEvent, BTN_SHUTDOWN_EVT);

  Task_sleep(100 * 1000 / Clock_tickPeriod);
  Button_shutdown();
}

static bool Button_pollingPowerOn(void)
{
  int sum = 0;
  for (;;)
  {
    // active low
    if (!GPIO_read(Board_GPIO_BTN1))
    {
      // pressed
      sum = sum < 0 ? 0 : sum + 1;
    }
    else
    {
      // released
      // sum = sum > 0 ? 0 : sum - 1;
      sum = sum - 1;
    }

    if (sum > 15) return true;
    if (sum < -10) return false;
    Task_sleep(200 * 1000 / Clock_tickPeriod);
#ifdef CC2640R2_LAUNCHXL
    GPIO_toggle(Board_GPIO_GLED);
#endif
  }
}

static void Button_pollingPowerOff(void)
{
  int sum = 0;

  // continue until a button release
  for (;;)
  {
    if(GPIO_read(Board_GPIO_BTN1))
    {
      sum++;
    }
    else
    {
      sum = 0;
    }

    if (sum > 2)
    {
      sum = 0;
      break;
    }
    Task_sleep(100 * 1000 / Clock_tickPeriod);
  }

  for (;;)
  {
    if (!GPIO_read(Board_GPIO_BTN1))
    {
      sum = sum < 0 ? 0 : sum + 1;
    }
    else
    {
      sum = 0;
    }

    if (sum > 3) return;
    Task_sleep(1000 * 1000 / Clock_tickPeriod);
  }
}

static void Button_shutdown(void)
{
#ifdef CC2640R2_LAUNCHXL
  for (int i = 0; i < 10; i++)
  {
    GPIO_toggle(Board_GPIO_RLED);
    Task_sleep(200 * 1000 / Clock_tickPeriod);
  }
  GPIO_write(Board_GPIO_RLED, 0);
  GPIO_write(Board_GPIO_GLED, 0);
  Task_sleep(100 * 1000 / Clock_tickPeriod);
#endif

  Display_print0(dispHandle, 0xff, 0, "shutdown in 400ms");

  Task_sleep(400 * 1000 / Clock_tickPeriod);

  /* Configure DIO for wake up from shutdown */
  PINCC26XX_setWakeup(ButtonTableWakeUp);

  /* Go to shutdown */
  Power_shutdown(0, 0);
}

