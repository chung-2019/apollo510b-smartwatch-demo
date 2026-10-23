//*****************************************************************************
//
//! @file rtos.c
//!
//! @brief Essential functions to make the RTOS run correctly.
//!
//! These functions are required by the RTOS for ticking, sleeping, and basic
//! error checking.
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

#include "nemagfx_watchface.h"
#include "render_task.h"
#include "display_task.h"
#include "sensor_task.h"
#include "radio_task.h"
#include "led_task.h"
#include "touch_task.h"
#include "map_task.h"
#include "asset_upload.h"
//*****************************************************************************
//
// Task priority definition.
//
//*****************************************************************************
// The BLE RadioTask gets the highest priority: Cordio has timing requirements
// for responding to connection events, and wsfOsDispatcher() normally blocks on
// an event group and only runs when an event actually arrives, so giving it
// priority does not squeeze render. The relative order of Render/Display/Sensor
// is unchanged; all three simply shift down one step.
//
// The LOWEST tier was added later for LedTask: it only wakes on a button
// interrupt, performs one GPIO write, and at the lowest priority cannot possibly
// affect the timing of anything above it.
#define TASK_PRIORITY_HIGHEST           (configMAX_PRIORITIES -1)
#define TASK_PRIORITY_HIGH              (TASK_PRIORITY_HIGHEST - 1)
#define TASK_PRIORITY_MIDDLE            (TASK_PRIORITY_HIGH - 1)
#define TASK_PRIORITY_LOW               (TASK_PRIORITY_MIDDLE - 1)
#define TASK_PRIORITY_LOWEST            (TASK_PRIORITY_LOW - 1)

//*****************************************************************************
//
// Task handle for the initial setup task.
//
//*****************************************************************************
TaskHandle_t xSetupTask;

//*****************************************************************************
//
// Frame buffer lock.
//
//*****************************************************************************
SemaphoreHandle_t g_semDisplayStart = NULL;
SemaphoreHandle_t g_semDisplayEnd = NULL;

//*****************************************************************************
//
// Event semphore.
//
//*****************************************************************************
SemaphoreHandle_t g_semEventStart = NULL;
SemaphoreHandle_t g_semEventEnd = NULL;

//*****************************************************************************
//
// Sleep function called from FreeRTOS IDLE task.
// Do necessary application specific Power down operations here
// Return 0 if this function also incorporates the WFI, else return value same
// as idleTime
//
//*****************************************************************************
//*****************************************************************************
//
// WATCHFACE_ALLOW_DEEPSLEEP
//
// 1 = IDLE enters deepsleep (the target for normal operation)
// 0 = IDLE only enters normal sleep (for diagnosis)
//
// Why this switch exists:
//
// While the RTT viewer is attached, the debugger keeps HFRC enabled and the
// device cannot actually reach deepsleep; it stops at normal sleep. Only after
// the RTT viewer disconnects does it truly enter deepsleep. So "fine with RTT
// attached, hangs a few seconds after disconnecting" points almost certainly at
// some peripheral state on the deepsleep path, not at the BLE protocol or memory.
//
// Setting this to 0 isolates deepsleep with everything else unchanged:
//   - no longer hangs on normal sleep -> the problem is in deepsleep peripheral
//     recovery (GPU power domain, DC/DSI, IOM6/EM9305 are all candidates)
//   - still hangs                     -> unrelated to deepsleep, look elsewhere
//
//*****************************************************************************
#define WATCHFACE_ALLOW_DEEPSLEEP       1

//*****************************************************************************
//
// WATCHFACE_ENABLE_BLE
//
// 1 = create RadioTask and start Cordio BLE (normal operation)
// 0 = do not create RadioTask (diagnostic control group; the BLE code still compiles, it just does not start)
//
// Purpose: combined with WATCHFACE_ALLOW_DEEPSLEEP, it removes BLE as a variable while investigating.
//
// Measured results (RTT viewer detached throughout):
//
//   BLE  DEEP   Result    Note
//   ---  ----   -------   --------------------------------------------------
//    1    0     OK        BLE and display both work under normal sleep
//    1    1     Hang      before the FPU setup was added
//    1    1     OK        after the FPU setup was added (the current combination)
//
// Root cause: main() was missing am_hal_sysctrl_fpu_enable() and
// fpu_stacking_enable(true) (present in the BLE example, lost during the port).
// Lazy stacking decides how FPU registers are preserved across exceptions and
// interrupts, and deepsleep wake-up runs through the interrupt path, so the
// symptom only appears once deepsleep is genuinely entered (no debugger attached).
//
// Note: a BLE=0 / DEEP=1 control group was attempted during the investigation,
// but that run flashed a stale image that had not been rebuilt (BLE still
// connected, which proves it), so the result does not count and is not in the
// table above. After changing a switch, always re-make and confirm the [build]
// line in the boot log matches expectations.
//
//*****************************************************************************
#define WATCHFACE_ENABLE_BLE            1

