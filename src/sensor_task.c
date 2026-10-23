//*****************************************************************************
//
//! @file sensor_task.c
//!
//! @brief Sensor acquisition task.
//!
//! Moves the TMP119 I2C read out of the render loop into a separate task that
//! samples on a fixed period and caches the result for other tasks. This serves
//! two purposes:
//!
//! 1. Temperature updates are no longer tied to the UI page. Previously a read
//!    only happened while the temperature page was on screen; swiping away froze
//!    the value at the last reading, and reporting that over BLE later would
//!    hand the peer data that looks plausible but is long stale.
//!
//! 2. Concurrent IOM1 access is eliminated. The cache is the only external
//!    interface and SensorTask is the only task touching the TMP119, so the IOM
//!    handle needs no lock.
//!
//! am_hal_iom_blocking_transfer() is PIO polling and one read takes about 120 us.
//! Being preempted inside the lowest-priority task costs nothing, so there is no
//! need for non-blocking DMA transfers.
//
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

//*****************************************************************************
//
// Project common headers.
//
//*****************************************************************************
#include "nemagfx_watchface.h"
#include "sensor_task.h"
#include "tmp119.h"
#include "render_task.h"
#include "display_task.h"
#include "radio_task.h"
#include "ble_diag.h"

//*****************************************************************************
//
// WATCHFACE_RTOS_DIAG
//
// When set to 1, SensorTask additionally prints the FreeRTOS heap headroom and
// each task's stack high water mark every sampling period. Useful for diagnosing
// "the screen freezes after running for a while": a gradually exhausted heap or
// a task running out of stack both show up in the numbers first.
//
// Set it back to 0 once diagnosis is complete, and return
// INCLUDE_uxTaskGetStackHighWaterMark in FreeRTOSConfig.h to 0 as well.
//
//*****************************************************************************
#define WATCHFACE_RTOS_DIAG     0

//*****************************************************************************
//
// WATCHFACE_SENSOR_LOG
//
// 1 = print one temperature line to RTT every sampling period
// 0 = print nothing (used while diagnosing BLE)
//
// am_util_stdio_printf() shares a single static buffer with no lock. With WSF
// trace enabled for BLE diagnosis, turning this off reduces interleaving and
// keeps the BLE trace readable.
//
//*****************************************************************************
#define WATCHFACE_SENSOR_LOG    1

//*****************************************************************************
//
// WATCHFACE_BLE_IRQ_DIAG
//
// Diagnose whether the EM9305 INT path really works. Printed every sampling period:
//
//   irq   = g_ui32InterruptsSeen in hci_drv_em9305.c (HciDrvIntService entries)
//   pin   = current GPIO117 level (high = the EM9305 has data waiting for the host)
//   nvic  = whether GPIO0_607F_IRQn is enabled in the NVIC
//
// Two things to watch when reading it, or it is easy to misinterpret:
//
//   * g_ui32InterruptsSeen is zeroed inside HciDrvRadioBoot(), so it is not a
//     count that accumulates across restarts. A number going down or back to 0
//     means the controller was just rebooted.
//   * A moving count only proves HciDrvIntService() was entered; it does not
//     mean every HCI response was fully processed. To find which layer a
//     response stalls at, look at the tx cmd / rx / cmd cmpl sequence in
//     [bledbg] (see ble_diag.h).
//
// How to read it:
//   irq stays 0             -> the interrupt never reached HciDrvIntService. The
//                              only thing that can wake HciDrvHandler() is then
//                              WsfSetEvent() from hciDrvWrite() (the handler's
//                              own GPIO_INT polling does not happen by itself),
//                              so the 10 s heartbeat looks like "one step every
//                              10 seconds". But hciCmdTimeout() is also 10 s, so
//                              only [bledbg] tells which one is at work.
//   irq keeps growing       -> interrupts do arrive; continue towards the driver
//                              handler, the SPI read and the HCI completion event.
//   nvic = 0                -> the NVIC has not enabled that IRQ.
//   pin high, irq frozen    -> the EM9305 has data waiting, but the interrupt is
//                              not firing or not being cleared.
//
//*****************************************************************************
#define WATCHFACE_BLE_IRQ_DIAG  1

