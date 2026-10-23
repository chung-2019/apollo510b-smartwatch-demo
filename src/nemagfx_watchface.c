//*****************************************************************************
//
//! @file nemagfx_watchface.c
//!
//! @brief NemaGFX Watchface Example - Real-time Clock Display
//!
//! @addtogroup graphics_examples Graphics Examples
//!
//! @defgroup nemagfx_watchface NemaGFX Watchface Example
//! @ingroup graphics_examples
//! @{
//!
//! Purpose: This example demonstrates the creation of a watch face
//! application using NemaGFX graphics library. The watch face features:
//!
//! - Real-time clock display with rotating hour, minute, and second hands
//! - Background image rendering with proper scaling and positioning
//! - SVG format weather icon display with dynamic updates
//! - Power-optimized rendering with GPU and display power management
//!
//! @section nemagfx_watchface_features Key Features
//!
//! 1. @b Rotating @b Clock @b Hands: Uses raw NemaGFX APIs for smooth hand rotation
//!    with proper pivot point calculations and matrix transformations
//!
//! 2. @b GPU @b Power @b Management: Implements intelligent GPU power cycling where
//!    the GPU powers off after command list execution and restarts for next frame
//!
//! 3. @b Display @b Power @b Optimization: DC and DSI are powered up only during
//!    frame transfer operations and immediately shut down after completion
//!
//! 4. @b Multi-threaded @b Architecture: Uses FreeRTOS tasks for rendering and
//!    display management with proper synchronization
//!
//! @section nemagfx_watchface_hardware Hardware Requirements
//!
//! - Compatible Development Board
//! - Display panel with DSI interface
//! - External power supply for stable operation
//!
//! @section nemagfx_watchface_usage Usage
//!
//! The application automatically starts displaying the watch face upon boot.
//! The clock hands update in real-time, and weather icons cycle periodically.
//!
//! @note This example requires proper display configuration and external power
//! for optimal performance and power management features.
//!
//*****************************************************************************

//*****************************************************************************
//
// Copyright (c) 2026, Ambiq Micro, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// Third party software included in this distribution is subject to the
// additional license terms as defined in the /docs/licenses directory.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// This is part of revision release_sdk5p2p0-66487dd10 of the AmbiqSuite Development Package.
//
//*****************************************************************************
#include "nemagfx_watchface.h"
#include "rtos.h"
#include "psram.h"
#include "asset_store.h"
#include "asset_provision.h"

#if defined(AM_USE_RTT_LOG)
#include "rtt_log.h"
#endif

//*****************************************************************************
//
//! @brief Main application entry point
//!
//! @details Initializes the system, configures power management, and starts
//! the FreeRTOS tasks for watch face rendering and display management.
//!
//! @return int Returns 0 on successful execution (never returns)
//!
//*****************************************************************************
int
main(void)
{
#ifndef NOFPU
    //
    // Enable the FPU and turn on lazy stacking.
    //
    // These two lines originally lived only in main() of
    // ble_freertos_tmp119_hts and were missed during the port. Lazy stacking
    // decides how FPU registers are preserved across exceptions and interrupts
    // -- and deep-sleep wake-up runs through the interrupt path, so this cannot
    // be omitted once BLE is layered on top.
    //
    am_hal_sysctrl_fpu_enable();
    am_hal_sysctrl_fpu_stacking_enable(true);
#else
    am_hal_sysctrl_fpu_disable();
#endif

    //
    // External power on
    //
    am_bsp_external_pwr_on();

    //
    // Configure the board for low power.
    //
    am_bsp_low_power_init();

    am_hal_cachectrl_icache_enable();
    am_hal_cachectrl_dcache_enable(true);

    //
    // Initialize the printf interface.
    //
#if defined(AM_USE_RTT_LOG)
    //
    // Route am_util_stdio_printf() to SEGGER RTT channel 0.  RTT needs no extra
    // wiring beyond the existing SWD link and does not stall the render task
    // when no host is attached (see rtt_log.c).
    //
    rtt_log_init();
#else
    //
    // ITM/SWO output.
    //
    am_bsp_itm_printf_enable();
#endif

    //
    // Clear the terminal and print the banner.
    //
    am_util_stdio_terminal_clear();

    //
    // Global interrupt enable
    //
    am_hal_interrupt_master_enable();

    //
    // Print debug information.
    //
    am_util_debug_printf("FreeRTOS nemagfx_watchface Example\n");

    //
    // Asset filesystem on eMMC (SDIO0). The home image, the watch faces, the
    // hands and the map tiles all live here.
    //
    // It is ordered before PSRAM so the log stays visible: the PSRAM DDR timing
    // scan floods dozens of printf lines, and in NO_BLOCK_TRIM mode the RTT ring
    // buffer discards whatever no longer fits, taking the eMMC error messages
    // ("No card is present", "card config set failed", both from the driver)
    // with it. The two have no dependency, so the order is free to choose.
    //
    // Like PSRAM, this must run before the scheduler: eMMC init resets the SDHC
    // controller and must not interleave with anyone else's accesses. If the
    // mount fails the screen loses its images, but vector-drawn content such as
    // the watch hands and the digits on the temperature page is unaffected.
    //
    if ( AssetStoreInit() )
    {
        //
        // Factory provisioning: if eMMC is blank, write the assets built into
        // the firmware. Existing files are left untouched -- artwork the user
        // replaced over USB must not be overwritten by the boot path.
        //
        (void)AssetProvisionRun();
    }

    //
    // External PSRAM (MSPI0, 64 MB). The map navigation page keeps its tiles here.
    //
    // Must complete before the scheduler starts: the DDR timing scan inside
    // init resets MSPI repeatedly and must not interleave with other tasks'
    // accesses to that bus. A failure does not stop boot; the map page simply
    // reports itself as unavailable (see psram.h).
    //
    (void)psram_init();

    //
    // Boot result summary.
    //
    // Deliberately last: the PSRAM DDR timing scan prints dozens of lines of
    // scan progress, and in NO_BLOCK_TRIM mode the RTT ring buffer just drops
    // what does not fit, so the earlier [psram] / [asset] results often vanish
    // entirely. Printing once more after the flood makes it obvious at a glance
    // whether both subsystems came up.
    //
    am_util_stdio_printf("[boot] psram=%s emmc=%s\n",
                         psram_is_ready() ? "ok" : "FAIL",
                         AssetStoreIsReady() ? "ok" : "FAIL");

    //
    // Run the application.
    //
    run_tasks();

    //
    // We shouldn't ever get here.
    //
    while (1)
    {
    }
}

//*****************************************************************************
//
// End Doxygen group.
//! @}
//
//*****************************************************************************

