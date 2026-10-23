//*****************************************************************************
//
//! @file touch.c
//!
//! @brief CHSC5816 touch driver wrapper used by the NemaGFX watch face.
//!
//! apollo510b_evb carries a CHSC5816 touch IC on IOM2 (SCL=GPIO25,
//! SDA=GPIO26, INT=GPIO98, RST=GPIO99), unlike apollo4p_evb_disp_shield_rev2
//! which uses a TMA525. Their register maps, I2C addresses and driver APIs all
//! differ; only the reported touch_info structure happens to be identical.
//!
//! Sampling is now triggered by the touch interrupt on GP98 (see touch_task.c),
//! but the I2C read itself is still blocking polling, executed in TouchTask's
//! task context rather than in an ISR.
//!
//! Why the driver's non-blocking variant is not used (the kind in event_task.c
//! of nemagfx_earth_nasa): a non-blocking read has to start the transfer in the
//! ISR and hand the data over through a completion callback, buying back the CPU
//! spent blocking. But one read here is only ~16 bytes at 400 kHz, about 400 us,
//! and it happens only while a finger is actually on the screen. The cost of
//! blocking TouchTask for that is far below the complexity of another callback
//! state machine. IOM2 belongs to touch alone (the TMP119 is on IOM1), so
//! blocking holds nobody else up either.
//
//*****************************************************************************

#include "touch.h"
#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"
#include "am_devices_display_generic.h"
#include "am_devices_chsc5816_ap5.h"

//*****************************************************************************
//
// Globals
//
//*****************************************************************************

//
// DMA transfer control buffer. Only used if the application later switches to
// non-blocking reads, but am_devices_chsc5816_init() requires it up front.
//
#define TOUCH_DMA_BUFF_SIZE     256
AM_SHARED_RW uint32_t g_ui32TouchDMATCBBuffer[TOUCH_DMA_BUFF_SIZE];

static void *g_pCHSC5816Handle    = NULL;
static void *g_pIOMCHSC5816Handle = NULL;
static bool  g_bTouchReady        = false;

static am_devices_iom_chsc5816_config_t g_sTouchI2cConfig =
{
    .ui32ClockFreq      = AM_HAL_IOM_400KHZ,
    .ui32NBTxnBufLength = TOUCH_DMA_BUFF_SIZE,
    .pNBTxnBuf          = g_ui32TouchDMATCBBuffer
};

static am_devices_tc_chsc5816_info_t g_sTouchInfo =
{
    .x0 = 0, .y0 = 0,
    .x1 = 0, .y1 = 0,
    .dx0 = 0, .dy0 = 0,
    .dx1 = 0, .dy1 = 0,
    .event = 0, .state = 0,
    .kb_event = 0, .kb_key = 0,
    .finger_number = 0,
    .touch_released = true,
    .timer_id = 0, .timer_expirations = 0,
    .fw_ver = 0
};

//*****************************************************************************
//
// Takes over the IOM interrupt handler for the bus the touch panel uses.
//
//*****************************************************************************
#define touch_iom_isr                                                          \
    am_iom_isr1(AM_BSP_TP_IOM_MODULE)
#define am_iom_isr1(n)                                                         \
    am_iom_isr(n)
#define am_iom_isr(n)                                                          \
    am_iomaster ## n ## _isr

//*****************************************************************************
//
//! @brief IOM interrupt service routine for the touch panel bus.
//!
//! Blocking transfers wait for completion by polling, so this routine normally
//! never fires. Keeping it means an unexpected IOM interrupt is handled instead
//! of falling into the default handler, and lets the driver move to non-blocking
//! reads later without any extra change to the interrupt architecture.
//!
//! The two levels of test here check two different things and are easy to confuse:
//!
//!   Outer  did the call itself succeed (is the handle valid)
//!   Inner  is there actually an interrupt bit to service
//!
//! This is because am_hal_iom_interrupt_status_get() returns the interrupt state
//! through an out-param (ui32Status) while its return value is an am_hal_status_e
//! error code, and AM_HAL_STATUS_SUCCESS is the first member of that enum (value
//! 0). Vendor examples therefore often write
//! "if ( !am_hal_iom_interrupt_status_get(...) )" -- where the ! means "succeeded"
//! and not "no interrupt". The comparison is explicit here to avoid that reading.
//!
//! Compare am_gpio0_001f_isr() in led_task.c: the GPIO version simply discards
//! the return value, because its only failure mode is an invalid IRQn and
//! GPIO0_001F_IRQn is a compile-time constant that cannot be wrong. The IOM
//! version has a runtime handle, which is why it has to be checked.
//
//*****************************************************************************
void
touch_iom_isr(void)//IOM2
{
    uint32_t ui32Status;

    //
    // Before touch_init() has run (or after it failed) the handle is still NULL,
    // and calling into the HAL would only return AM_HAL_STATUS_INVALID_HANDLE;
    // rejecting it here is clearer.
    //
    if ( g_pIOMCHSC5816Handle == NULL )
    {
        return;
    }

    if ( am_hal_iom_interrupt_status_get(g_pIOMCHSC5816Handle, true, &ui32Status)
            != AM_HAL_STATUS_SUCCESS )
    {
        return;
    }

    if ( ui32Status == 0 )
    {
        return;
    }

    //
    // Clearing must happen before servicing: a new interrupt arriving during
    // service would otherwise be cleared along with the old state. (Same reason
    // as the GPIO ISR in led_task.c.)
    //
    am_hal_iom_interrupt_clear(g_pIOMCHSC5816Handle, ui32Status);
    am_hal_iom_interrupt_service(g_pIOMCHSC5816Handle, ui32Status);
}

