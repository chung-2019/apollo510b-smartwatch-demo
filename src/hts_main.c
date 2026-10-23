/*************************************************************************************************/
/*!
 *  \file
 *
 *  \brief  Health thermometer sample application using the following profiles:
 *            Health Thermometer Profile
 *
 *  Copyright (c) 2011-2019 Arm Ltd. All Rights Reserved.
 *
 *  Copyright (c) 2019 Packetcraft, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
/*************************************************************************************************/

#include <string.h>
#include "am_util.h"          /* am_util_stdio_printf(): startup timestamps */
#include "wsf_types.h"
#include "util/bstream.h"
#include "wsf_msg.h"
#include "wsf_trace.h"
#include "hci_api.h"
#include "dm_api.h"
#include "att_api.h"
#include "smp_api.h"
#include "app_api.h"
#include "app_db.h"
#include "app_ui.h"
#include "app_hw.h"
#include "app_main.h"
#include "svc_ch.h"
#include "svc_core.h"
#include "svc_hts.h"
#include "svc_dis.h"
#include "svc_batt.h"
#include "gatt/gatt_api.h"
#include "bas/bas_api.h"
#include "htps/htps_api.h"
#include "tmp119.h"
#include "sensor_task.h"
#include "hts_api.h"
#include "atts_main.h"
#include "ble_led.h"
#include "ble_pair.h"

#ifdef AM_BLE_EATT
#include "att_eatt.h"
#include "eatt_api.h"
#endif

/**************************************************************************************************
  Macros
**************************************************************************************************/

/*! Starting value for the application's own WSF message events.
 *  Cordio's ATT, DM and other system events use lower event values, so this
 *  application follows the examples and allocates from 0xA0 to avoid collisions
 *  and stay recognizable; the enum below then yields 0xA0 (temperature timer),
 *  0xA1 (battery timer) and 0xA2 (LED timer). */
#define TMPS_MSG_START               0xA0

/*! Blink half-period of the BLE status LED (LED3 / GP19) while advertising, in milliseconds */
#define TMPS_LED_BLINK_MS            500

/*! Temperature measurement indication period, in milliseconds.
 *
 *  Set to 1000 to match the display update rate: SensorTask samples once per
 *  second (aligned with the TMP119's 1 s conversion period), so the display and
 *  BLE take the same cache at the same rate.
 *
 *  Note that their phases are independent -- BLE is driven by the HTPS's own WSF
 *  timer and reads a cached value 0 to 1 s old, not one from the same instant as
 *  the display update.
 *
 *  The connection interval is 15 to 30 ms (see tmpsUpdateCfg), giving 33 to 66
 *  connection events per second, ample for one indication that needs a peer ACK.
 *
 *  Changing this value requires changing APP_HW_TM_PERIOD_SEC in app_hw_tmp119.c too.
 */
#define TMPS_TM_MEAS_PERIOD_MS       1000

/*! Temperature measurement flags: Celsius, with a timestamp and a temperature type.
 *  The nRF52832 version sets TEMP_TYPE_AS_CHARACTERISTIC to 0, so the
 *  temperature type travels inside the measurement value rather than as a
 *  separate characteristic. */
#define TMPS_TM_FLAGS                (CH_TM_FLAG_UNITS_C | CH_TM_FLAG_TIMESTAMP | \
                                      CH_TM_FLAG_TEMP_TYPE)

/*! WSF message event enumeration */
enum
{
  TMPS_TM_TIMER_IND = TMPS_MSG_START,      /*! Temperature measurement timer expired */
  TMPS_BATT_TIMER_IND,                     /*! Battery measurement timer expired */
  TMPS_LED_TIMER_IND                       /*! Status LED blink timer expired */
};

/**************************************************************************************************
  Data types
**************************************************************************************************/

/*! Application message type */
typedef union
{
  wsfMsgHdr_t     hdr;
  dmEvt_t         dm;
  attsCccEvt_t    ccc;
  attEvt_t        att;
} tmpsMsg_t;

/**************************************************************************************************
  Configurable parameters
**************************************************************************************************/

/*! Advertising configuration */
static const appAdvCfg_t tmpsAdvCfg =
{
  {0,     0,     0},                  /*! Advertising duration in milliseconds (0 means no timeout) */
  {  320,     0,     0}                   /*! Advertising interval in units of 0.625 ms (200 ms) */
};

/*! Peripheral role configuration */
static const appSlaveCfg_t tmpsSlaveCfg =
{
  TMPS_CONN_MAX,                           /*! Maximum number of connections */
};

//*****************************************************************************
//
// TMPS_PAIRING_MODE
//
//   2 = original BLE example behaviour: bonding + LE Secure Connections (stores the LTK) <- current setting
//   1 = no bonding, but LE Secure Connections still supported
//   0 = no pairing support at all (auth = 0)
//
// Measured conclusion (2026-09-06):
//   mode 2 -> indication subscription succeeds (the only verified working setting)
//   mode 1 -> subscription fails
//   mode 0 -> subscription fails
//
// Mode 2 is therefore kept for now so the feature works. Removing bonding is not
// yet achieved -- why mode 1 fails is still unclear (how the Cordio app framework
// handles CCCD writes and pairing under AppSetBondable(FALSE) is the next thing
// to investigate), and it is not solved by simply dropping the auth flags.
//
// Also note: "reset cmpl repeating every 10 seconds" is a separate problem
// unrelated to the pairing configuration and present in mode 2 as well (visible
// as about 10 seconds from boot to the LED starting to blink); it is to be
// addressed later.
//
// The notes below record the design intent behind modes 0 and 1:
//
// All three CCCDs have security level DM_SEC_LEVEL_NONE (see tmpsCccSet), so
// writing a CCCD needs no encryption in itself. That does not mean pairing can be
// declared unsupported -- modern phones (iOS / Android) often initiate LE Secure
// Connections pairing after connecting, and a device answering "SC not supported"
// fails the negotiation, which can take CCCD writes and indication subscription
// down with it. Subscription failures were indeed observed with auth = 0.
//
// Mode 1 keeps the SC capability so pairing can succeed, but drops
// DM_AUTH_BOND_FLAG and calls AppSetBondable(FALSE), so no long-term key is
// stored -- every connection starts fresh and the phone keeps no bonding record.
// That is the "no pairing persistence" a device can decide unilaterally; "the
// peer never initiates pairing" requires the peer's cooperation and cannot be
// forced by the device.
//
// Note: removing SC does not shorten the boot-to-advertising delay. Measurement
// shows the delay is still about 10 seconds with DmSecGenerateEccKeyReq() skipped;
// the bottleneck is not ECC but AttsCalculateDbHash() -- this project links
// sec_cmac_hci.c, where every 16-byte AES-CMAC block costs one HCI round trip to
// the EM9305.
//
//*****************************************************************************
#define TMPS_PAIRING_MODE            2

