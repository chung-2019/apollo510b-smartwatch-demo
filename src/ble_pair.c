//*****************************************************************************
//
//! @file ble_pair.c
//!
//! @brief Shared state for Numeric Comparison pairing and the link parameters.
//!
//! The rationale is documented in ble_pair.h.
//
//*****************************************************************************

#include <string.h>

//*****************************************************************************
//
//! @file ble_pair.c
//!
//! @brief Shared state for Numeric Comparison pairing.
//!
//! The rationale is documented in ble_pair.h.
//
//*****************************************************************************

#include "am_mcu_apollo.h"

#include "ble_pair.h"

//
// The state and the numeric value must be read and written as a pair; see the
// explanation in ble_pair.h.
//
static uint32_t         g_ui32PairPasskey;
static ble_pair_state_e g_ePairState = BLE_PAIR_IDLE;

//
// The connection flag is a standalone boolean: reads and writes are atomic on
// their own, so no critical section is needed.
//
static volatile bool    g_bConnOpen;

//
// This link's parameters. The fields must be consistent with each other (see
// ble_link_info_get()), so like the pairing state every read and write of the
// whole set is wrapped in AM_CRITICAL.
//
// The initial value means "nothing known yet": interval 0, PHY NONE, RSSI not
// read. On disconnect ble_pair_conn_set(false) clears it back to this state.
//
static ble_link_info_t  g_sLinkInfo;

void
ble_pair_compare_set(uint32_t ui32Passkey)
{
    AM_CRITICAL_BEGIN;

    g_ui32PairPasskey = ui32Passkey;
    g_ePairState      = BLE_PAIR_COMPARE;

    AM_CRITICAL_END;
}

void
ble_pair_result_set(bool bPass)
{
    AM_CRITICAL_BEGIN;

    //
    // Record a result only when this round actually ran a numeric comparison.
    // A bonded phone that reconnects encrypts straight away without comparing
    // digits, and that does not count as a new pairing.
    //
    if ( g_ePairState == BLE_PAIR_COMPARE )
    {
        g_ePairState = bPass ? BLE_PAIR_PASS : BLE_PAIR_FAIL;
    }

    AM_CRITICAL_END;
}

void
ble_pair_reset(void)
{
    AM_CRITICAL_BEGIN;

    g_ui32PairPasskey = 0;
    g_ePairState      = BLE_PAIR_IDLE;

    AM_CRITICAL_END;
}

void
ble_pair_conn_set(bool bConnected)
{
    g_bConnOpen = bConnected;

    //
    // On disconnect, clear the link parameters entirely. Otherwise the profile
    // panel would keep showing the previous link's interval, PHY and RSSI until
    // the next connection, and look as if it were still connected.
    //
    if ( !bConnected )
    {
        AM_CRITICAL_BEGIN;

        memset(&g_sLinkInfo, 0, sizeof(g_sLinkInfo));

        AM_CRITICAL_END;
    }
}

void
ble_link_conn_begin(uint16_t ui16Interval)
{
    AM_CRITICAL_BEGIN;

    memset(&g_sLinkInfo, 0, sizeof(g_sLinkInfo));

    g_sLinkInfo.bConnected       = true;
    g_sLinkInfo.ui16ConnInterval = ui16Interval;

    //
    // Every connection starts on LE 1M (Core Spec). If 2M is negotiated later, a
    // DM_PHY_UPDATE_IND overwrites this; no such event means the link stayed on
    // 1M, not that the PHY is unknown.
    //
    g_sLinkInfo.ui8TxPhy = BLE_LINK_PHY_1M;
    g_sLinkInfo.ui8RxPhy = BLE_LINK_PHY_1M;

    AM_CRITICAL_END;
}

void
ble_link_interval_set(uint16_t ui16Interval)
{
    AM_CRITICAL_BEGIN;

    g_sLinkInfo.ui16ConnInterval = ui16Interval;

    AM_CRITICAL_END;
}

void
ble_link_phy_set(uint8_t ui8TxPhy, uint8_t ui8RxPhy)
{
    AM_CRITICAL_BEGIN;

    g_sLinkInfo.ui8TxPhy = ui8TxPhy;
    g_sLinkInfo.ui8RxPhy = ui8RxPhy;

    AM_CRITICAL_END;
}

void
ble_link_sec_set(bool bEncrypted, bool bBonded)
{
    AM_CRITICAL_BEGIN;

    g_sLinkInfo.bEncrypted = bEncrypted;
    g_sLinkInfo.bBonded    = bBonded;

    AM_CRITICAL_END;
}

void
ble_link_rssi_set(int8_t i8Rssi)
{
    AM_CRITICAL_BEGIN;

    g_sLinkInfo.i8Rssi     = i8Rssi;
    g_sLinkInfo.bRssiValid = true;

    AM_CRITICAL_END;
}

void
ble_link_info_get(ble_link_info_t *psInfo)
{
    if ( psInfo == NULL )
    {
        return;
    }

    AM_CRITICAL_BEGIN;

    *psInfo = g_sLinkInfo;

    //
    // bConnected follows the flag set by ble_pair_conn_set(). On the normal path
    // the two always agree (both are set inside DM_CONN_OPEN_IND); ANDing them
    // here only keeps a timing gap -- "the flag says disconnected but the
    // parameters are still there" -- from reaching the screen.
    //
    psInfo->bConnected = psInfo->bConnected && g_bConnOpen;

    AM_CRITICAL_END;
}

bool
ble_pair_conn_is_open(void)
{
    return g_bConnOpen;
}

ble_pair_state_e
ble_pair_state_get(uint32_t *pui32Passkey)
{
    ble_pair_state_e eState;

    AM_CRITICAL_BEGIN;

    eState = g_ePairState;

    if ( pui32Passkey != NULL )
    {
        *pui32Passkey = g_ui32PairPasskey;
    }

    AM_CRITICAL_END;

    return eState;
}
