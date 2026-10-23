//*****************************************************************************
//
//! @file ble_diag.h
//!
//! @brief BLE startup latency diagnostics: HCI event log and stuck-INT detection.
//!
//! Corresponds to docs/BLE_STARTUP_DELAY_INVESTIGATION.md. The first round of
//! measurements settled it: the boot delay comes from hciCmdTimeout()
//! (HCI_CMD_TIMEOUT = 10 s), not from the heartbeat in hci_drv_em9305.c; and the
//! timeout happens because the EM9305 INT is already high while the host never
//! received the interrupt (g_ui32InterruptsSeen frozen).
//!
//! GPIO117 is configured as AM_HAL_GPIO_PIN_INTDIR_LO2HI (rising edge only), so
//! once that transition is missed there is **no second chance** -- the only way
//! out is the 10 s timeout restarting the controller. That explains the
//! quantized ladder of "sometimes it advertises at boot, sometimes after 10 or
//! 20 seconds": 10 s per missed edge.
//!
//! This file provides three things:
//!
//! 1. The `[bledbg]` event ring buffer (section 6.2): all hook points are
//!    implemented with GNU ld's --wrap (see LFLAGS in gcc/Makefile), so not one
//!    word of third_party/cordio changes. --wrap only rewrites calls that cross
//!    translation units, and all five hooked symbols qualify:
//!
//!      hciTrSendCmd          defined in hci_tr.c,   called from hci_cmd.c
//!      hciTrSerialRxIncoming defined in hci_tr.c,   called from hci_drv_em9305
//!      hciCmdRecvCmpl        defined in hci_cmd.c,  called from hci_evt.c
//!      hciCmdTimeout         defined in hci_cmd.c,  called from hci_core_ps.c
//!      HciDrvRadioBoot       defined in hci_drv_em9305.c, called from radio_task.c / hci_cmd.c
//!
//! 2. An output path no other task can trample: the hooks write RAM only, and
//!    SensorTask calls ble_diag_drain() once a second to emit it; emission
//!    formats into its own local buffer before handing it to
//!    SEGGER_RTT_WriteString(). See ble_diag_emit() in ble_diag.c for why.
//!
//! 3. Stuck-INT detection and re-kick (ble_diag_int_kick()): called by RenderTask
//!    every frame; on detecting "INT held high while the interrupt count does not
//!    move" it posts one extra WsfSetEvent(), turning the 10 s of idle waiting
//!    into a few tens of milliseconds. It is also the last piece of evidence for
//!    this causal chain.
//
//*****************************************************************************

#ifndef BLE_DIAG_H
#define BLE_DIAG_H

#include <stdint.h>

#include "wsf_types.h"
#include "wsf_os.h"

//*****************************************************************************
//
// WATCHFACE_BLE_DIAG
//
// 1 = enable HCI event logging and [bledbg] output
// 0 = compiled out entirely (the --wrap options in gcc/Makefile should then be
//     commented out too, or the linker reports undefined references to __wrap_*)
//
//*****************************************************************************
#define WATCHFACE_BLE_DIAG      1

//*****************************************************************************
//
// WATCHFACE_BLE_INT_KICK
//
// Workaround for a missed EM9305 INT (requires WATCHFACE_BLE_DIAG = 1).
//
// 1 = RenderTask checks once per frame; if INT stays high continuously while
//     g_ui32InterruptsSeen does not move within BLE_DIAG_KICK_STUCK_MS, one
//     extra BLE_TRANSFER_NEEDED_EVENT is posted so HciDrvHandler() drains the
//     backlogged packets.
// 0 = observe only, do not intervene (for reproducing the original 10 s delay).
//
// Why post an event rather than change the interrupt configuration: Apollo5 GPIO
// interrupts are edge-triggered only (HI2LO / LO2HI / BOTH), with no level
// trigger. With INT already high there is no further rising edge, so there is no
// hardware way to re-trigger it; software has to kick it periodically. The
// author of hci_drv_em9305.c hit the same thing -- see the comment at line 931.
// "It looks like there's time that we won't get interrupt even though
//  there's packet waiting for host to grab."
//
//*****************************************************************************
#define WATCHFACE_BLE_INT_KICK  1