/*! Security configuration */
static const appSecCfg_t tmpsSecCfg =
{
#if (TMPS_PAIRING_MODE == 2)
  DM_AUTH_BOND_FLAG |
    DM_AUTH_SC_FLAG |
    DM_AUTH_MITM_FLAG,                    /*! Authentication and bonding flags */
  0,                                      /*! Initiator key distribution flags */
  DM_KEY_DIST_LTK,                        /*! Responder key distribution flags */
#elif (TMPS_PAIRING_MODE == 1)
  DM_AUTH_SC_FLAG |
    DM_AUTH_MITM_FLAG,                    /*! SC supported but BOND not set: no key stored */
  0,                                      /*! Initiator key distribution flags */
  DM_KEY_DIST_LTK,                        /*! SC pairing still distributes an LTK (never persisted) */
#else
  0,                                      /*! No authentication, no bonding, no SC */
  0,                                      /*! Initiator distributes no keys */
  0,                                      /*! Responder distributes no keys */
#endif
  FALSE,                                  /*! TRUE if out-of-band pairing data is present */
  FALSE                                    /*! TRUE to initiate security once connected */
};

// For power testing only: update the connection interval to 500 ms with zero latency
#if POWER_CONSUMPTION_TEST
/*! Connection parameter update configuration */
static const appUpdateCfg_t tmpsUpdateCfg =
{
  100,                                    /*! Connection idle time before attempting a parameter update,
                                              in milliseconds; 0 disables it */
  400,                                    /*! Minimum connection interval in 1.25 ms units */
  400,                                    /*! Maximum connection interval in 1.25 ms units */
  0,                                      /*! Connection latency */
  600,                                    /*! Supervision timeout in 10 ms units */
  5                                       /*! Update attempts before giving up */
};

#else

/*! Connection parameter update configuration */
static const appUpdateCfg_t tmpsUpdateCfg =
{
  5000,                                   /*! Connection idle time before attempting a parameter update,
                                              in milliseconds; 0 disables it */
  12,                                     /*! Minimum connection interval in 1.25 ms units (15 ms) */
  24,                                     /*! Maximum connection interval in 1.25 ms units (30 ms) */
  0,                                      /*! Connection latency */
  400,                                    /*! Supervision timeout in 10 ms units (4 s) */
  3                                       /*! Update attempts before giving up */
};
#endif

/*! Temperature measurement configuration */
static const htpsCfg_t tmpsHtpsCfg =
{
  TMPS_TM_MEAS_PERIOD_MS   /*! Measurement timer expiry period, in milliseconds */
};

/*! Battery measurement configuration */
static const basCfg_t tmpsBasCfg =
{
  30,       /*! Battery measurement timer expiry period, in seconds */
  1,        /*! Perform a battery measurement after this many timer periods */
  100       /*! Notify the peer of the level when the charge drops below this */
};

/*! SMP security parameter configuration
 *
 *  ioCap is SMP_IO_DISP_YES_NO: this device has a display for the six digits and
 *  can express Yes / No, while the phone's ioCap is KeyboardDisplay. Under LE
 *  Secure Connections with MITM protection required on both sides, the SMP
 *  pairing method table selects Numeric Comparison.
 *
 *  It used to be SMP_IO_NO_IN_NO_OUT, which falls through to Just Works -- no
 *  matter how the auth flags are set, DM_SEC_COMPARE_IND never occurs and there
 *  are no digits to display. The other half of the condition is that
 *  tmpsSecCfg.auth must carry DM_AUTH_MITM_FLAG; neither alone is enough. */
static const smpCfg_t tmpsSmpCfg =
{
  3000,                                   /*! "Repeated attempts" timeout, in milliseconds */
  SMP_IO_DISP_YES_NO,                     /*! I/O capability: see the note above */
  7,                                      /*! Minimum encryption key length */
  16,                                     /*! Maximum encryption key length */
  3,                                      /*! Attempts that trigger the repeated-attempts timeout */
  0,                                      /*! Device authentication requirements */
  64000,                                  /*! Maximum repeated-attempts timeout, in milliseconds */
  64000,                                  /*! Time before attemptExp is decremented, in milliseconds */
  2                                       /*! Exponent of the repeated-attempts multiplier */
};

#ifdef AM_BLE_EATT
/* Priority required for each channel */
uint8_t eatt_pri[] = {0x1, 0x1};
/* EATT configuration structure */
static const eattCfg_t fiteattCfg =
{
  247,                              /* MTU */
  247,                              /* MPS */
  TRUE,                             /* Open EATT channels automatically on connection */
  FALSE,                            /* Whether authorization is required */
  DM_SEC_LEVEL_NONE,                /* Required security level */
  2,                                /* Enhanced L2CAP channels per connection */
  eatt_pri                          /* Channel priority table */
};
#endif

/**************************************************************************************************
  Advertising data
**************************************************************************************************/

/*! Device name: used both for the scan response packet (shown in scan lists) and
 *  for the GAP Device Name (read after connecting). The two must agree, or a
 *  phone shows one name while scanning and another once connected. This line is
 *  the only place to change. */
#define TMPS_DEVICE_NAME  "EdgePilot-510B"

// For power testing only: fill the advertising data to the full 31 bytes
#if POWER_CONSUMPTION_TEST
/*! Advertising data for discoverable mode */
static const uint8_t tmpsAdvDataDisc[] =
{
  /*! Flags */
  2,                                      /*! Length */
  DM_ADV_TYPE_FLAGS,                      /*! AD type */
  DM_FLAG_LE_GENERAL_DISC |               /*! Flags */
  DM_FLAG_LE_BREDR_NOT_SUP,

  /*! Transmit power */
  2,                                      /*! Length */
  DM_ADV_TYPE_TX_POWER,                   /*! AD type */
  0,                                      /*! Transmit power */

  /*! Service UUID list */
  9,                                      /*! Length */
  DM_ADV_TYPE_16_UUID,                    /*! AD type */
  UINT16_TO_BYTES(ATT_UUID_HEART_RATE_SERVICE),
  UINT16_TO_BYTES(ATT_UUID_RUNNING_SPEED_SERVICE),
  UINT16_TO_BYTES(ATT_UUID_DEVICE_INFO_SERVICE),
  UINT16_TO_BYTES(ATT_UUID_BATTERY_SERVICE),

  /*! Device name */
  14,                                     /*! Length */
  DM_ADV_TYPE_LOCAL_NAME,                 /*! AD type */
  'P',
  'o',
  'w',
  'e',
  'r',
  ' ',
  't',
  'e',
  's',
  't',
  'i',
  'n',
  'g'
};
#else

