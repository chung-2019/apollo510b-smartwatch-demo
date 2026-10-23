//*****************************************************************************
//
//! @file radio_task.c
//!
//! @brief Task to handle radio operation.
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

//*****************************************************************************
//
// Global includes for this project.
//
//*****************************************************************************
#include "ble_freertos_tmp119_hts.h"

//*****************************************************************************
//
// WSF standard includes.
//
//*****************************************************************************
#include "wsf_types.h"
#include "wsf_trace.h"
#include "wsf_buf.h"
#include "wsf_timer.h"

//*****************************************************************************
//
// Includes for operating the ExactLE stack.
//
//*****************************************************************************
#include "hci_handler.h"
#include "dm_handler.h"
#include "l2c_handler.h"
#include "att_handler.h"
#include "smp_handler.h"
#include "l2c_api.h"
#include "att_api.h"
#include "smp_api.h"
#include "app_api.h"
#include "hci_core.h"
#include "hci_drv.h"
#include "hci_drv_apollo.h"

#include "wsf_msg.h"

#if defined(AM_PART_APOLLO510)
#include "hci_drv_em9305.h"
#include "am_devices_em9305.h"
#endif

#if defined(AM_PART_APOLLO330P_510L)
#include "hci_drv_510L_radio.h"
#include "am_devices_510L_radio.h"
#endif
//*****************************************************************************
//
// Includes for the ExactLE "fit" profile.
//
//*****************************************************************************
#include "hts_api.h"
#include "app_ui.h"

//*****************************************************************************
//
// BLE startup latency diagnostics (HCI event log).
//
//*****************************************************************************
#include "ble_diag.h"

//*****************************************************************************
//
// Radio task handle.
//
//*****************************************************************************
TaskHandle_t radio_task_handle;

//*****************************************************************************
//
// Function prototypes
//
//*****************************************************************************
void exactle_stack_init(void);

//*****************************************************************************
//
// WSF buffer pools.
//
//*****************************************************************************
#define WSF_BUF_POOLS               4

// Important note: the size of g_pui32BufMem should includes both overhead of internal
// buffer management structure, wsfBufPool_t (up to 16 bytes for each pool), and pool
// description (e.g. g_psPoolDescriptors below).
// The length of buffers in pool MUST be a multiple of sizeof(wsfBufMem_t), which is 8
// by default. Or it may cause WSF buffer pool initialization failure.

// Memory for the buffer pool
// extra AMOTA_PACKET_SIZE bytes for OTA handling
static uint32_t g_pui32BufMem[
        (WSF_BUF_POOLS*16
         + 16*8 + 32*4 + 64*6 + 280*14) / sizeof(uint32_t)];
// Default pool descriptor.
static wsfBufPoolDesc_t g_psPoolDescriptors[WSF_BUF_POOLS] =
{
    {  16,  8 },
    {  32,  4 },
    {  64,  6 },
    { 280,  14 }
};

//*****************************************************************************
//
// Tracking variable for the scheduler timer.
//
//*****************************************************************************

void radio_timer_handler(void);

//*****************************************************************************
//
// Initialization for the ExactLE stack.
//
//*****************************************************************************
void
exactle_stack_init(void)
{
    wsfHandlerId_t handlerId;
    uint16_t       wsfBufMemLen;
    //
    // Set up timers for the WSF scheduler.
    //
    WsfOsInit();
    WsfTimerInit();

    //
    // Initialize a buffer pool for WSF dynamic memory needs.
    //
    wsfBufMemLen = WsfBufInit(sizeof(g_pui32BufMem), (uint8_t *)g_pui32BufMem, WSF_BUF_POOLS,
               g_psPoolDescriptors);

    if (wsfBufMemLen > sizeof(g_pui32BufMem))
    {
        am_util_debug_printf("Memory pool is too small by %d\r\n",
                             wsfBufMemLen - sizeof(g_pui32BufMem));
    }

    //
    // Initialize the WSF security service.
    //
    SecInit();
    SecAesInit();
    SecCmacInit();
    SecEccInit();

    //
    // Set up callback functions for the various layers of the ExactLE stack.
    //
    handlerId = WsfOsSetNextHandler(HciHandler);
    HciHandlerInit(handlerId);

    //
    // DM is the Device Manager: it owns BLE advertising, connections, PHY,
    // security and privacy. DmHandler(), defined in dm_main.c of the Cordio BLE
    // Host, is the WSF event handler of the DM subsystem itself and dispatches
    // incoming messages to the matching DM component.
    //
    handlerId = WsfOsSetNextHandler(DmHandler);
    DmDevVsInit(0);
    DmAdvInit();
    DmPhyInit();
    DmConnInit();
    DmConnSlaveInit();
    DmSecInit();
    DmSecLescInit();
    DmPrivInit();
    DmHandlerInit(handlerId);

    handlerId = WsfOsSetNextHandler(L2cSlaveHandler);
    L2cSlaveHandlerInit(handlerId);
    L2cInit();
    L2cSlaveInit();

    handlerId = WsfOsSetNextHandler(AttHandler);
    AttHandlerInit(handlerId);
    AttsInit();
    AttsIndInit();
    AttcInit();

    handlerId = WsfOsSetNextHandler(SmpHandler);
    SmpHandlerInit(handlerId);
    SmprInit();
    SmprScInit();
    HciSetMaxRxAclLen(251);

    //
    // AppHandler(), defined in app_main.c of the Cordio BLE Profiles, is the WSF
    // event handler of the shared application framework; it dispatches messages
    // to the master role, the slave role or internal framework flows by event type.
    //
    handlerId = WsfOsSetNextHandler(AppHandler);
    AppHandlerInit(handlerId);

    //
    // WsfOsSetNextHandler(), defined in wsf_os.c of Cordio WSF, appends a
    // handler to the WSF handler array and returns the allocated handler ID.
    // TmpsHandler(), defined in hts_main.c, handles the ATT messages, DM callback
    // events and temperature service messages this application receives; it does
    // not replace DmHandler() but processes the events DM reports up to the
    // application layer.
    //
    handlerId = WsfOsSetNextHandler(TmpsHandler);
    TmpsHandlerInit(handlerId); // Hand the allocated handler ID to the temperature service app.

#if WATCHFACE_BLE_DIAG
    //
    // Register the diagnostic wrapper rather than HciDrvHandler itself: every
    // time an HCI driver event fires it first records the timestamp, the event
    // source and the current interrupt-path state (g_ui32InterruptsSeen and the
    // GPIO117 level), then forwards to the original handler. See ble_diag.h for
    // how to read the output.
    //
    handlerId = WsfOsSetNextHandler(ble_diag_hci_drv_handler);
#else
    handlerId = WsfOsSetNextHandler(HciDrvHandler);
#endif
    HciDrvHandlerInit(handlerId);

    //
    // Tell ble_diag_int_kick() which handler should receive the replayed
    // transfer event.
    //
    ble_diag_set_hci_handler_id(handlerId);
}