uint32_t
am_freertos_sleep(uint32_t idleTime)
{
    //
    // am_bsp_debug_printf_deepsleep_prepare() is deliberately NOT called here.
    //
    // That BSP function is designed for ITM/SWO output: it flushes the TPIU, and
    // before sleeping it switches AM_BSP_GPIO_ITM_SWO to a plain GPIO output and
    // back to SWO on wake. This project logs over SEGGER RTT (AM_USE_RTT_LOG) and
    // never uses ITM, so the call does nothing useful while reconfiguring the
    // SWD/SWO pins on every IDLE entry and exit -- and with render at about
    // 54 fps, IDLE runs very often.
    //
#if WATCHFACE_ALLOW_DEEPSLEEP
    am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_DEEP);
#else
    am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_NORMAL);
#endif

    return 0;
}

//*****************************************************************************
//
// Recovery function called from FreeRTOS IDLE task, after waking up from Sleep
// Do necessary 'wakeup' operations here, e.g. to power up/enable peripherals etc.
//
//*****************************************************************************
void
am_freertos_wakeup(uint32_t idleTime)
{
    return;
}

//*****************************************************************************
//
// FreeRTOS debugging functions.
//
//*****************************************************************************
void
vApplicationMallocFailedHook(void)
{
    //
    // Called if a call to pvPortMalloc() fails because there is insufficient
    // free memory available in the FreeRTOS heap.  pvPortMalloc() is called
    // internally by FreeRTOS API functions that create tasks, queues, software
    // timers, and semaphores.  The size of the FreeRTOS heap is set by the
    // configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h.
    //
    // The failure must be stated out loud: this used to be a silent while(1), and
    // the only symptom was "the screen suddenly stops", with no way to tell heap
    // exhaustion from a stack overflow or a hard fault.
    //
    am_util_stdio_printf("\r\n*** FATAL: pvPortMalloc failed ***\r\n");
    am_util_stdio_printf("    heap free    = %u bytes\r\n",
                         (unsigned)xPortGetFreeHeapSize());
    am_util_stdio_printf("    heap min-ever= %u bytes\r\n",
                         (unsigned)xPortGetMinimumEverFreeHeapSize());
    am_util_stdio_printf("    configTOTAL_HEAP_SIZE = %u\r\n",
                         (unsigned)configTOTAL_HEAP_SIZE);

    while (1);
}

void
vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName)
{
    (void) pxTask;

    //
    // Run time stack overflow checking is performed if
    // configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
    // function is called if a stack overflow is detected.
    //
    // Print which task overflowed. Without this line it is pure guesswork, and
    // with no debugger attached the BKPT raises another hard fault that buries
    // the evidence.
    //
    am_util_stdio_printf("\r\n*** FATAL: stack overflow in task '%s' ***\r\n",
                         (pcTaskName != NULL) ? pcTaskName : "?");

    while (1)
    {
        __asm("BKPT #0\n") ; // Break into the debugger
    }
}

#ifndef BAREMETAL