/*! Advertising data for discoverable mode */
static const uint8_t tmpsAdvDataDisc[] =
{
  /*! Flags */
  2,                                      /*! Length */
  DM_ADV_TYPE_FLAGS,                      /*! AD type */
  DM_FLAG_LE_GENERAL_DISC |               /*! Flags */
  DM_FLAG_LE_BREDR_NOT_SUP,

  /*! Transmit power */
  2,                                      /*! Length */
  DM_ADV_TYPE_TX_POWER,                   /*! AD type */
  0,                                      /*! Transmit power */

  /*! Service UUID list */
  7,                                      /*! Length */
  DM_ADV_TYPE_16_UUID,                    /*! AD type */
  /* Health Thermometer Service (UUID 0x1809): declares that this device provides HTS temperature measurement. */
  UINT16_TO_BYTES(ATT_UUID_HEALTH_THERM_SERVICE),
  /* Device Information Service (UUID 0x180A): manufacturer, model and firmware version can be read. */
  UINT16_TO_BYTES(ATT_UUID_DEVICE_INFO_SERVICE),
  /* Battery Service (UUID 0x180F): the battery level can be read and level change notifications subscribed. */
  UINT16_TO_BYTES(ATT_UUID_BATTERY_SERVICE)
};

#endif

/*! Scan response data for discoverable mode
 *
 *  A structure describes the single AD (Advertising Data) field, so the length
 *  byte is derived from TMPS_DEVICE_NAME automatically and need not be kept in
 *  sync by hand on a rename. Every member is a 1-byte type, so the compiler
 *  inserts no padding and the memory layout is identical to the original byte
 *  array. */
static const struct
{
  uint8_t len;                            /*! Length = AD type (1) + name characters */
  uint8_t advType;                        /*! AD type */
  char    name[sizeof(TMPS_DEVICE_NAME) - 1];  /*! Name characters (without the trailing '\0') */
} tmpsScanDataDisc =
{
  /*! Device name */
  sizeof(TMPS_DEVICE_NAME),               /*! Name characters + '\0' happens to equal the required length */
  DM_ADV_TYPE_LOCAL_NAME,
  TMPS_DEVICE_NAME                        /*! At exactly this length, C drops the trailing '\0' */
};

/**************************************************************************************************
  Client characteristic configuration descriptors (CCCD)
**************************************************************************************************/

/*! Client characteristic configuration descriptor (CCCD) index enumeration */
enum
{
  TMPS_GATT_SC_CCC_IDX,                    /*! Service Changed characteristic of the GATT service */
  TMPS_HTS_TM_CCC_IDX,                    /*! Temperature Measurement characteristic of the health thermometer service */
  TMPS_BATT_LVL_CCC_IDX,                  /*! Battery Level characteristic of the battery service */
  TMPS_NUM_CCC_IDX
};

/*! Client characteristic configuration descriptor (CCCD) set, indexed by the enumeration above */
static const attsCccSet_t tmpsCccSet[TMPS_NUM_CCC_IDX] =
{
  /* CCCD handle          permitted value range      security level */
  {GATT_SC_CH_CCC_HDL,    ATT_CLIENT_CFG_INDICATE,  DM_SEC_LEVEL_NONE},   /* TMPS_GATT_SC_CCC_IDX */
  {HTS_TM_CH_CCC_HDL,     ATT_CLIENT_CFG_INDICATE,  DM_SEC_LEVEL_NONE},   /* TMPS_HTS_TM_CCC_IDX */
  {BATT_LVL_CH_CCC_HDL,   ATT_CLIENT_CFG_NOTIFY,    DM_SEC_LEVEL_NONE}    /* TMPS_BATT_LVL_CCC_IDX */
};

/**************************************************************************************************
  Global variables
**************************************************************************************************/

/*! WSF handler ID */
wsfHandlerId_t tmpsHandlerId;

/*! BLE status LED: blink timer and whether advertising is in progress */
static wsfTimer_t tmpsLedTimer;
static bool_t     tmpsAdvActive;


/*************************************************************************************************/
/*!
 *  \brief  Application DM callback.
 *
 *  \param  pDmEvt  DM callback event.
 *
 *  \return None.
 */
/*************************************************************************************************/
static void tmpsDmCback(dmEvt_t *pDmEvt)
{
  dmEvt_t *pMsg;
  uint16_t len;

  len = DmSizeOfEvt(pDmEvt);

  if ((pMsg = WsfMsgAlloc(len)) != NULL)
  {
    memcpy((uint8_t *)pMsg, (uint8_t *)pDmEvt, len);
    WsfMsgSend(tmpsHandlerId, pMsg);
  }
}

/*************************************************************************************************/
/*!
 *  \brief  Application ATT callback.
 *
 *  \param  pEvt    ATT callback event.
 *
 *  \return None.
 */
/*************************************************************************************************/
static void tmpsAttCback(attEvt_t *pEvt)
{
  attEvt_t *pMsg;

  if ((pMsg = WsfMsgAlloc(sizeof(attEvt_t) + pEvt->valueLen)) != NULL)
  {
    memcpy(pMsg, pEvt, sizeof(attEvt_t));
    pMsg->pValue = (uint8_t *) (pMsg + 1);
    memcpy(pMsg->pValue, pEvt->pValue, pEvt->valueLen);
    WsfMsgSend(tmpsHandlerId, pMsg);
  }
}

/*************************************************************************************************/
/*!
 *  \brief  Application ATTS client characteristic configuration callback.
 *
 *  \param  pDmEvt  DM callback event.
 *
 *  \return None.
 */
/*************************************************************************************************/
static void tmpsCccCback(attsCccEvt_t *pEvt)
{
  attsCccEvt_t  *pMsg;
  appDbHdl_t    dbHdl;

  /* Print CCCD writes directly (not through APP_TRACE_INFO, so it is unaffected
   * by WSF_TRACE_ENABLED). This is the most direct evidence of whether the peer
   * subscribed successfully:
   *   idx=1 (TMPS_HTS_TM_CCC_IDX) with value=0x0002 means temperature
   *   indications are enabled.
   * No such line at all means the peer never wrote the CCCD. */
  am_util_stdio_printf("[ccc] idx=%u handle=0x%04X value=0x%04X connId=%u\r\n",
                       (unsigned)pEvt->idx,
                       (unsigned)pEvt->handle,
                       (unsigned)pEvt->value,
                       (unsigned)pEvt->hdr.param);

  /* If this CCC was not set by initialization, and a bond with a device record exists */
  if ((pEvt->handle != ATT_HANDLE_NONE) &&
      ((dbHdl = AppDbGetHdl((dmConnId_t) pEvt->hdr.param)) != APP_DB_HDL_NONE) &&
      AppCheckBonded((dmConnId_t)pEvt->hdr.param))
  {
    /* Store the setting in the device database. */
    AppDbSetCccTblValue(dbHdl, pEvt->idx, pEvt->value);
  }

  if ((pMsg = WsfMsgAlloc(sizeof(attsCccEvt_t))) != NULL)
  {
    memcpy((uint8_t *)pMsg, (uint8_t *)pEvt, sizeof(attsCccEvt_t));
    WsfMsgSend(tmpsHandlerId, pMsg);
  }
}