#if defined(AM_PART_APOLLO510)
//*****************************************************************************
//
// GPIO interrupt handler.
//
// This project deliberately does NOT define GPIO_INT_ISR here -- it would
// expand to the same symbol as disp_tp_isr() in the display driver and produce
// a multiple definition link error:
//
//   GPIO_INT_ISR  = AM_BSP_EM9305_RADIO_INT_ISR = am_gpio0_607f_isr
//                   (EM9305 INT = GPIO117 -> 117/32 = 3 -> GPIO0_607F_IRQn)
//   disp_tp_isr   = am_gpio0_607f_isr
//                   (Touch  INT = GPIO98  ->  98/32 = 3 -> GPIO0_607F_IRQn)
//
// Both share the same IRQ line, and the two implementations do exactly the same
// thing: irq_status_get -> irq_clear -> interrupt_service. The original BLE
// example read am_hal_gpio_interrupt_status_get() one extra time, but never
// used the result.
//
// am_hal_gpio_interrupt_service() dispatches by the handler registered for each
// pin, so the single implementation in am_devices_display_generic.c serves both
// the EM9305 and the touch panel. The EM9305 handler is registered inside
// HciDrvRadioBoot() and is unaffected.
//
//*****************************************************************************

//*****************************************************************************
//
// UART interrupt handler.
//
//*****************************************************************************
#if AM_BSP_UART_PRINT_INST == 0
void am_uart_isr(void)
#elif AM_BSP_UART_PRINT_INST == 1
void am_uart1_isr(void)
#elif AM_BSP_UART_PRINT_INST == 2
void am_uart2_isr(void)
#elif AM_BSP_UART_PRINT_INST == 3
void am_uart3_isr(void)
#endif
{
    uint32_t ui32Status;

    //
    // Read and save the interrupt status, but clear out the status register.
    //
    ui32Status = UARTn(AM_BSP_UART_PRINT_INST)->MIS;
    UARTn(AM_BSP_UART_PRINT_INST)->IEC = ui32Status;
}
#endif
//*****************************************************************************
//
// Perform initial setup for the radio task.
//
//*****************************************************************************
void
RadioTaskSetup(void)
{
    am_util_debug_printf("RadioTask: setup\r\n");
}

//*****************************************************************************
//
// Short Description.
//
//*****************************************************************************
void
RadioTask(void *pvParameters)
{
#if WSF_TRACE_ENABLED == TRUE
    //
    // Enable ITM
    //
    am_util_debug_printf("Starting wicentric trace:\n\n");
#endif

    //
    // Startup timestamps: used to locate which phase consumes the time between
    // boot and the start of advertising. A tick is 1 ms (configTICK_RATE_HZ = 1000).
    //
    am_util_stdio_printf("[radio] t=%u ms  task start\r\n",
                         (unsigned)xTaskGetTickCount());

    //
    // Boot the radio.
    //
    HciDrvRadioBoot(1);

    am_util_stdio_printf("[radio] t=%u ms  HciDrvRadioBoot done\r\n",
                         (unsigned)xTaskGetTickCount());

    //
    // Initialize the main ExactLE stack.
    //
    exactle_stack_init();

    am_util_stdio_printf("[radio] t=%u ms  exactle_stack_init done\r\n",
                         (unsigned)xTaskGetTickCount());

    // uncomment the following to set custom Bluetooth address here
    // {
    //     uint8_t bd_addr[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    //     HciVscSetCustom_BDAddr(&bd_addr[0]);
    // }

    //
    // Start the "Fit" profile.
    //
    TmpsStart();

    am_util_stdio_printf("[radio] t=%u ms  TmpsStart done, heap free=%u\r\n",
                         (unsigned)xTaskGetTickCount(),
                         (unsigned)xPortGetFreeHeapSize());

    while (1)
    {
        //
        // Calculate the elapsed time from our free-running timer, and update
        // the software timers in the WSF scheduler.
        //
        wsfOsDispatcher();

    }
}
