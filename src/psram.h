//*****************************************************************************
//
//! @file psram.h
//!
//! @brief Initialization and address space of the external PSRAM on MSPI0.
//!
//! apollo510b_evb carries an APS51216BA on MSPI0 (512 Mbit = 64 MB, x16 hex
//! DDR). Once XIP is enabled the whole PSRAM appears in the address space from
//! 0x60000000 and is directly addressable by both the CPU and the GPU -- the map
//! tiles live there so the GPU can sample them as textures without first copying
//! them into the 3 MB of SSRAM.
//!
//! The display runs over DSI and the NOR flash sits on MSPI1, so PSRAM is the
//! only user of the whole MSPI0 bus; no bus sharing is handled here.
//
//*****************************************************************************

#ifndef PSRAM_H
#define PSRAM_H

#include <stdint.h>
#include <stdbool.h>
#include "am_mcu_apollo.h"

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
//! @brief Which MSPI instance the PSRAM is attached to.
//!
//! @details The board routing fixes this value (CE GP199 / CLK GP72 / D0-D15
//! GP64-71, GP37-44); it is not a configurable option. MSPI1 carries the NOR
//! flash (AM_BSP_MSPI_FLASH_MODULE), so the two do not conflict.
//
//*****************************************************************************
#define PSRAM_MSPI_MODULE       (0)

//*****************************************************************************
//
//! @brief Base address and size of the PSRAM in XIP mode.
//!
//! @details The apertures of the MSPI instances are 64 MB apart, and MSPI0 lands
//! at 0x60000000. The size comes from the actual part, APS51216BA: 512 Mbit / 8 = 64 MB.
//!
//! Can the GPU sample this region? Yes -- the official nemagfx_performance_test
//! places textures at MSPI_XIP_BASE_ADDRESS to measure GPU read bandwidth from
//! PSRAM, and RGB565 is one of the formats tested. This is the opposite of MRAM,
//! which the GPU cannot sample.
//
//*****************************************************************************
#define PSRAM_XIP_BASE          (MSPI0_APERTURE_START_ADDR)
#define PSRAM_SIZE_BYTES        (64 * 1024 * 1024)

//*****************************************************************************
//
//! @brief How the PSRAM is divided.
//!
//! @details Each user has a fixed, non-overlapping region, listed here rather
//! than scattered across the .c files -- otherwise a new user cannot see what the
//! others already occupy:
//!
//!     0x60000000  map tile cache        map_task.c     3 slots x 428 KB = 1.25 MB
//!     0x60400000  weather page assets   render_task.c  5 backgrounds x 428 KB
//!                                                      + 536 KB overlay = 2.62 MB
//!
//! Each region reserves 4 MB, far more than it uses today: the number of tile
//! cache slots follows the map's minimum zoom (see MAP_CACHE_SLOTS in
//! map_task.h), and 64 MB can spare it. Each user checks at compile time that it
//! stays inside its own region.
//
//*****************************************************************************
#define PSRAM_MAP_BASE          (PSRAM_XIP_BASE)
#define PSRAM_MAP_SIZE          (4 * 1024 * 1024)

#define PSRAM_WEATHER_BASE      (PSRAM_MAP_BASE + PSRAM_MAP_SIZE)
#define PSRAM_WEATHER_SIZE      (4 * 1024 * 1024)

//*****************************************************************************
//
//! @brief Initialize MSPI0 and the PSRAM, then switch to XIP mode.
//!
//! @details Three steps in order: DDR timing scan, device init, enable XIP. The
//! timing scan is mandatory for hex DDR at 192 MHz and finds the RxDQS delay
//! usable for this part on this board; skipping it gives intermittently corrupt
//! reads.
//!
//! A failure is reported but does not hang the boot -- the map page reports
//! itself unavailable while the watch face and temperature pages are entirely
//! unaffected. The official example has a while(1) here, which is reasonable for
//! a test program that only exercises PSRAM and unreasonable for a watch.
//!
//! Must be called before the FreeRTOS scheduler starts, in single-threaded
//! context: the timing scan resets MSPI repeatedly and must not interleave with
//! anyone else's accesses to that bus.
//!
//! @return 0 on success, non-zero is an am_devices_mspi_psram error code.
//
//*****************************************************************************
extern uint32_t psram_init(void);

//*****************************************************************************
//
//! @brief Whether the PSRAM is addressable yet.
//!
//! @details True only after psram_init() succeeds. Code that uses the PSRAM (the
//! map tile cache) always asks here first; otherwise an access to 0x60000000 is a
//! bus fault rather than a broken image.
//!
//! @return true when XIP is enabled.
//
//*****************************************************************************
extern bool psram_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif // PSRAM_H