/*************************************************************************************************/
/*!
 *  \brief  Handle a CCC state change.
 *
 *  \param  pMsg    Message pointer.
 *
 *  \return None.
 */
/*************************************************************************************************/
static void tmpsProcCccState(tmpsMsg_t *pMsg)
{
  APP_TRACE_INFO3("ccc state ind value:%d handle:%d idx:%d", pMsg->ccc.value, pMsg->ccc.handle, pMsg->ccc.idx);

  /* Handle the HTS Temperature Measurement CCC */
  if (pMsg->ccc.idx == TMPS_HTS_TM_CCC_IDX)
  {
    if (pMsg->ccc.value == ATT_CLIENT_CFG_INDICATE)
    {
      /* Send the first measurement immediately; schedule the rest once a confirmation arrives */
      HtpsMeasComplete((dmConnId_t) pMsg->ccc.hdr.param, TMPS_HTS_TM_CCC_IDX);
    }
    else
    {
      HtpsMeasStop();
    }
    return;
  }

  /* Handle the Battery Level CCC */
  if (pMsg->ccc.idx == TMPS_BATT_LVL_CCC_IDX)
  {
    if (pMsg->ccc.value == ATT_CLIENT_CFG_NOTIFY)
    {
      BasMeasBattStart((dmConnId_t) pMsg->ccc.hdr.param, TMPS_BATT_TIMER_IND, TMPS_BATT_LVL_CCC_IDX);
    }
    else
    {
      BasMeasBattStop((dmConnId_t) pMsg->ccc.hdr.param);
    }
    return;
  }
}

/*************************************************************************************************/
/*!
 *  \brief  Perform UI actions when a connection closes.
 *
 *  \param  pMsg    Message pointer.
 *
 *  \return None.
 */
/*************************************************************************************************/
static void tmpsClose(tmpsMsg_t *pMsg)
{
  /* Stop the temperature measurements */
  HtpsMeasStop();

  /* Stop the battery measurements */
  BasMeasBattStop((dmConnId_t) pMsg->hdr.param);
}

/*************************************************************************************************/
/*!
 *  \brief  Set up advertising and the other procedures that must run
 *          after the device resets.
 *
 *  \param  pMsg    Message pointer.
 *
 *  \return None.
 */
/*************************************************************************************************/
static void tmpsSetup(tmpsMsg_t *pMsg)
{
  /* Set the advertising and scan response data for discoverable mode */
  AppAdvSetData(APP_ADV_DATA_DISCOVERABLE, sizeof(tmpsAdvDataDisc), (uint8_t *) tmpsAdvDataDisc);
  AppAdvSetData(APP_SCAN_DATA_DISCOVERABLE, sizeof(tmpsScanDataDisc), (uint8_t *) &tmpsScanDataDisc);

  /* Set the advertising and scan response data for connectable mode */
  AppAdvSetData(APP_ADV_DATA_CONNECTABLE, sizeof(tmpsAdvDataDisc), (uint8_t *) tmpsAdvDataDisc);
  AppAdvSetData(APP_SCAN_DATA_CONNECTABLE, sizeof(tmpsScanDataDisc), (uint8_t *) &tmpsScanDataDisc);

  /* Start advertising; connectable / discoverable and bondable modes are set automatically */
  AppAdvStart(APP_MODE_AUTO_INIT);

  /* Only mode 2 allows bonding (storing the LTK); see TMPS_PAIRING_MODE */
  AppSetBondable((TMPS_PAIRING_MODE == 2) ? TRUE : FALSE);
}

/*************************************************************************************************/
/*!
 *  \brief  Button callback.
 *
 *  \param  btn    Button event.
 *
 *  \return None.
 */
/*************************************************************************************************/
static void tmpsBtnCback(uint8_t btn)
{
  dmConnId_t      connId;

  /* Button actions while connected */
  if ((connId = AppConnIsOpen()) != DM_CONN_ID_NONE)
  {
    switch (btn)
    {
      case APP_UI_BTN_1_SHORT:
        /*
         * Record the latest temperature reading.
         *
         * This callback runs in the WSF/RadioTask context, so it only reads the
         * SensorTask cache and never touches I2C directly -- that would contend
         * with SensorTask over IOM1 (the am_hal_iom handle is not thread-safe).
         * The same approach as AppHwTmRead().
         */
        {
          int32_t i32TempC100 = TMP119_TEMP_INVALID;

          temp_cache_get(&i32TempC100, TEMP_CACHE_MAX_AGE_TICKS);
          APP_TRACE_INFO1("TMP119 %d (C x100)", i32TempC100);
        }
        break;

      case APP_UI_BTN_1_LONG:
        AppConnClose(connId);
        break;

      default:
        break;
    }
  }
  /* Button actions while not connected */
  else
  {
    switch (btn)
    {
      case APP_UI_BTN_1_SHORT:
        /* Start or restart advertising */
        AppAdvStart(APP_MODE_AUTO_INIT);
        break;

      case APP_UI_BTN_1_MED:
        /* Enter discoverable mode (only mode 2 allows bonding; see TMPS_PAIRING_MODE) */
        AppSetBondable((TMPS_PAIRING_MODE == 2) ? TRUE : FALSE);
        AppAdvStart(APP_MODE_DISCOVERABLE);
        break;

      case APP_UI_BTN_1_LONG:
        /* Clear all bonding information */
        AppSlaveClearAllBondingInfo();

        /* Restart advertising */
        AppAdvStart(APP_MODE_AUTO_INIT);
        break;

      default:
        break;
    }
  }
}

//*****************************************************************************
//
// Stores the HciDrvHandler handler ID so events can be sent to it
// through the WSF task system.
//
// Note: every Cordio application must add the following two lines
// to its exactle initialization function at startup:
//
//     handlerId = WsfOsSetNextHandler(HciDrvHandler);
//     HciDrvHandler(handlerId);
//
//*****************************************************************************
wsfHandlerId_t g_CustHandleID = 0xFF;
void CustHandlerInit(wsfHandlerId_t handlerId)
{
    g_CustHandleID = handlerId;
}

/*************************************************************************************************/
/*! 
 *  \brief  Drive the BLE status LED (LED3 / GP19) from the current connection state.
 *
 *  Connected outranks advertising: when a central connects, both DM_ADV_STOP_IND
 *  and DM_CONN_OPEN_IND arrive, but their order is not guaranteed.
 *
 *  \return None.
 */