//*****************************************************************************
//
// High priority task to run immediately after the scheduler starts.
//
// This task is used for any global initialization that must occur after the
// scheduler starts, but before any functional tasks are running. This can be
// useful for enabling events, semaphores, and other global, RTOS-specific
// features.
//
//*****************************************************************************
void
setup_task(void *pvParameters)
{
    vTaskSuspendAll();

    //
    // Create the semaphone for buffer sync task
    //
    g_semDisplayStart = xSemaphoreCreateBinary();
    g_semDisplayEnd = xSemaphoreCreateBinary();

    //
    // Create the render tasks with the high priority.
    //
    xTaskCreate(RenderTask, "RenderTask", 1024, 0, TASK_PRIORITY_HIGH, &RenderTaskHandle);

    //
    // Create the display tasks with the middle priority.
    //
    xTaskCreate(DisplayTask, "DisplayTask", 1024, 0, TASK_PRIORITY_MIDDLE, &DisplayTaskHandle);

    //
    // Create the touch task at the same (highest) priority as RadioTask.
    //
    // Why so high: it has to outrank RenderTask (14). RenderTask holds the CPU
    // for several ms at a time inside draw_*() and nema_cl_wait(), and with a
    // lower priority the TOUCH_HOLD_POLL_MS (15 ms) fallback period during a hold
    // would stretch to the frame period -- defeating the point, since decoupling
    // sampling from the frame rate is exactly what this revision is for.
    //
    // Why the same priority does not hurt BLE: TouchTask spends almost all its
    // time blocked on a task notification (portMAX_DELAY when idle, zero CPU),
    // and once woken it performs one ~400 us I2C read plus a few dozen integer
    // comparisons. configUSE_TIME_SLICING is 1, so equal priorities round-robin
    // per tick and the worst case is RadioTask waiting 1 ms.
    //
    // The stack is 512 words (2 KB), same as SensorTask: both are "blocking I2C
    // read plus a little arithmetic" with equally shallow call depth.
    //
    // GPIO setup, interrupt registration and queue creation are deliberately left
    // inside TouchTask(); see touch_task.h: an interrupt can arrive the moment it
    // is enabled, and the callback needs a task handle that is already valid.
    //
#if WATCHFACE_ENABLE_TOUCH
    xTaskCreate(TouchTask, "TouchTask", 512, 0, TASK_PRIORITY_HIGHEST,
                &TouchTaskHandle);
#else
    am_util_stdio_printf("[rtos] touch disabled (WATCHFACE_ENABLE_TOUCH=0)\r\n");
#endif

    //
    // Create the sensor task at the lowest priority.
    //
    // Reading the TMP119 is blocking I2C (PIO polling, about 120 us per read).
    // Being preempted in the lowest-priority task costs nothing and never lands
    // in render's critical path.
    //
    xTaskCreate(SensorTask, "SensorTask", 512, 0, TASK_PRIORITY_LOW, &SensorTaskHandle);

    //
    // Create the map task at the same priority as SensorTask.
    //
    // It wakes every 33 ms to update navigation state and occasionally performs a
    // 438 KB tile copy (MRAM -> PSRAM). That copy must not cut in front of
    // RenderTask -- a frame would be dropped; but it should not rank below button
    // feedback either, or the map takes too long to fill in when crossing a tile
    // boundary.
    //
    // The stack is 512 words: this task does not recurse and uses no large locals;
    // its deepest layer is memcpy plus am_util_debug_printf.
    //
    xTaskCreate(MapTask, "MapTask", 512, 0, TASK_PRIORITY_LOW, &MapTaskHandle);

#if WATCHFACE_ASSET_UPLOAD
    //
    // Create the asset upload task at the lowest priority.
    //
    // It normally wakes every 10 ms to check the RTT down buffer and only gets
    // busy when data arrives. The lowest priority is because during a transfer it
    // runs continuously with taskYIELD() -- that is for throughput (the RTT down
    // buffer is small, and sleeping means the host waits for us), at the cost of
    // never being allowed to block anyone.
    //
    // The stack is 768 words: the receive buffer grew from 64 to 256 bytes (to
    // keep up with the host), and with f_write() and printf on top, 512 was tight.
    //
    xTaskCreate(AssetUploadTask, "UploadTask", 768, 0, TASK_PRIORITY_LOWEST,
                &AssetUploadTaskHandle);
#endif

    //
    // Create the button LED task, one priority step below SensorTask.
    //
    // It normally blocks entirely on a task notification and only wakes when BTN0
    // (GP46) is pressed, to perform one GPIO write and one log line. The lowest
    // priority guarantees it never cuts into render's or BLE's critical path;
    // conversely, button feedback a few milliseconds late is invisible.
    //
    // GPIO setup and interrupt registration are deliberately left inside
    // LedTask(); see led_task.c: an interrupt can arrive the moment it is enabled,
    // and the callback needs a task handle that is already valid.
    //
    xTaskCreate(LedTask, "LedTask", 512, 0, TASK_PRIORITY_LOWEST, &LedTaskHandle);

#if WATCHFACE_ENABLE_BLE
    //
    // Create the BLE RadioTask at the highest priority.
    //
    // The stack keeps the BLE example's 2048 words (8 KB) -- Cordio's call depth
    // is far greater than this project's other tasks and must not be trimmed to
    // SensorTask's size.
    //
    RadioTaskSetup();
    xTaskCreate(RadioTask, "RadioTask", (2*1024), 0, TASK_PRIORITY_HIGHEST,
                &radio_task_handle);
#else
    am_util_stdio_printf("[rtos] BLE disabled (WATCHFACE_ENABLE_BLE=0)\r\n");
#endif

    //
    // Resume task switch
    //
    xTaskResumeAll();

    //
    // The setup operations are complete, so suspend the setup task now.
    //
    vTaskDelete(NULL);

    while (1);
}

//*****************************************************************************
//
// Initializes all tasks
//
//*****************************************************************************
void
run_tasks(void)
{
    //
    // Boot banner: print the build time and the key compile-time switches.
    //
    // While investigating the deepsleep problem, a switch was changed without
    // re-running make before flashing, which led the control experiment to the
    // wrong conclusion. With this line, one look at RTT confirms which
    // combination is actually running on the board, with no need to infer it
    // from behaviour.
    //
    am_util_stdio_printf("\r\n[build] %s %s | BLE=%d DEEPSLEEP=%d heap=%uKB\r\n",
                         __DATE__, __TIME__,
                         WATCHFACE_ENABLE_BLE,
                         WATCHFACE_ALLOW_DEEPSLEEP,
                         (unsigned)(configTOTAL_HEAP_SIZE / 1024));

    //
    // Set some interrupt priorities before we create tasks or start the scheduler.
    //
    // Note: Timer priority is handled by the FreeRTOS kernel, so we won't
    // touch it here.
    //

    //
    // Create essential tasks.
    //
    xTaskCreate(setup_task, "Setup", 512, 0, TASK_PRIORITY_HIGH, &xSetupTask);

    //
    // Start the scheduler.
    //
    vTaskStartScheduler();

    /* If all is well, the scheduler will now be running, and the following
     * line will never be reached.  If the following line does execute, then
     * there was insufficient FreeRTOS heap memory available for the idle and/or
     * timer tasks to be created.  See the memory management section on the
     * FreeRTOS web site for more details (this is standard text that is not
     * really applicable to the Win32 simulator port). */
    while (1);
}

#endif
