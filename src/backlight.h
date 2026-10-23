//*****************************************************************************
//
//! @file backlight.h
//!
//! @brief Display brightness: written by the UI, applied by DisplayTask.
//!
//! The CO5300 is a self-emissive AMOLED. The board has no separate LED backlight
//! and no BL_PWM-style pin -- brightness is set directly on the panel through
//! MIPI DCS 0x51 (Write Display Brightness). am_devices_display_generic.h in
//! AmbiqSuite 5.2.0 exposes no API for it, and only am_devices_dc_dsi_raydium.c
//! writes 0xff once during panel init, so runtime control has to be added as
//! this extra layer.
//!
//! Why this is split into a "write" step and an "apply" step:
//!
//! DC/DSI is awake only between the moment DisplayTask takes g_semDisplayStart
//! and the end of the frame transfer; the rest of the time it is in deep sleep
//! (see display_task.c). Sending commands while DSI is unpowered is pointless,
//! and RenderTask handles touch exactly during that sleep window. So RenderTask
//! only calls backlight_set_level() to record the desired value, and the command
//! itself is issued by DisplayTask in backlight_apply_pending().
//!
//! This also settles a second question: the display interface is accessed by
//! DisplayTask alone, so DSI needs no extra mutex and a brightness command can
//! never interleave with a frame transfer.
//!
//! The writer is RenderTask and the reader is DisplayTask. They run at different
//! priorities with no other synchronization, so the value and the "pending" flag
//! must be read and written as a pair, wrapped in AM_CRITICAL (the same approach
//! as ble_pair.c; the critical section holds nothing but a few assignments).
//
//*****************************************************************************

#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <stdbool.h>
#include <stdint.h>

//*****************************************************************************
//
//! Settable brightness range.
//!
//! DCS 0x51 takes an 8-bit value: 0x00 is darkest, 0xff is brightest.
//!
//! The lower bound is deliberately not 0x00:
//!
//!   1. On this panel 0x00 does not mean off, only darkest, and how dark that
//!      actually is depends on the panel firmware (turning the screen off is
//!      Display Off, DCS 0x28).
//!   2. More importantly, usability -- brightness is adjusted with an on-screen
//!      slider, and once the screen is too dark to see, the slider to bring it
//!      back cannot be found either. Keep a floor at which the UI stays legible.
//!
//! 0x18 is roughly 9% of full brightness.
//
//*****************************************************************************
#define BACKLIGHT_LEVEL_MIN     (0x18)
#define BACKLIGHT_LEVEL_MAX     (0xff)

//*****************************************************************************
//
//! @brief Initialize the brightness state.
//!
//! @details Only sets the initial value this module tracks and sends no command
//! -- at the point it is called (RenderTask startup) DC/DSI is still asleep, and
//! panel init has already set brightness to 0xff (am_devices_dc_dsi_raydium.c).
//! The two agree, so boot does not flicker.
//
//*****************************************************************************
extern void backlight_init(void);

//*****************************************************************************
//
//! @brief Set the desired brightness.
//!
//! @param ui8Level Brightness value, clamped between BACKLIGHT_LEVEL_MIN and MAX
//!
//! @details Called by the UI (RenderTask). It only records the value and marks it
//! pending; no hardware is touched. An unchanged value is not marked, so holding
//! the slider on one step does not resend the command.
//
//*****************************************************************************
extern void backlight_set_level(uint8_t ui8Level);

//*****************************************************************************
//
//! @brief Read the current brightness.
//!
//! @return The current brightness value (BACKLIGHT_LEVEL_MIN to MAX)
//!
//! @details For drawing the UI slider. It returns the value that was requested,
//! not one read back from the panel -- DCS 0x51 has a read counterpart, but
//! waking DSI for it is not worth it, and nothing outside this program changes
//! the value anyway.
//
//*****************************************************************************
extern uint8_t backlight_get_level(void);

//*****************************************************************************
//
//! @brief Send the DCS command when a brightness change is pending.
//!
//! @details **Must be called by DisplayTask after DC/DSI has woken up**, and it
//! must be the only task that accesses the display interface. Called at any other
//! time (from RenderTask, for instance) DSI may be in deep sleep and the command
//! would be lost or cause undefined behaviour.
//!
//! With nothing pending it returns immediately without any bus access, so calling
//! it every frame costs nothing measurable.
//!
//! @return true when a command was actually sent
//
//*****************************************************************************
extern bool backlight_apply_pending(void);

#endif // BACKLIGHT_H