//*****************************************************************************
//
//! @brief Initialize the CHSC5816 touch controller.
//
//*****************************************************************************
uint32_t
touch_init(am_hal_gpio_handler_t pfnIntCallback)
{
    uint32_t ui32Status;

    if ( pfnIntCallback == NULL )
    {
        return AM_DEVICES_CHSC5816_STATUS_ERROR;
    }

    //
    // The CHSC5816 on this board is paired with the CO5300 display panel.
    // A different panel means a different touch IC, or no touch IC at all.
    //
    if ( g_sDispCfg.eIC != DISP_IC_CO5300 )
    {
        am_util_debug_printf("[touch] unsupported panel, touch disabled\n");
        return AM_DEVICES_CHSC5816_STATUS_ERROR;
    }

    //
    // Hardware connections of the CHSC5816 touch IC:
    //   I2C: IOM2 (SCL=GPIO25, SDA=GPIO26)
    //   Touch interrupt: GPIO98
    //   Touch reset: GPIO99
    // The BSP macros below pass these IOM/GPIO settings to the touch driver.
    //
    ui32Status = am_devices_chsc5816_init(AM_BSP_TP_IOM_MODULE,
                                          &g_sTouchI2cConfig,
                                          &g_pCHSC5816Handle,
                                          &g_pIOMCHSC5816Handle,
                                          AM_BSP_GPIO_TOUCH_INT,
                                          AM_BSP_GPIO_TOUCH_RST,
                                          pfnIntCallback,
                                          NULL);
    if ( ui32Status != AM_DEVICES_CHSC5816_STATUS_SUCCESS )
    {
        am_util_debug_printf("[touch] chsc5816 init failed (%u)\n", ui32Status);
        return ui32Status;
    }

    //
    // The GP98 interrupt stays enabled -- am_devices_chsc5816_init() has already
    // registered the callback and turned it on, and unlike before this revision
    // it is no longer disabled here.
    //
    // The earlier reason for disabling it was that "an I2C read issued from the
    // ISR would contend with the render loop's polled read over the same bus".
    // That concern assumed the render loop reads I2C itself, whereas touch I2C
    // access is now all in TouchTask and the ISR only issues a single task
    // notification (see touch_int_callback() in touch_task.c). The source of the
    // conflict is gone.
    //
    // IOM2 belongs to touch alone (the TMP119 is on IOM1, see tmp119.c), so there
    // is no cross-module contention either.
    //
    // The ISR body is not defined in this project: GP98 -> 98/32 = 3 ->
    // GPIO0_607F_IRQn, the same line as the EM9305 INT (GPIO117), so the BSP's
    // existing am_gpio0_607f_isr is reused (see the note in radio_task.c).
    // am_hal_gpio_interrupt_service() dispatches by pin.
    //

    g_bTouchReady = true;
    am_util_debug_printf("[touch] chsc5816 ready on IOM%u\n", AM_BSP_TP_IOM_MODULE);

    return AM_DEVICES_CHSC5816_STATUS_SUCCESS;
}

//*****************************************************************************
//
//! @brief Poll the CHSC5816 touch data with a blocking I2C read.
//
//*****************************************************************************
void
touch_poll(void)
{
    if ( !g_bTouchReady )
    {
        return;
    }

    am_devices_chsc5816_get_point(g_pCHSC5816Handle, &g_sTouchInfo);
}

//*****************************************************************************
//
//! @brief Get the current touch state.
//
//*****************************************************************************
void
touch_get_info(touch_state_t *pState)
{
    if ( pState == NULL )
    {
        return;
    }

    pState->x       = g_sTouchInfo.x0;
    pState->y       = g_sTouchInfo.y0;
    pState->pressed = !g_sTouchInfo.touch_released;

    pState->x2      = g_sTouchInfo.x1;
    pState->y2      = g_sTouchInfo.y1;

    //
    // The finger count cannot simply forward the driver's finger_number.
    //
    // It is written only when a 1-finger or 2-finger packet arrives and is never
    // zeroed when all fingers lift -- the driver only sets touch_released to true
    // (see chsc5816_pointinfo_parse() in am_devices_chsc5816_ap5.c). Copying it
    // verbatim would leave this field stuck at 2 after a two-finger pinch ends,
    // and the next single-finger drag would be misread as a continuing pinch.
    //
    // So touch_released acts as the gate: nothing held down means zero fingers.
    //
    pState->ui8Fingers = pState->pressed ? g_sTouchInfo.finger_number : 0;
}
