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