#if WATCHFACE_BLE_IRQ_DIAG
//
// Defined in
// third_party/cordio/ble-host/sources/hci/ambiq/em9305/hci_drv_em9305.c (not
// static, so it can be referenced directly).
//
extern volatile uint32_t g_ui32InterruptsSeen;
#endif

//*****************************************************************************
//
// Sensor task handle.
//
//*****************************************************************************
TaskHandle_t SensorTaskHandle;

//*****************************************************************************
//
// Temperature cache.
//
// Both variables are 32-bit and naturally aligned, so a single read or write is
// atomic on Cortex-M55; sharing them across tasks needs only volatile, no mutex
// and no critical section.
//
// Until a valid reading arrives, g_ui32TempStampTick stays 0 and the temperature
// is TMP119_TEMP_INVALID, which is how temp_cache_get() reports failure.
//
//*****************************************************************************
static volatile int32_t  g_i32TempC100Cache  = TMP119_TEMP_INVALID;
static volatile uint32_t g_ui32TempStampTick = 0;
static volatile bool     g_bTempCacheValid   = false;

//*****************************************************************************
//
// Get the most recent successfully read temperature. See sensor_task.h for details.
//
//*****************************************************************************
uint32_t
temp_cache_get(int32_t *pi32TempC100, uint32_t ui32MaxAgeTicks)
{
    int32_t  i32Value;
    uint32_t ui32Stamp;

    if ( pi32TempC100 == NULL )
    {
        return 1;
    }

    if ( !g_bTempCacheValid )
    {
        *pi32TempC100 = TMP119_TEMP_INVALID;
        return 1;
    }

    //
    // The read order is the reverse of SensorTask's write order: value first,
    // then timestamp. SensorTask writes the value before the timestamp, so the
    // worst case here is "new value with an old timestamp", i.e. fresh data
    // judged slightly stale -- conservative and safe. The opposite order would
    // pair an old value with a new timestamp and hand out expired data as fresh.
    //
    i32Value  = g_i32TempC100Cache;
    ui32Stamp = g_ui32TempStampTick;

    if ( (ui32MaxAgeTicks != portMAX_DELAY) &&
         ((xTaskGetTickCount() - ui32Stamp) > ui32MaxAgeTicks) )
    {
        //
        // Consecutive read failures beyond the acceptable age: report invalid
        // rather than keep serving the old value.
        //
        *pi32TempC100 = TMP119_TEMP_INVALID;
        return 1;
    }

    *pi32TempC100 = i32Value;

    return 0;
}

