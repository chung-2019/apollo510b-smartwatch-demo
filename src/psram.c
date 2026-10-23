//*****************************************************************************
//
//! @file psram.c
//!
//! @brief Initialization of the external PSRAM on MSPI0 (APS51216BA, 64 MB hex DDR).
//!
//! The sequence follows the SDK's nemagfx_earth_nasa: DDR timing scan -> device
//! init -> enable XIP. The only difference is that a failure does not hang the
//! boot (see psram.h).
//
//*****************************************************************************

#include "psram.h"
#include "am_bsp.h"
#include "am_util.h"

//
// The driver body is pulled in with #include instead of being added to SRC in
// the Makefile.
//
// This is the SDK's own idiom (earth_nasa and
// mspi_hex_ddr_psram_bandwidth_example both do it), because the driver decides
// which part to generate code for from a compile-time macro: USE_APS51216BA in
// am_bsp.h switches it to the x16 hex variant (the APS51216BA on this board),
// and without that macro the same file builds the x8 octal APS25616BA. Compiled
// as a standalone .o, whether the macro is in effect depends on include order,
// which makes it easy to silently build the wrong part.
//
#include "am_devices_mspi_psram_aps25616ba_1p2v.h"
#include "am_devices_mspi_psram_aps25616ba_1p2v.c"

//*****************************************************************************
//
// MSPI / PSRAM handles and configuration.
//
//*****************************************************************************
static void *g_pPsramHandle;
static void *g_pMSPIPsramHandle;
static bool  g_bPsramReady;

//
// Hex DDR at 192 MHz: matches the board's x16 routing and is what the SDK
// examples use for this part. The non-blocking transfer buffer is left empty --
// map tiles are copied by the CPU straight to XIP addresses and never go through
// the MSPI DMA queue.
//
static am_devices_mspi_psram_config_t g_sMspiPsramConfig =
{
    .eDeviceConfig            = AM_HAL_MSPI_FLASH_HEX_DDR_CE0,
    .eClockFreq               = AM_HAL_MSPI_CLK_192MHZ,
    .ui32NBTxnBufLength       = 0,
    .pNBTxnBuf                = NULL,
    .ui32ScramblingStartAddr  = 0,
    .ui32ScramblingEndAddr    = 0,
};

static am_devices_mspi_psram_ddr_timing_config_t g_sMspiDdrTimingConfig;

//*****************************************************************************
//
// MSPI0 interrupt handling.
//
// XIP reads and writes generate no interrupts of their own (the hardware handles
// those bus accesses directly). This exists mainly for the transfers the driver
// performs internally during init, and to keep unexpected interrupts out of the
// default handler.
//
//*****************************************************************************
void
am_mspi0_isr(void)
{
    uint32_t ui32Status;

    am_hal_mspi_interrupt_status_get(g_pMSPIPsramHandle, &ui32Status, false);
    am_hal_mspi_interrupt_clear(g_pMSPIPsramHandle, ui32Status);
    am_hal_mspi_interrupt_service(g_pMSPIPsramHandle, ui32Status);
}

//*****************************************************************************
//
// Initialize the PSRAM.
//
//*****************************************************************************
uint32_t
psram_init(void)
{
    uint32_t ui32Status;

    g_bPsramReady = false;

    //
    // One line at start and one at the end, so the RTT timestamps measure how
    // long this section takes.
    //
    // This happens before the scheduler starts, so it adds directly to boot time
    // -- and boot latency has always been a sensitive topic in this project (see
    // docs/BLE_STARTUP_DELAY_INVESTIGATION.md). No timer is read here because
    // there is no reliable time base yet: the FreeRTOS tick has not started, and
    // the state of the timer module at this point depends on BSP init details.
    //
    am_util_debug_printf("[psram] init start (DDR timing scan)\n");

    //
    // DDR timing scan.
    //
    // Finds the RxDQS delay usable for this part on this board. Read timing
    // margin at 192 MHz DDR is small and shifts with board routing and
    // temperature, so it is rescanned at every boot rather than hard-coded.
    //
    ui32Status = am_devices_mspi_psram_aps25616ba_ddr_init_timing_check(
                     PSRAM_MSPI_MODULE, &g_sMspiPsramConfig,
                     &g_sMspiDdrTimingConfig);
    if ( ui32Status != AM_DEVICES_MSPI_PSRAM_STATUS_SUCCESS )
    {
        am_util_debug_printf("[psram] DDR timing scan failed (0x%x)\n",
                             ui32Status);
        return ui32Status;
    }

    am_util_debug_printf("[psram] timing scan ok, RXDQSDELAY0 = %d\n",
                         g_sMspiDdrTimingConfig.sTimingCfg.ui8RxDQSDelay);

    //
    // Configure the MSPI and the PSRAM device.
    //
    ui32Status = am_devices_mspi_psram_aps25616ba_ddr_init(
                     PSRAM_MSPI_MODULE, &g_sMspiPsramConfig,
                     &g_pPsramHandle, &g_pMSPIPsramHandle);
    if ( ui32Status != AM_DEVICES_MSPI_PSRAM_STATUS_SUCCESS )
    {
        am_util_debug_printf("[psram] device init failed (0x%x)\n", ui32Status);
        return ui32Status;
    }

    NVIC_SetPriority(MSPI0_IRQn, 5);
    NVIC_EnableIRQ(MSPI0_IRQn);

    //
    // Apply the timing the scan just found.
    //
    ui32Status = am_devices_mspi_psram_aps25616ba_apply_ddr_timing(
                     g_pPsramHandle, &g_sMspiDdrTimingConfig);
    if ( ui32Status != AM_DEVICES_MSPI_PSRAM_STATUS_SUCCESS )
    {
        am_util_debug_printf("[psram] apply timing failed (0x%x)\n",
                             ui32Status);
        return ui32Status;
    }

    //
    // Switch to XIP: from this point the whole PSRAM is directly addressable.
    //
    ui32Status = am_devices_mspi_psram_aps25616ba_ddr_enable_xip(g_pPsramHandle);
    if ( ui32Status != AM_DEVICES_MSPI_PSRAM_STATUS_SUCCESS )
    {
        am_util_debug_printf("[psram] enable XIP failed (0x%x)\n", ui32Status);
        return ui32Status;
    }

    g_bPsramReady = true;

    am_util_debug_printf("[psram] ready: %u MB @ 0x%08X (hex DDR 192MHz)\n",
                         (uint32_t)(PSRAM_SIZE_BYTES / (1024 * 1024)),
                         (uint32_t)PSRAM_XIP_BASE);

    return 0;
}

//*****************************************************************************
//
// Query whether the PSRAM is usable.
//
//*****************************************************************************
bool
psram_is_ready(void)
{
    return g_bPsramReady;
}
