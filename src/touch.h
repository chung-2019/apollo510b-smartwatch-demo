//*****************************************************************************
//
//! @file touch.h
//!
//! @brief CHSC5816 touch driver wrapper for the NemaGFX watchface.
//!
//! The public API is deliberately identical to the TMA525 wrapper used on
//! apollo4p_evb_disp_shield_rev2, so application code ports over unchanged.
//! Only the implementation behind it talks to a different touch IC.
//
//*****************************************************************************

#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>
#include <stdbool.h>
#include "am_mcu_apollo.h"

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
//! Touch state reported to the application.
//!
//! CHSC5816 reports up to two fingers and the driver already parses both (see
//! chsc5816_pointinfo_parse()), so the second contact is passed straight
//! through.  Single-finger callers can keep reading x/y/pressed and ignore the
//! rest: those three fields mean exactly what they always did.
//!
//! @note @c ui8Fingers is derived here rather than forwarded verbatim.  The
//!       driver leaves its own @c finger_number at the last non-zero value when
//!       every finger lifts — only @c touch_released goes true — so a caller
//!       trusting that field alone would think two fingers were still down.
//
//*****************************************************************************
typedef struct
{
    uint16_t x;
    uint16_t y;
    bool     pressed;

    //
    //! Second contact, valid only while ui8Fingers == 2.
    //
    uint16_t x2;
    uint16_t y2;

    //
    //! Fingers currently down: 0, 1 or 2.
    //
    uint8_t  ui8Fingers;
} touch_state_t;

//*****************************************************************************
//
//! @brief Initialize the CHSC5816 touch controller.
//!
//! Brings up IOM2 in I2C mode, resets the touch IC, and registers @p
//! pfnIntCallback on the touch interrupt pin (GP98), leaving that interrupt
//! enabled.  All touch I2C traffic then belongs to a single caller, so the
//! callback is free to just notify a task and let it do the reading.
//!
//! @param pfnIntCallback - GPIO callback for the touch interrupt, invoked in
//!                         ISR context by am_hal_gpio_interrupt_service().
//!                         Must not be NULL.
//!
//! @return 0 on success, non-zero on failure.
//
//*****************************************************************************
extern uint32_t touch_init(am_hal_gpio_handler_t pfnIntCallback);

//*****************************************************************************
//
//! @brief Poll touch data from the CHSC5816 (blocking I2C read).
//!
//! Call this from task context after the touch interrupt fires, then read the
//! result with touch_get_info().
//!
//! @return None.
//
//*****************************************************************************
extern void touch_poll(void);

//*****************************************************************************
//
//! @brief Get the current touch state.
//!
//! @param pState - Pointer to touch_state_t to fill with the current state.
//!
//! @return None.
//
//*****************************************************************************
extern void touch_get_info(touch_state_t *pState);

#ifdef __cplusplus
}
#endif

#endif // TOUCH_H
