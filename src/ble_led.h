//*****************************************************************************
//
//! @file ble_led.h
//!
//! @brief Bluetooth status indicator LED (LED3 / GP19, active low).
//!
//! Deliberately exposes no Ambiq or BSP types, so the Cordio application layer
//! (hts_main.c) can include it without pulling in am_bsp.h -- which would
//! collide with WSF's own TRUE/FALSE definitions.
//! The blink cadence belongs to the caller's WSF timer; this module only turns
//! the LED on and off.
//
//*****************************************************************************

#ifndef BLE_LED_H
#define BLE_LED_H

#ifdef __cplusplus
extern "C" {
#endif

//*****************************************************************************
//
//! @brief Configure LED3 (GP19) as an output and turn it off.
//
//*****************************************************************************
void BleLedInit(void);

//*****************************************************************************
//
//! @brief Turn LED3 on.
//
//*****************************************************************************
void BleLedOn(void);

//*****************************************************************************
//
//! @brief Turn LED3 off.
//
//*****************************************************************************
void BleLedOff(void);

//*****************************************************************************
//
//! @brief Toggle the current state of LED3.
//
//*****************************************************************************
void BleLedToggle(void);

#ifdef __cplusplus
};
#endif

#endif // BLE_LED_H