/*************************************************************************************************/
static void tmpsLedUpdate(void)
{
  if (AppConnIsOpen() != DM_CONN_ID_NONE)
  {
    /* Connected: steady on */
    APP_TRACE_INFO0("[led] connected - solid");
    WsfTimerStop(&tmpsLedTimer);
    BleLedOn();
  }
  else if (tmpsAdvActive)
  {
    /* Advertising: blinking */
    APP_TRACE_INFO0("[led] advertising - blink");
    BleLedOn();
    WsfTimerStartMs(&tmpsLedTimer, TMPS_LED_BLINK_MS);
  }
  else
  {
    /* Idle: off */
    APP_TRACE_INFO0("[led] idle - off");
    WsfTimerStop(&tmpsLedTimer);
    BleLedOff();
  }
}

/*************************************************************************************************/
/*!
 *  \brief  Central handler for the WSF / Cordio messages this application receives.
 *
 *  Messages fall into three groups:
 *  - TMPS_*_TIMER_IND: WSF software timer events created by this application.
 *  - ATTS_*, ATT_*: events from the ATT server and the shared ATT layer, such as
 *    CCC settings, indication confirmations and GATT Database Hash completion.
 *  - DM_*: Device Manager controller, advertising, connection and security events.
 *
 *  Most branches complete a profile or application operation directly; where the
 *  button / LED UI state has to change, uiEvent is set first and AppUiAction() is
 *  called once after the switch.
 *
 *  \param  pMsg    Message pointer.
 *
 *  \return None.
 */