//*****************************************************************************
//
// Event kinds.
//
//*****************************************************************************
typedef enum
{
    BLE_DIAG_TAG_TX_CMD = 0,    // HCI command written to transport (a = opcode)
    BLE_DIAG_TAG_RX,            // Bytes handed to the host stack (a = len, b = first 4 bytes)
    BLE_DIAG_TAG_CMD_CMPL,      // Command Complete / Status (a = opcode, b = numCmdPkts)
    BLE_DIAG_TAG_CMD_TIMEOUT,   // hciCmdTimeout() (a = the stuck opcode)
    BLE_DIAG_TAG_RADIO_BOOT,    // HciDrvRadioBoot() (a = bColdBoot)
    BLE_DIAG_TAG_DRV_EVT,       // HciDrvHandler() entry (a = event/msg, b = irq/pin)
    BLE_DIAG_TAG_HEARTBEAT,     // Heartbeat event triggering a benign command
    BLE_DIAG_TAG_INT_KICK       // Stuck INT detected and event re-posted (a = milliseconds stuck)
} ble_diag_tag_e;

#if WATCHFACE_BLE_DIAG

//*****************************************************************************
//
//! @brief Push one event into the ring buffer. RAM only; callable from task or interrupt context.
//!
//! @param ui8Tag Event kind (ble_diag_tag_e)
//! @param ui16A  Small field, meaning depends on the event kind
//! @param ui32B  Large field, meaning depends on the event kind
//
//*****************************************************************************
extern void ble_diag_log(uint8_t ui8Tag, uint16_t ui16A, uint32_t ui32B);

//*****************************************************************************
//
//! @brief Emit the accumulated events to RTT and report how many were emitted.
//!
//! Called from a low-priority task (SensorTask). The number emitted per call is
//! capped to avoid holding RTT and the CPU for long; the rest waits for the next call.
//
//*****************************************************************************
extern uint32_t ble_diag_drain(void);

//*****************************************************************************
//
//! @brief Wrapper around HciDrvHandler(): record first, then forward to the original.
//!
//! Registered with WsfOsSetNextHandler() by exactle_stack_init() in
//! radio_task.c, in place of HciDrvHandler itself. Each entry also samples
//! g_ui32InterruptsSeen and the GPIO117 level, so whether the interrupt path is
//! alive can be seen without touching hci_drv_em9305.c.
//
//*****************************************************************************
extern void ble_diag_hci_drv_handler(wsfEventMask_t event, wsfMsgHdr_t *pMsg);

//*****************************************************************************
//
//! @brief Record the HCI driver's WSF handler ID for ble_diag_int_kick() to post to.
//!
//! Called by radio_task.c after WsfOsSetNextHandler(). g_HciDrvHandleID in
//! hci_drv_em9305.c is deliberately not read directly, to avoid depending on the
//! implicit convention that "0 means not yet initialized".
//
//*****************************************************************************
extern void ble_diag_set_hci_handler_id(wsfHandlerId_t handlerId);

//*****************************************************************************
//
//! @brief Check whether the EM9305 INT is stuck high and post a transfer event if needed.
//!
//! Called by RenderTask every frame (roughly every 18 ms). The conditions and
//! the reasoning are described under WATCHFACE_BLE_INT_KICK.
//
//*****************************************************************************
extern void ble_diag_int_kick(void);

#else

#define ble_diag_log(tag, a, b)             do { } while (0)
#define ble_diag_drain()                    do { } while (0)
#define ble_diag_set_hci_handler_id(id)     do { } while (0)
#define ble_diag_int_kick()                 do { } while (0)

#endif // WATCHFACE_BLE_DIAG

#endif // BLE_DIAG_H
