//*****************************************************************************
//
//! @file hts_api.h
//!
//! @brief Application interface for the TMP119 Health Thermometer Service.
//!
//! Adapted from the Cordio fit example (ble-profiles/sources/apps/fit).
//! The heart rate service is replaced by the Health Thermometer Service
//! (0x1809), with the temperature sourced from a TMP119 (IOM1 / GP8, GP9).
//
//*****************************************************************************

#ifndef HTS_API_H
#define HTS_API_H

#include "wsf_os.h"

#ifdef __cplusplus
extern "C" {
#endif

//
// Maximum number of simultaneous connections.
//
#ifndef TMPS_CONN_MAX
#define TMPS_CONN_MAX                 1
#endif

//
// Events delivered to the custom WSF handler (radio_task.c uses them to track
// connection state).
//
#define CUST_BLE_ADV_START_EVENT       0x01
#define CUST_BLE_CONN_OPEN_EVENT       0x02

//*****************************************************************************
//
//! @brief Start the application: register the service, set the advertising
//! data and reset the device.
//
//*****************************************************************************
void TmpsStart(void);

//*****************************************************************************
//
//! @brief Initialize the application's WSF handler.
//!
//! @param handlerId WSF handler ID
//
//*****************************************************************************
void TmpsHandlerInit(wsfHandlerId_t handlerId);

//*****************************************************************************
//
//! @brief WSF event handling entry point.
//!
//! @param event Event mask
//! @param pMsg  Message
//
//*****************************************************************************
void TmpsHandler(wsfEventMask_t event, wsfMsgHdr_t *pMsg);

#ifdef __cplusplus
};
#endif

#endif // HTS_API_H