/*************************************************************************************************/
static void tmpsProcMsg(tmpsMsg_t *pMsg)
{
  /* No UI action by default; only the relevant DM events overwrite this. */
  uint8_t uiEvent = APP_UI_NONE;

  switch(pMsg->hdr.event)
  {
    case TMPS_TM_TIMER_IND:
      /* Temperature measurement period expired:
       * HtpsMeasComplete() first confirms that this connection's TM CCCD has
       * indications enabled, then takes the TMP119 temperature through
       * AppHwTmRead(), builds the IEEE-11073 HTS packet and sends an indication
       * on HTS_TM_HDL (Temperature Measurement, UUID 0x2A1C).
       * TMPS_HTS_TM_CCC_IDX is this characteristic's index in the CCC table. */
      HtpsMeasComplete((dmConnId_t) pMsg->hdr.param, TMPS_HTS_TM_CCC_IDX);
      break;

    case TMPS_BATT_TIMER_IND:
      /* Battery measurement period expired; the Battery Service profile reads and handles the value. */
      BasProcMsg(&pMsg->hdr);
      break;

    case TMPS_LED_TIMER_IND:
      /* LED blink beat while advertising: toggle once, then restart the next blink period. */
      BleLedToggle();
      WsfTimerStartMs(&tmpsLedTimer, TMPS_LED_BLINK_MS);
      break;

    case ATTS_HANDLE_VALUE_CNF:
      /* Only one indication may be outstanding at a time, so the next measurement
       * is scheduled once the confirmation arrives. HtpsMeasComplete() already
       * stopped the timer when sending; HtpsMeasStart() restarts it at the
       * configured period. It must restart even when the confirmation failed or
       * timed out, since HtpsMeasComplete() rechecks the CCC before the next send. */
      if (pMsg->att.handle == HTS_TM_HDL)
      {
        HtpsMeasStart((dmConnId_t) pMsg->hdr.param, TMPS_TM_TIMER_IND, TMPS_HTS_TM_CCC_IDX);
      }
      BasProcMsg(&pMsg->hdr);
      break;

    case ATTS_CCC_STATE_IND:
      /* The ATT server emits this after a central writes any CCCD.
       * tmpsProcCccState() starts or stops battery notifications and temperature
       * indications from the CCC index and the 0x0001 / 0x0002 value. */
      tmpsProcCccState(pMsg);
      break;

    case ATT_MTU_UPDATE_IND:
      /* ATT MTU negotiation complete; only the final MTU is logged here, no HTS repacking is needed. */
      APP_TRACE_INFO1("Negotiated MTU %d", ((attEvt_t *)pMsg)->mtu);
      break;

    case DM_RESET_CMPL_IND:
    {
      /* Only after the controller reset completes can the initialization below, which depends on ATT and security state, run. */
      uint8_t addr[BDA_ADDR_LEN];

      /* Generate a random static address and set it as this session's BLE device address. */
      SecRand(addr, BDA_ADDR_LEN);
      DM_RAND_ADDR_SET(addr, DM_RAND_ADDR_STATIC);
      DmDevSetRandAddr(addr);
#ifdef AM_BLE_USE_NVM
        /* With NVM in use, load the previously saved resolving list and bonding data. */
        AppLoadResList();
#endif
      // After reset, mark the database hash calculation as pending until a new hash is produced
      attsCsfSetHashUpdateStatus(TRUE);

      // Generate ECC keys if Secure Connections is configured,
      // otherwise calculate the ATT database hash
      am_util_stdio_printf("[ble] t=%u ms  reset cmpl, auth=0x%02X\r\n",
                           (unsigned)xTaskGetTickCount(),
                           (unsigned)tmpsSecCfg.auth);

      if( tmpsSecCfg.auth & DM_AUTH_SC_FLAG )
      {
          DmSecGenerateEccKeyReq();
      }
      else
      {
          AttsCalculateDbHash();
      }

      uiEvent = APP_UI_RESET_CMPL;
    }
      break;

    case ATTS_DB_HASH_CALC_CMPL_IND:
      /* The ATT server has finished updating the GATT Database Hash (computed with CMAC):
       * 1. Cordio has written the new 16-byte hash into the GATT Database Hash characteristic;
       * 2. this event does not send the hash to the central, it tells the application "the database is ready";
       * 3. only now is tmpsSetup() called to load the advertising / scan response
       *    data, start advertising and set the bondable mode.
       *
       * The event comes from the asynchronous completion of AttsCalculateDbHash();
       * with Secure Connections enabled, the calculation waits for the ECC keys
       * from DM_SEC_ECC_KEY_IND first.
       *
       * The difference between this timestamp and the reset cmpl one above is the
       * total cost of ECC (if any) plus the database hash. This project links
       * sec_cmac_hci.c, where every 16-byte AES-CMAC block costs one HCI round
       * trip, which makes this stretch the prime suspect for the "boot to
       * advertising" delay. */
      am_util_stdio_printf("[ble] t=%u ms  db hash cmpl -> start advertising\r\n",
                           (unsigned)xTaskGetTickCount());
      tmpsSetup(pMsg);
      break;

    case DM_ADV_SET_START_IND:
      /* The advertising set start procedure finished; notify the UI state machine, though advertising has not necessarily begun. */
      uiEvent = APP_UI_ADV_SET_START_IND;
      break;

    case DM_ADV_SET_STOP_IND:
      /* The advertising set stop procedure finished; notify the UI state machine. */
      uiEvent = APP_UI_ADV_SET_STOP_IND;
      break;

     case DM_ADV_START_IND:
      /* The controller has actually begun sending advertising packets: record the
       * advertising state, update the LED and notify the custom HCI driver handler (if registered). */
      uiEvent = APP_UI_ADV_START;
      tmpsAdvActive = TRUE;
      tmpsLedUpdate();
      if(g_CustHandleID != 0xFF)
      {
        WsfSetEvent(g_CustHandleID, CUST_BLE_ADV_START_EVENT);
      }
      break;

    case DM_ADV_STOP_IND:
      /* The controller has stopped sending advertising packets: clear the advertising state and update the LED. */
      uiEvent = APP_UI_ADV_STOP;
      tmpsAdvActive = FALSE;
      tmpsLedUpdate();
      break;

    case DM_CONN_OPEN_IND:
      /* A central has connected:
       * - let the Battery Service handle its connection state;
       * - no longer advertising, so show the connected state on the LED;
       * - notify the registered custom handler. */
      am_util_stdio_printf("[ble] t=%u ms  conn open, connId=%u\r\n",
                           (unsigned)xTaskGetTickCount(),
                           (unsigned)pMsg->hdr.param);

      BasProcMsg(&pMsg->hdr);

      //
      // Every connection starts from a clean state. On the normal path
      // DM_CONN_CLOSE_IND has already cleared it; this covers the cases that never
      // reach a close event -- hciCmdTimeout() in this project is known to go
      // straight to HciDrvRadioShutdown() + HciDrvRadioBoot() + DmDevReset(), and
      // when that happens mid-connection a conn close is not guaranteed to follow.
      //
      ble_pair_reset();
      ble_pair_conn_set(true);

      // AppSlaveSecurityReq(1);  // Request security on connection ID 1 (currently disabled)
      uiEvent = APP_UI_CONN_OPEN;
      tmpsAdvActive = FALSE;
      tmpsLedUpdate();
      if(g_CustHandleID != 0xFF)
      {
        WsfSetEvent(g_CustHandleID, CUST_BLE_CONN_OPEN_EVENT);
      }
      break;

    case DM_CONN_CLOSE_IND:
      {
        /* The connection dropped: stop the HTS and battery measurement procedures,
         * update the UI / LED, and log the real reason from the HCI disconnect complete event. */
        hciDisconnectCmplEvt_t *evt = (hciDisconnectCmplEvt_t*) pMsg;

        /* The disconnect reason is printed with plain printf, unaffected by WSF_TRACE_ENABLED.
         * When subscription fails this reason code is often the key:
         *   0x13 remote terminated, 0x16 local terminated, 0x08 connection supervision timeout,
         *   0x3D MIC failure (encryption problem), 0x22 LMP/LL response timeout. */
        am_util_stdio_printf("[ble] t=%u ms  conn close, reason=0x%02X\r\n",
                             (unsigned)xTaskGetTickCount(),
                             (unsigned)evt->reason);

        //
        // After a disconnect the display returns to "TMP119 - IOM1": Pass / Fail
        // only mean something during the connection in which pairing happened, and
        // leaving them up would suggest the pairing is still in effect.
        //
        // This also covers another case: some phones do not send SMP Pairing
        // Failed when the user dismisses the numeric comparison dialog and simply
        // disconnect (reason 0x13, remote terminated). The state is then still
        // "comparing", and without this the six digits would stay on screen.
        //
        ble_pair_reset();
        ble_pair_conn_set(false);

        tmpsClose(pMsg);
        uiEvent = APP_UI_CONN_CLOSE;
        tmpsLedUpdate();
      }
      break;

    case DM_PHY_UPDATE_IND:
      /* PHY update complete; status is the result, rxPhy / txPhy the negotiated receive / transmit PHY. */
      APP_TRACE_INFO3("DM_PHY_UPDATE_IND status: %d, RX: %d, TX: %d", pMsg->dm.phyUpdate.status,pMsg->dm.phyUpdate.rxPhy, pMsg->dm.phyUpdate.txPhy);
      break;

    case DM_SEC_PAIR_CMPL_IND:
      /* After successful pairing, request or refresh the ECC keys from DM and tell
       * the UI that the security procedure is done. Pressing Yes on the phone
       * takes this path, and the display shows Paring Pass. */
      am_util_stdio_printf("[ble] t=%u ms  pair cmpl -> pass\r\n",
                           (unsigned)xTaskGetTickCount());

      ble_pair_result_set(true);

      DmSecGenerateEccKeyReq();
      uiEvent = APP_UI_SEC_PAIR_CMPL;
      break;

    case DM_SEC_PAIR_FAIL_IND:
      /* A pairing failure also requests or refreshes the ECC keys and moves the UI
       * into the pairing-failed state. When the user presses No or cancels, the
       * peer sends SMP Pairing Failed (reason usually 0x04 Confirm Value Failed or
       * 0x0C Numeric Comparison Failed), which converges here into Paring Fail on screen. */
      am_util_stdio_printf("[ble] t=%u ms  pair fail, status=0x%02X -> fail\r\n",
                           (unsigned)xTaskGetTickCount(),
                           (unsigned)pMsg->hdr.status);

      ble_pair_result_set(false);

      DmSecGenerateEccKeyReq();
      uiEvent = APP_UI_SEC_PAIR_FAIL;
      break;

    case DM_SEC_ENCRYPT_IND:
      /* The connection is now encrypted. If the central already subscribed to
       * temperature indications earlier in this connection, send one measurement
       * immediately rather than waiting for the next TMPS_TM_TIMER_IND. */
      uiEvent = APP_UI_SEC_ENCRYPT;
      if(AttsCccEnabled((dmConnId_t) pMsg->dm.hdr.param, TMPS_HTS_TM_CCC_IDX))
      {
        APP_TRACE_INFO0("encrypted, ccc enabled\r\n");
        HtpsMeasComplete((dmConnId_t) pMsg->dm.hdr.param, TMPS_HTS_TM_CCC_IDX);
      }
      break;

    case DM_SEC_ENCRYPT_FAIL_IND:
      /* Encryption failed; only the UI is updated, and sending measurements still waits on security and CCC state. */
      uiEvent = APP_UI_SEC_ENCRYPT_FAIL;
      break;

    case DM_SEC_AUTH_REQ_IND:
      /* The peer requested authentication; the shared app layer handles the passkey and pairing method. */
      AppHandlePasskey(&pMsg->dm.authReq);
      break;

    case DM_SEC_ECC_KEY_IND:
      /* DM returns the ECC keys: hand them to DM; if the database hash was marked
       * pending at reset, AttsCalculateDbHash() can start only now, and its
       * completion returns to the ATTS_DB_HASH_CALC_CMPL_IND branch above. */
      am_util_stdio_printf("[ble] t=%u ms  ECC key ready -> calc db hash\r\n",
                           (unsigned)xTaskGetTickCount());

      DmSecSetEccKey(&pMsg->dm.eccMsg.data.key);
      // Calculate the database hash only while the hash update is in progress
      if( attsCsfGetHashUpdateStatus() )
      {
        AttsCalculateDbHash();
      }
      break;

    case DM_SEC_COMPARE_IND:
      /* Numeric Comparison: the controller's 16-byte confirm value is turned into
       * six decimal digits by DmSecGetCompareValue(), and both ends must show the same.
       *
       * AppHandleNumericComparison() is deliberately not called: it passes the
       * digits to AppUiDisplayConfirmValue() (printf only, invisible on screen),
       * whereas what is wanted here is to store them in ble_pair for RenderTask to
       * draw. Apart from the display target the behaviour is identical -- this end
       * always accepts (DmSecCompareRsp TRUE) and the phone's Yes / No decides the
       * outcome, matching the Nordic version where auth_passkey_confirm() calls
       * bt_conn_auth_passkey_confirm() directly. */
      {
        uint32_t ui32Confirm = DmSecGetCompareValue(pMsg->dm.cnfInd.confirm);

        am_util_stdio_printf("[ble] t=%u ms  numeric comparison: %06u\r\n",
                             (unsigned)xTaskGetTickCount(),
                             (unsigned)ui32Confirm);

        ble_pair_compare_set(ui32Confirm);

        DmSecCompareRsp((dmConnId_t)pMsg->dm.cnfInd.hdr.param, TRUE);
      }
      break;

    case DM_PRIV_CLEAR_RES_LIST_IND:
      /* The privacy resolving list has been cleared; status is the controller's result. */
      APP_TRACE_INFO1("Clear resolving list status 0x%02x", pMsg->hdr.status);
      break;

    case DM_HW_ERROR_IND:
      /* The controller reports a hardware error; the UI displays and handles the error state.
       *
       * This event is the key evidence for the "reset repeating every 10 seconds":
       * hci_drv_em9305.c has a 10-second heartbeat (HEARTBEAT_TIMEOUT_MS = 10000)
       * that periodically probes the radio with an HCI command, and a command
       * timeout raises this event; the Cordio app framework (DM_HW_ERROR_IND in
       * app_slave.c) then calls HciDrvRadioBoot(0) + DmDevReset() and
       * reinitializes the whole radio. Externally this looks like a round of
       * "reset cmpl -> start advertising" every 10 seconds, the stack never
       * settles into a connectable state, and indication subscription is bound to fail.
       *
       * If this line appears in the log, the problem is confirmed as an HCI communication timeout rather than BLE configuration or pairing. */
      am_util_stdio_printf("[ble] t=%u ms  *** DM_HW_ERROR_IND (HCI timeout) -> radio reboot ***\r\n",
                           (unsigned)xTaskGetTickCount());
      uiEvent = APP_UI_HW_ERROR;
      break;

    case DM_VENDOR_SPEC_IND:
      {
        /* A vendor-specific HCI event (event code 0xFF) arrived from the controller;
         * hdr.param holds the payload length in this structure, and the bytes are printed below for debugging. */
        uint8_t *pParam = pMsg->dm.vendorSpec.param;
        uint16_t len = pMsg->dm.vendorSpec.hdr.param;   /* The length is stored in hdr.param */
        uint8_t status = pMsg->dm.vendorSpec.hdr.status;

        APP_TRACE_INFO2("DM_VENDOR_SPEC_IND: len=%d, status=0x%02X", len, status);

        /* Print all the vendor-specific event data */
        APP_TRACE_INFO1("Vendor Specific Event Data, len:%d", len);
        for (uint16_t i = 0; i < len; i++)
        {
          APP_TRACE_INFO2("  [%02d]: 0x%02X", i, pParam[i]);
        }
      }
      break;

    case DM_VENDOR_SPEC_CMD_CMPL_IND:
      {
        /* Controller vendor-specific command complete; only the Apollo / Apollo2 platforms decode known opcodes. */
        #if defined(AM_PART_APOLLO) || defined(AM_PART_APOLLO2)

          uint8_t *param_ptr = &pMsg->dm.vendorSpecCmdCmpl.param[0];

          switch (pMsg->dm.vendorSpecCmdCmpl.opcode)
          {
            case 0xFC20: // Read the given address
            {
              uint32_t read_value;

              BSTREAM_TO_UINT32(read_value, param_ptr);

              APP_TRACE_INFO3("VSC 0x%0x complete status %x param %x",
                pMsg->dm.vendorSpecCmdCmpl.opcode,
                pMsg->hdr.status,
                read_value);
            }

            break;
            default:
                APP_TRACE_INFO2("VSC 0x%0x complete status %x",
                    pMsg->dm.vendorSpecCmdCmpl.opcode,
                    pMsg->hdr.status);
            break;
          }

        #endif
      }
      break;

    default:
      /* WSF / Cordio events this application does not need; safely ignored. */
      break;
  }

  if (uiEvent != APP_UI_NONE)
  {
    /* Perform the UI state transition centrally, instead of repeating the call in every DM event branch. */
    AppUiAction(uiEvent);
  }
}

