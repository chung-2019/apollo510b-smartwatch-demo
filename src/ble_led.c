//*****************************************************************************
//
//! @file ble_led.c
//!
//! @brief Bluetooth status indicator LED (LED3 / GP19, active low).
//!
//! GP19 is AM_BSP_GPIO_LED1 in the BSP (silkscreened LED3 on the board).
//!
//! am_devices_led is deliberately not used here. The polarity field in
//! am_bsp_psLEDs[] only carries AM_DEVICES_LED_ON_LOW (0x0) and omits the
//! AM_DEVICES_LED_POL_OPEN_DRAIN bit, so am_devices_led_init() takes the
//! tristate branch: it overrides the BSP open-drain config for LED1 with
//! am_hal_gpio_pincfg_tristate and switches to output-enable as the control.
//! Applying the BSP's own g_AM_BSP_GPIO_LED1 (OUTCFG = OPENDRAIN) matches how
//! the board is actually wired: pull low to light, release to extinguish.
//
//*****************************************************************************

#include "ble_led.h"

#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

#define BLE_LED_PIN     AM_BSP_GPIO_LED1

//
// Open-drain, active low: CLEAR pulls the pin low and lights the LED, SET
// releases it and lets the LED go out.
//
#define BLE_LED_ON_STATE    AM_HAL_GPIO_OUTPUT_CLEAR
#define BLE_LED_OFF_STATE   AM_HAL_GPIO_OUTPUT_SET

static bool g_bBleLedOn = false;

void
BleLedInit(void)
{
    uint32_t ui32Value = 0;

    am_hal_gpio_pinconfig(BLE_LED_PIN, g_AM_BSP_GPIO_LED1);

    BleLedOff();

    //
    // Read the pin back to confirm the configuration took effect -- the first
    // thing to check when the LED stays dark.
    //
    am_hal_gpio_state_read(BLE_LED_PIN, AM_HAL_GPIO_OUTPUT_READ, &ui32Value);
    am_util_debug_printf("[led] GP%d configured, output = %d\n",
                         BLE_LED_PIN, ui32Value);
}

void
BleLedOn(void)
{
    am_hal_gpio_state_write(BLE_LED_PIN, BLE_LED_ON_STATE);
    g_bBleLedOn = true;
}

void
BleLedOff(void)
{
    am_hal_gpio_state_write(BLE_LED_PIN, BLE_LED_OFF_STATE);
    g_bBleLedOn = false;
}

void
BleLedToggle(void)
{
    if (g_bBleLedOn)
    {
        BleLedOff();
    }
    else
    {
        BleLedOn();
    }
}
