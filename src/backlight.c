//*****************************************************************************
//
//! @file backlight.c
//!
//! @brief Shared storage for display brightness and the DCS command that applies it.
//!
//! The rationale is documented in backlight.h.
//
//*****************************************************************************

#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

//
// nemadc_mipi_cmd_write() is declared in the platform's nema_sys_defs.h
// (third_party/ThinkSi/config/apollo510_nemagfx/), already on the INCLUDES path.
//
#include "nema_sys_defs.h"

#include "backlight.h"

//*****************************************************************************
//
// MIPI DCS: Write Display Brightness.
//
// The name and the value match the one used for panel init in
// am_devices_dc_dsi_raydium.c. It is redefined here rather than included from
// that driver, which does not export the constant.
//
//*****************************************************************************
#define MIPI_SET_DISPLAY_BRIGHTNESS     (0x51)

//*****************************************************************************
//
// The current level and whether one is pending.
//
// The two must be read and written together: updating the value but missing the
// flag means DisplayTask never sends the command, while updating the flag but
// missing the value sends the previous round's brightness. See backlight.h.
//
//*****************************************************************************
static uint8_t          g_ui8Level = BACKLIGHT_LEVEL_MAX;
static bool             g_bPending;

void
backlight_init(void)
{
    AM_CRITICAL_BEGIN;

    //
    // Matches the 0xff written during panel init, so nothing is marked pending --
    // boot does not spend an extra command setting an identical value, and there
    // is no flicker.
    //
    g_ui8Level = BACKLIGHT_LEVEL_MAX;
    g_bPending = false;

    AM_CRITICAL_END;
}

void
backlight_set_level(uint8_t ui8Level)
{
    //
    // Clamp into the settable range. The reason the lower bound is not 0x00 is in
    // backlight.h: make the screen invisible and the slider to bring it back is
    // gone with it.
    //
    if ( ui8Level < BACKLIGHT_LEVEL_MIN )
    {
        ui8Level = BACKLIGHT_LEVEL_MIN;
    }
    else if ( ui8Level > BACKLIGHT_LEVEL_MAX )
    {
        ui8Level = BACKLIGHT_LEVEL_MAX;
    }

    AM_CRITICAL_BEGIN;

    //
    // Do not mark when the value is unchanged. While dragging the slider, a
    // finger resting on one step yields the same value for several frames in a
    // row, and without this test every frame would send a DSI command.
    //
    if ( ui8Level != g_ui8Level )
    {
        g_ui8Level = ui8Level;
        g_bPending = true;
    }

    AM_CRITICAL_END;
}

uint8_t
backlight_get_level(void)
{
    uint8_t ui8Level;

    AM_CRITICAL_BEGIN;

    ui8Level = g_ui8Level;

    AM_CRITICAL_END;

    return ui8Level;
}

bool
backlight_apply_pending(void)
{
    uint8_t ui8Level;
    bool    bPending;

    //
    // Take the value and the flag inside the critical section and clear the flag
    // there, then send the command outside it: DSI command transmission completes
    // by polling, and wrapping it in a critical section would mask interrupts for
    // tens of microseconds.
    //
    // Clearing the flag before sending is deliberate: if RenderTask sets a new
    // brightness while the command is in flight, the flag is raised again and the
    // next frame sends it. The other order (send, then clear) would wipe that new
    // request and leave the panel at the old value.
    //
    AM_CRITICAL_BEGIN;

    bPending   = g_bPending;
    ui8Level   = g_ui8Level;
    g_bPending = false;

    AM_CRITICAL_END;

    if ( !bPending )
    {
        return false;
    }

#if (DISP_CTRL_IP == DISP_CTRL_IP_DC)
    if ( g_sDispCfg.eInterface == DISP_IF_DSI )
    {
        //
        // Exactly the call shape used by am_devices_dc_dsi_raydium.c: the command
        // argument is 0, the real DCS command byte is the first byte of the
        // parameter buffer, and bDCS is false (the whole thing goes out as a
        // generic write). That driver sets brightness to 0xff this way, the only
        // form verified on this board, so it is copied verbatim.
        //
        uint8_t pui8Cmd[2] =
        {
            MIPI_SET_DISPLAY_BRIGHTNESS,
            ui8Level
        };

        (void)nemadc_mipi_cmd_write(0, pui8Cmd, sizeof(pui8Cmd), false, false);

        //
        // The driver follows each panel init command with DELAY(10); that is not
        // reproduced here. It exists to give the panel time between consecutive
        // init commands, whereas this sends at most one command per frame with a
        // frame transfer immediately after. Stalling 10 ms per frame would more
        // than halve the frame rate while the slider is dragged.
        //
        return true;
    }
#endif

    //
    // The same command shape has not been verified on non-DSI interfaces
    // (SPI4/QSPI panels), so nothing is sent. The flag is already cleared, so
    // there is no retry on every frame.
    //
    return false;
}