/*************************************************************************************************/
/*!
 *  \brief  The startup steps, in the order they actually run.
 *
 *  Unlike the nRF5 SDK, where main() has a single linear startup path, Cordio is
 *  driven by the WSF scheduler and the application is only ever entered through
 *  callbacks. The startup work is therefore split across two entry points, and
 *  each step below names its nRF5 counterpart.
 *
 *
 *    TmpsHandlerInit()   steps 1 to 5. Called while exactle_stack_init()
 *                        registers the WSF handler. Only then is the handler ID
 *                        available, and the profiles' measurement timers need it.
 *
 *    TmpsStart()         steps 6 to 10. Called once the protocol stack is up. The
 *                        GATT database must exist before DmDevReset(), while the
 *                        advertising data can only be loaded after the database
 *                        hash completes. The last step therefore travels through
 *                        DM_RESET_CMPL_IND and ATTS_DB_HASH_CALC_CMPL_IND before
 *                        reaching tmpsSetup().
 *
 *  \return None.
 */
/*************************************************************************************************/
static void tmpsTimersInit(wsfHandlerId_t handlerId)
{
  /* Status LED blink timer; the measurement timers are managed inside the profiles */
  tmpsLedTimer.handlerId = handlerId;
  tmpsLedTimer.msg.event = TMPS_LED_TIMER_IND;
}

static void tmpsGapCfgInit(void)
{
  pAppAdvCfg = (appAdvCfg_t *) &tmpsAdvCfg;
  pAppSlaveCfg = (appSlaveCfg_t *) &tmpsSlaveCfg;
  pAppUpdateCfg = (appUpdateCfg_t *) &tmpsUpdateCfg;
}

static void tmpsSecCfgInit(void)
{
  pAppSecCfg = (appSecCfg_t *) &tmpsSecCfg;
  pSmpCfg = (smpCfg_t *) &tmpsSmpCfg;
#ifdef AM_BLE_EATT
  pEattCfg = (eattCfg_t *) &fiteattCfg;
#endif
}

