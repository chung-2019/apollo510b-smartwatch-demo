//*****************************************************************************
//
//! @file ble_pair.h
//!
//! @brief BLE state bridge: written by the BLE side, read by the display side.
//!
//! It currently carries two things: whether a connection is up (for drawing the
//! Bluetooth icon) and the progress and result of Numeric Comparison pairing.
//! Both are one-way data: RadioTask writes, RenderTask reads.
//!
//! Pairing was changed from Just Works to LE Secure Connections Numeric
//! Comparison (six-digit confirmation). Three things must hold at once (see
//! hts_main.c):
//!
//!   1. tmpsSecCfg.auth carries DM_AUTH_SC_FLAG and DM_AUTH_MITM_FLAG
//!   2. tmpsSmpCfg.ioCap = SMP_IO_DISP_YES_NO
//!   3. DM_SEC_COMPARE_IND must be handled locally (show the digits, reply with DmSecCompareRsp)
//!
//! Without the MITM flag, SMP negotiates Just Works and DM_SEC_COMPARE_IND never
//! occurs at all; leaving ioCap at SMP_IO_NO_IN_NO_OUT has the same effect.
//!
//! This file only passes state between tasks. The writer is RadioTask (WSF / DM
//! event handling) and the reader is RenderTask (every frame that draws the
//! temperature page). They run at different priorities with no other
//! synchronization, so the six digits and the state code must be read and written
//! together -- otherwise it is possible to draw "state is COMPARE but the digits
//! are still from the previous round". Wrapping them in AM_CRITICAL is the
//! simplest sufficient approach: the critical section holds a few assignments and
//! does not disturb BLE timing.
//
//*****************************************************************************

#ifndef BLE_PAIR_H
#define BLE_PAIR_H

#include <stdbool.h>
#include <stdint.h>

//*****************************************************************************
//
//! @brief Pairing display state.
//
//*****************************************************************************
typedef enum
{
    BLE_PAIR_IDLE = 0,      // No pairing in progress and no result to show
    BLE_PAIR_COMPARE,       // Comparing: the six digits should be displayed
    BLE_PAIR_PASS,          // The peer pressed Yes, pairing succeeded
    BLE_PAIR_FAIL           // The peer pressed No / cancelled, or pairing failed otherwise
} ble_pair_state_e;

//*****************************************************************************
//
//! @brief Record the six comparison digits to display and move to BLE_PAIR_COMPARE.
//!
//! @param ui32Passkey The comparison value from DmSecGetCompareValue() (0 to 999999)
//
//*****************************************************************************
extern void ble_pair_compare_set(uint32_t ui32Passkey);

//*****************************************************************************
//
//! @brief Record the pairing result and move to BLE_PAIR_PASS or BLE_PAIR_FAIL.
//!
//! @param bPass true when pairing succeeded
//!
//! @note A result is kept only for a round that went through
//! ble_pair_compare_set(). A bonded phone reconnecting does no digit comparison,
//! and recording a result then would show it as a fresh pairing, so it is ignored.
//
//*****************************************************************************
extern void ble_pair_result_set(bool bPass);

//*****************************************************************************
//
//! @brief Clear the state and return to BLE_PAIR_IDLE.
//!
//! Called on DM_CONN_CLOSE_IND and DM_CONN_OPEN_IND: Pass / Fail only mean
//! something during the connection in which pairing happened. After a disconnect
//! the display must go back to the usual sensor source text, so nobody believes
//! the pairing is still in effect.
//
//*****************************************************************************
extern void ble_pair_reset(void);

//*****************************************************************************
//
//! @brief Record whether a BLE connection is established.
//!
//! @param bConnected true when a connection is up
//!
//! Called on DM_CONN_OPEN_IND / DM_CONN_CLOSE_IND. The temperature page uses it
//! to decide whether to draw the Bluetooth icon, with the same meaning as the
//! icon in a phone's status bar: it appears only when connected.
//
//*****************************************************************************
extern void ble_pair_conn_set(bool bConnected);

//*****************************************************************************
//
//! @brief Query whether a BLE connection is established.
//!
//! @return true when a connection is up
//
//*****************************************************************************
extern bool ble_pair_conn_is_open(void);

//*****************************************************************************
//
//! @brief Read out the current state and the six digits.
//!
//! @param pui32Passkey When non-NULL, receives the six comparison digits; the
//!                     value is meaningless unless the state is BLE_PAIR_COMPARE
//!
//! @return The current ble_pair_state_e
//
//*****************************************************************************
extern ble_pair_state_e ble_pair_state_get(uint32_t *pui32Passkey);

#endif // BLE_PAIR_H