//*****************************************************************************
//
// Sensor task
//
//*****************************************************************************
void
SensorTask(void *pvParameters)
{
    //
    // Initialize the TMP119 temperature sensor (IOM1 / GP8, GP9).
    // On failure the temperature page shows "no sensor"; nothing else is affected.
    //
    if ( TMP119_init() != 0 )
    {
        //
        // The sensor is not present. Keep the task alive and keep retrying, so
        // hot-plugging the mikroBUS module recovers without a reboot.
        //
        am_util_debug_printf("[sensor] TMP119 init failed, will retry\n");
    }
    else
    {
        am_util_debug_printf("[sensor] task started, %u ms period\n",
                             (unsigned)(TEMP_SAMPLE_PERIOD_TICKS * 1000 /
                                        configTICK_RATE_HZ));
    }

    while (1)
    {
        if ( !TMP119_is_ready() )
        {
            //
            // No sensor found yet, retry init. TMP119_init() returns immediately
            // once it has succeeded and never reconfigures anything twice.
            //
            TMP119_init();

#if WATCHFACE_SENSOR_LOG
            am_util_debug_printf("[sensor] no sensor\n");
#endif
        }
        else
        {
            int32_t i32Temp = TMP119_TEMP_INVALID;

            if ( (TMP119_read_temp_c100(&i32Temp) == 0) &&
                 (i32Temp != TMP119_TEMP_INVALID) )
            {
                //
                // Value first, then timestamp; see temp_cache_get() for why.
                //
                g_i32TempC100Cache  = i32Temp;
                g_ui32TempStampTick = xTaskGetTickCount();
                g_bTempCacheValid   = true;

#if WATCHFACE_SENSOR_LOG
                //
                // One line to RTT channel 0 per sampling period.
                //
                // am_util_stdio_printf() has no %f, so the x100 integer is split
                // into integer and fractional parts by hand. The sign is handled
                // separately: -50 (-0.50 C) has a quotient of 0, and "%d" alone
                // would swallow the sign and print "0.50".
                //
                int32_t i32Abs = (i32Temp < 0) ? -i32Temp : i32Temp;

                am_util_debug_printf("[sensor] %s%d.%02d C\n",
                                     (i32Temp < 0) ? "-" : "",
                                     i32Abs / 100,
                                     i32Abs % 100);
#endif
            }
            else
            {
                //
                // On a failed read the cache is left alone and the timestamp ages
                // naturally. Only after consecutive failures exceed
                // TEMP_CACHE_MAX_AGE_TICKS does temp_cache_get() report invalid.
                //
#if WATCHFACE_SENSOR_LOG
                am_util_debug_printf("[sensor] read error\n");
#endif
            }
        }

        //
        // Drain the BLE diagnostic events ([bledbg]).
        //
        // The records accumulate as plain RAM writes on RadioTask's HCI path, and
        // emission is concentrated here: am_util_stdio_printf() shares a single
        // unlocked static buffer, so formatting inside the highest-priority
        // RadioTask would both overwrite other tasks' output and stretch the very
        // timing being measured. See ble_diag.h.
        //
        ble_diag_drain();

#if WATCHFACE_BLE_IRQ_DIAG
        //
        // EM9305 interrupt path state. See the WATCHFACE_BLE_IRQ_DIAG comment at
        // the top of this file for how to read it.
        //
        am_util_stdio_printf("[irqdiag] em9305 irq=%u pin=%u nvic=%u\r\n",
                             (unsigned)g_ui32InterruptsSeen,
                             (unsigned)am_hal_gpio_input_read(AM_BSP_GPIO_EM9305_INT),
                             (unsigned)((NVIC->ISER[(uint32_t)GPIO0_607F_IRQn >> 5U] &
                                         (1UL << ((uint32_t)GPIO0_607F_IRQn & 0x1FUL))) ? 1U : 0U));
#endif // WATCHFACE_BLE_IRQ_DIAG

#if WATCHFACE_RTOS_DIAG
        //
        // Report RTOS resource headroom once a second.
        //
        // heap free trending down -> pvPortMalloc will eventually fail (configTOTAL_HEAP_SIZE too small).
        // any stack nearing 0      -> that task's stack is too small and will trip the overflow hook.
        // Units: heap in bytes, stack in words (1 word = 4 bytes).
        //
        {
            TaskHandle_t pxTasks[] =
            {
                RenderTaskHandle, DisplayTaskHandle,
                SensorTaskHandle, radio_task_handle
            };
            static const char * const pcNames[] =
            {
                "Render", "Display", "Sensor", "Radio"
            };

            am_util_stdio_printf("[diag] heap free=%u min=%u |",
                                 (unsigned)xPortGetFreeHeapSize(),
                                 (unsigned)xPortGetMinimumEverFreeHeapSize());

            for ( uint32_t i = 0; i < (sizeof(pxTasks) / sizeof(pxTasks[0])); i++ )
            {
                if ( pxTasks[i] != NULL )
                {
                    am_util_stdio_printf(" %s=%u",
                                         pcNames[i],
                                         (unsigned)uxTaskGetStackHighWaterMark(pxTasks[i]));
                }
            }

            am_util_stdio_printf("\r\n");
        }
#endif // WATCHFACE_RTOS_DIAG

        // Suspend this task for 1 second (1000 ticks in this project); the
        // scheduler resumes it once the time is up.
        vTaskDelay(TEMP_SAMPLE_PERIOD_TICKS);
    }
}
