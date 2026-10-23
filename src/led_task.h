//*****************************************************************************
//
//! @file led_task.h
//!
//! @brief On-board LED task (driven by BTN0 / GP46 and by the UI control centre).
//!
//! Manages the two on-board LEDs, both open-drain and active low:
//!
//!   LED_ID_LED2  silkscreen LED2 = GP11 = AM_BSP_GPIO_LED0
//!   LED_ID_LED4  silkscreen LED4 = GP83 = AM_BSP_GPIO_LED2
//!
//! Note that the BSP macro names are off by one from the board silkscreen
//! (AM_BSP_GPIO_LED0 is printed LED2). This module uses silkscreen numbering
//! throughout, because that is what the user sees on the board.
//!
//! There are two sources of control. Neither touches GPIO directly; both set a
//! target state and wake LedTask:
//!
//!   Button_callback()  ISR context, one press of BTN0 toggles LED4
//!   LedTaskSetState()  task context, called when a control centre icon is tapped
//!
//! Keeping every GPIO write in LedTask has two benefits: the ISR stays extremely
//! short and calls nothing that might block in interrupt context, and the render
//! task does not have to know about pin polarity or open-drain wiring.
//!
//! This module is fully independent of the existing render / display / sensor /
//! radio tasks: no shared semaphores, no IOM access, and it does not use LED3
//! (GP19), which belongs to ble_led.c.
//
//*****************************************************************************

#ifndef LED_TASK_H
#define LED_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
// LED task handle.
//
//*****************************************************************************
extern TaskHandle_t LedTaskHandle;

//*****************************************************************************
//
//! @brief The controllable LEDs, numbered by board silkscreen.
//!
//! @details The BSP's LED0/1/2 naming is deliberately not reused; that numbering
//! is off by one from the silkscreen and mixing the two will light the wrong LED
//! sooner or later. LED3 (GP19) is not listed -- ble_led.c owns it for BLE status.
//
//*****************************************************************************
typedef enum
{
    LED_ID_LED2 = 0,    // GP11, silkscreen LED2
    LED_ID_LED4,        // GP83, silkscreen LED4
    LED_ID_COUNT
} led_id_e;

//*****************************************************************************
//
//! @brief Minimum button debounce interval (ticks, 1 tick = 1 ms).
//!
//! @details Mechanical bounce on press and release usually settles within a few
//! ms. Edges closer together than this are discarded inside Button_callback();
//! otherwise one press counts as several toggles and the LED looks unresponsive.
//
//*****************************************************************************
#define BUTTON_DEBOUNCE_TICKS       (50)

//*****************************************************************************
//
//! @brief Set the target state of one LED.
//!
//! @details Only updates a flag and wakes LedTask; the actual GPIO write happens
//! in task context, so the caller is never held up by a GPIO operation and does
//! not have to deal with open-drain polarity. Callable from any task context;
//! from an ISR use LedTaskToggleFromISR() instead.
//!
//! @param eLed Target LED
//! @param bOn  true lights it, false turns it off
//
//*****************************************************************************
extern void LedTaskSetState(led_id_e eLed, bool bOn);

//*****************************************************************************
//
//! @brief Toggle the target state of one LED.
//!
//! @param eLed Target LED
//
//*****************************************************************************
extern void LedTaskToggle(led_id_e eLed);

//*****************************************************************************
//
//! @brief Query the current target state of one LED.
//!
//! @details Reports the flag rather than reading GPIO back. Used by the UI to show switch state.
//!
//! @param eLed Target LED
//!
//! @return true when the LED is requested on. Returns false for an out-of-range index.
//
//*****************************************************************************
extern bool LedTaskGetState(led_id_e eLed);

//*****************************************************************************
//
//! @brief Query the on flag of LED4 (GP83).
//!
//! @details Legacy interface, equivalent to LedTaskGetState(LED_ID_LED4), kept for existing callers.
//!
//! @return true when the flag requests on, false when it requests off.
//
//*****************************************************************************
extern bool LedTaskGetFlag(void);

//*****************************************************************************
//
//! @brief Button LED task.
//!
//! @details At task start it configures the GPIOs for both LEDs and BTN0 and
//! registers the button interrupt, then blocks on a task notification; each time
//! it wakes it applies every LED flag to the hardware.
//
//*****************************************************************************
extern void LedTask(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // LED_TASK_H