static void tmpsProfilesInit(wsfHandlerId_t handlerId)
{
  /* Health thermometer profile sensor and the flags carried by the measurement data */
  HtpsInit(handlerId, (htpsCfg_t *) &tmpsHtpsCfg);
  HtpsSetTmFlags(TMPS_TM_FLAGS);

  /* Battery Service server */
  BasInit(handlerId, (basCfg_t *) &tmpsBasCfg);
}

static void tmpsHwInit(void)
{
  /* BLE status LED: LED3 / GP19 */
  BleLedInit();

  /*
   * The TMP119 is deliberately NOT initialized from the BLE side in this project.
   *
   * SensorTask is the sole owner of the TMP119 (see sensor_task.c) and handles
   * initialization, periodic sampling and retries; the BLE side always reads the
   * cache through temp_cache_get().
   *
   * Calling TMP119_init() here as well would race with SensorTask: both could see
   * g_bIomInitDone == false and call am_hal_iom_initialize(), the later one would
   * fail because the module is already taken, and it would clear an IOM handle
   * that was already valid -- the guard flags in TMP119_init() are not
   * thread-safe and cannot prevent this.
   */
}

static void tmpsCbackRegister(void)
{
  /* Protocol stack callbacks */
  DmRegister(tmpsDmCback);
  DmConnRegister(DM_CLIENT_ID_APP, tmpsDmCback);
  AttRegister(tmpsAttCback);
  AttConnRegister(AppServerConnCback);
  AttsCccRegister(TMPS_NUM_CCC_IDX, (attsCccSet_t *) tmpsCccSet, tmpsCccCback);

  /* Application framework callbacks */
  AppUiBtnRegister(tmpsBtnCback);
}

static void tmpsServicesInit(void)
{
  uint8_t tempType = CH_TT_ARMPIT;  /* Defined in Cordio services/svc_ch.h, value 1, meaning the armpit */
  static const uint8_t tmpsDeviceName[] = TMPS_DEVICE_NAME;

  SvcCoreGattCbackRegister(GattReadCback, GattWriteCback);
  SvcCoreAddGroup();

  /* svc_core's built-in default GAP Device Name is "Packetcraft" (CORE_DEFAULT_DEV_NAME);
   * it is overwritten here to match the scan response Local Name, so a phone shows the same
   * name while scanning and once connected. The attribute carries ATTS_SET_VARIABLE_LEN, so
   * AttsSetAttr() updates the length too; it must be called after SvcCoreAddGroup(), and the
   * limit is ATT_DEFAULT_PAYLOAD_LEN (20 bytes). */
  AttsSetAttr(GAP_DN_HDL, sizeof(tmpsDeviceName) - 1, (uint8_t *) tmpsDeviceName);

  SvcHtsAddGroup();

  /* svc_hts defaults the standalone Temperature Type characteristic to CH_TT_BODY;
   * here it is kept consistent with the type carried inside the measurement data */
  AttsSetAttr(HTS_TT_HDL, sizeof(tempType), &tempType);

  SvcDisAddGroup();

  SvcBattCbackRegister(BasReadCback, NULL);
  SvcBattAddGroup();

  /* Service Changed CCCD index */
  GattSetSvcChangedIdx(TMPS_GATT_SC_CCC_IDX);
}

/*************************************************************************************************/
/*!
 *  \brief  Application handler initialization, called during system initialization.
 *
 *  \param  handlerID  WSF handler ID.
 *
 *  \return None.
 */
/*************************************************************************************************/
void TmpsHandlerInit(wsfHandlerId_t handlerId)
{
  APP_TRACE_INFO0("TmpsHandlerInit");

  /* Store the handler ID */
  tmpsHandlerId = handlerId;

  /* 1. Application timers                 (nRF: timers_init) */
  tmpsTimersInit(handlerId);

  /* 2. GAP, advertising and connection parameters
   *                                      (nRF: gap_params_init + conn_params_init) */
  tmpsGapCfgInit();

  /* 3. Pairing, bonding and encryption    (nRF: peer_manager_init) */
  tmpsSecCfgInit();

  /* 4. Cordio application framework       (no nRF counterpart) */
  AppSlaveInit();
  AppServerInit();

  /* 5. Profile servers                    (nRF: the profile part of services_init) */
  tmpsProfilesInit(handlerId);
}

/*************************************************************************************************/
/*!
 *  \brief  The application's WSF event handler.
 *
 *  \param  event   WSF event mask. Each bit is one event set by WsfSetEvent(), so
 *                  several may be present at once and it should be tested with a
 *                  bitwise AND; this handler only processes pMsg, so event is
 *                  unused but kept to match the WSF handler interface.
 *  \param  pMsg    WSF message, delivered by WsfMsgSend() or a timer; pMsg->event is
 *                  the event number of a single message, unlike the mask, which can carry several.
 *
 *  \return None.
 */
/*************************************************************************************************/
void TmpsHandler(wsfEventMask_t event, wsfMsgHdr_t *pMsg)
{
  if (pMsg != NULL)
  {
    APP_TRACE_INFO1("Fit got evt %d", pMsg->event);

    /* Handle ATT messages */
    if (pMsg->event >= ATT_CBACK_START && pMsg->event <= ATT_CBACK_END)
    {
      /* Handle the server-related ATT messages */
      AppServerProcAttMsg(pMsg);
    }
    /* Handle DM messages */
    else if (pMsg->event >= DM_CBACK_START && pMsg->event <= DM_CBACK_END)
    {
      /* Handle advertising and connection messages */
      AppSlaveProcDmMsg((dmEvt_t *) pMsg);

      /* Handle security messages */
      AppSlaveSecProcDmMsg((dmEvt_t *) pMsg);
    }

    /* Perform the profile and user interface operations */
    tmpsProcMsg((tmpsMsg_t *) pMsg);
  }
}

/*************************************************************************************************/
/*!
 *  \brief  Start the application.
 *
 *  \return None.
 */
/*************************************************************************************************/
void TmpsStart(void)
{
  /* 6. Board hardware                     (nRF: buttons_leds_init + sensor init) */
  tmpsHwInit();

  /* 7. Enhanced ATT bearer                (Cordio only) */
#ifdef AM_BLE_EATT
  EattInit(EATT_ROLE_INITIATOR | EATT_ROLE_ACCEPTOR);
#endif

  /* 8. Protocol stack and UI callbacks    (nRF: the NRF_SDH_*_OBSERVER macros) */
  tmpsCbackRegister();

  /* 9. GATT database                      (nRF: the service part of services_init) */
  tmpsServicesInit();

  /*
   * 10. Reset the controller. DM_RESET_CMPL_IND starts the ECC key and ATT
   *     database hash procedures; the ATTS_DB_HASH_CALC_CMPL_IND that follows
   *     enters tmpsSetup(), which loads the advertising data and calls AppAdvStart()
   *     (nRF: advertising_init + advertising_start).
   */
  DmDevReset();
}
