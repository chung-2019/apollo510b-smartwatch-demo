//*****************************************************************************
//
//! @file ble_pair.h
//!
//! @brief BLE state bridge: written by the BLE side, read by the display side.
//!
//! It currently carries three things: whether a connection is up (for drawing the
//! Bluetooth icon), the progress and result of Numeric Comparison pairing, and
//! this link's parameters (connection interval, PHY, encryption / bonding, RSSI).
//! All three are one-way data: RadioTask writes, RenderTask reads.
//!
//! The link parameters feed the temperature page's Bluetooth profile panel
//! (pulled down from the top; see the BTP_* section of render_task.c). The panel
//! only displays and offers no controls, so this file has only setters and one
//! getter that reads everything at once -- nothing that asks the peer to change
//! a parameter.
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
//! @brief The device name, shared by advertising and the GAP Device Name.
//!
//! It lives here rather than in hts_main.c because the temperature page's
//! Bluetooth profile panel shows it too. Both sides use this one macro, so a
//! rename is still a one-line change.
//
//*****************************************************************************
#define BLE_DEVICE_NAME         "EdgePilot-510B"

//*****************************************************************************
//
//! @brief PHY codes.
//!
//! The values match the Core Spec's LE PHY Update Complete event (1 = LE 1M,
//! 2 = LE 2M, 3 = LE Coded), not the HCI_PHY_LE_*_BIT masks in hci_defs.h --
//! those are for the "preferred PHYs" fields, where Coded is 4 rather than 3, and
//! mixing the two shows the wrong PHY.
//
//*****************************************************************************
#define BLE_LINK_PHY_NONE       (0)
#define BLE_LINK_PHY_1M         (1)
#define BLE_LINK_PHY_2M         (2)
#define BLE_LINK_PHY_CODED      (3)

//*****************************************************************************
//
//! @brief A snapshot of this link's parameters.
//!
//! An unknown field is left at its "invalid" value rather than guessed: the panel
//! would rather show "--" than a number that looks real. Each field's invalid
//! value is noted below.
//
//*****************************************************************************
typedef struct
{
    bool     bConnected;        // whether a link is up; when false the rest is meaningless
    bool     bEncrypted;        // the link is encrypted (DM_SEC_ENCRYPT_IND)
    bool     bBonded;           // encrypted with an existing LTK, i.e. this phone is bonded
    bool     bRssiValid;        // whether i8Rssi has been read yet
    int8_t   i8Rssi;            // the most recent RSSI reading (dBm)
    uint16_t ui16ConnInterval;  // connection interval in 1.25 ms units; 0 = unknown
    uint8_t  ui8TxPhy;          // BLE_LINK_PHY_*; BLE_LINK_PHY_NONE = unknown
    uint8_t  ui8RxPhy;          // likewise
} ble_link_info_t;

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

//*****************************************************************************
//
//! @brief Record the connection interval.
//!
//! @param ui16Interval Connection interval, in units of 1.25 ms
//!
//! Called on DM_CONN_OPEN_IND and DM_CONN_UPDATE_IND. Both are needed: the link
//! opens with the parameters the peer proposed, after which AppSlaveProcDmMsg()
//! requests an update according to tmpsUpdateCfg, and the value actually in
//! effect is only known at DM_CONN_UPDATE_IND.
//
//*****************************************************************************
extern void ble_link_interval_set(uint16_t ui16Interval);

//*****************************************************************************
//
//! @brief Record the negotiated PHY.
//!
//! @param ui8TxPhy Transmit PHY (BLE_LINK_PHY_*)
//! @param ui8RxPhy Receive PHY (BLE_LINK_PHY_*)
//!
//! Called on DM_PHY_UPDATE_IND. Without that event the link stays on the default
//! LE 1M, which is why ble_link_conn_begin() starts both at BLE_LINK_PHY_1M.
//
//*****************************************************************************
extern void ble_link_phy_set(uint8_t ui8TxPhy, uint8_t ui8RxPhy);

//*****************************************************************************
//
//! @brief Record the encryption and bonding state.
//!
//! @param bEncrypted true when the link is encrypted
//! @param bBonded    true when an existing LTK was used (a bonded phone reconnecting)
//
//*****************************************************************************
extern void ble_link_sec_set(bool bEncrypted, bool bBonded);

//*****************************************************************************
//
//! @brief Record the most recent RSSI reading.
//!
//! @param i8Rssi The value reported by HCI Read RSSI (dBm)
//
//*****************************************************************************
extern void ble_link_rssi_set(int8_t i8Rssi);

//*****************************************************************************
//
//! @brief A link has opened: reset the parameters to this link's starting values.
//!
//! @param ui16Interval The connection interval carried by DM_CONN_OPEN_IND
//!
//! The PHY is set to LE 1M (the Core Spec's initial PHY for a connection),
//! encryption and bonding are cleared, and the RSSI is marked as not yet read.
//! Nothing carries over from the previous link -- that would make the panel show
//! the old phone's numbers for the first second after a reconnect.
//
//*****************************************************************************
extern void ble_link_conn_begin(uint16_t ui16Interval);

//*****************************************************************************
//
//! @brief Get a snapshot of the current link parameters.
//!
//! @param psInfo Where to write it; must not be NULL
//!
//! Read as one block rather than one getter per field: the fields have to be
//! consistent with each other (when bConnected is false, nothing should still
//! hold the previous link's values), and reading them separately could let the
//! screen assemble a combination that never existed.
//
//*****************************************************************************
extern void ble_link_info_get(ble_link_info_t *psInfo);

#endif // BLE_PAIR_H
