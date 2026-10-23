/*************************************************************************************************/
/*!
 *  \file
 *
 *  \brief  Application framework hardware interfaces.
 *
 *  Copyright (c) 2011-2018 Arm Ltd. All Rights Reserved.
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

/* Ingore the "initializer element is not a constant expression" warning
 * with -Wpedantic option enabled.
 */
#if defined ( __GNUC__ )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include <string.h>
#include "wsf_types.h"
#include "wsf_os.h"
#include "wsf_trace.h"
#include "util/bstream.h"
#include "app_hw.h"
#include "tmp119.h"
#include "sensor_task.h"
#include "svc_ch.h"

/**************************************************************************************************
  Macros
**************************************************************************************************/

/*! Simulated measurement RR interval, converted from bpm to 1/1024 sec units */
#define APP_HR_MEAS_SIM_RR(heartRate)     (((uint16_t) 1024 * 60) / (heartRate))

/*! Number of simulated blood pressure measurements */
#define APP_NUM_BPM         3       /* measurements */
#define APP_NUM_INT_BPM     3       /* intermediate measurements */

/*! Number of simulated weight scale measurements */
#define APP_NUM_WSM         3

/*! Number of simulated temperature measurements */
#define APP_NUM_TM          6

/**************************************************************************************************
  Data Types
**************************************************************************************************/

/*! Blood pressure simulated measurement structure */
typedef struct
{
  uint16_t          systolic;               /*! Systolic pressure */
  uint16_t          diastolic;              /*! Diastolic pressure */
  uint16_t          map;                    /*! Mean arterial pressure */
  uint16_t          pulseRate;              /*! Pulse rate */
  uint16_t          measStatus;             /*! Measurement status */
} appHwSimBpm_t;

/**************************************************************************************************
  Local Variables
**************************************************************************************************/

/* battery level */
static uint8_t appHwBattLevel = 100;

/* heart rate */
static uint8_t appHwHeartRate = 78;

/* rr intervals */
static uint16_t appHwRrInterval[2];

/* simluated blood pressure measurements */
static const appHwSimBpm_t appHwBpm[APP_NUM_BPM] =
{
  {
    SFLT_TO_UINT16(140, 0),                 /*! Systolic pressure */
    SFLT_TO_UINT16(80, 0),                  /*! Diastolic pressure */
    SFLT_TO_UINT16(100, 0),                 /*! Mean arterial pressure */
    SFLT_TO_UINT16(79, 0),                  /*! Pulse rate */
    0                                       /*! Measurement status */
  },
  {
    SFLT_TO_UINT16(141, 0),                 /*! Systolic pressure */
    SFLT_TO_UINT16(81, 0),                  /*! Diastolic pressure */
    SFLT_TO_UINT16(101, 0),                 /*! Mean arterial pressure */
    SFLT_TO_UINT16(80, 0),                  /*! Pulse rate */
    CH_BPM_MS_FLAG_MOVEMENT                 /*! Measurement status */
  },
  {
    SFLT_TO_UINT16(142, 0),                 /*! Systolic pressure */
    SFLT_TO_UINT16(82, 0),                  /*! Diastolic pressure */
    SFLT_TO_UINT16(102, 0),                 /*! Mean arterial pressure */
    SFLT_TO_UINT16(81, 0),                  /*! Pulse rate */
    CH_BPM_MS_FLAG_CUFF_FIT_LOOSE           /*! Measurement status */
  }
};

/* simluated intermediate blood pressure measurements */
static const appHwSimBpm_t appHwIntBpm[APP_NUM_INT_BPM] =
{
  {
    SFLT_TO_UINT16(103, 0),                 /*! Current cuff pressure */
    CH_SFLOAT_NAN,                          /*! Unused */
    CH_SFLOAT_NAN,                          /*! Unused */
    SFLT_TO_UINT16(76, 0),                  /*! Pulse rate */
    CH_BPM_MS_FLAG_IRR_PULSE                /*! Measurement status */
  },
  {
    SFLT_TO_UINT16(104, 0),                 /*! Current cuff pressure */
    CH_SFLOAT_NAN,                          /*! Unused */
    CH_SFLOAT_NAN,                          /*! Unused */
    SFLT_TO_UINT16(77, 0),                  /*! Pulse rate */
    CH_BPM_MS_FLAG_MEAS_POS_ERR             /*! Measurement status */
  },
  {
    SFLT_TO_UINT16(105, 0),                 /*! Current cuff pressure */
    CH_SFLOAT_NAN,                          /*! Unused */
    CH_SFLOAT_NAN,                          /*! Unused */
    SFLT_TO_UINT16(78, 0),                  /*! Pulse rate */
    CH_BPM_MS_FLAG_CUFF_FIT_LOOSE           /*! Measurement status */
  }
};

/* simulated measurement indexes */
static uint8_t appHwBpmIdx = 0;
static uint8_t appHwIntBpmIdx = 0;

/* seconds between temperature measurements; keep in step with TMPS_TM_MEAS_PERIOD_MS
 * (hts_main.c, currently 1000 ms). This is the step of the software clock: the
 * board has no RTC, so timestamps accumulate on each AppHwTmRead() call and
 * the two must therefore agree. */
#define APP_HW_TM_PERIOD_SEC    1

/* timestamp */
static appDateTime_t appHwDateTime =
{
  2014,       /*! Year */
  1,          /*! Month */
  27,         /*! Day */
  13,         /*! Hour */
  10,         /*! Minutes */
  0           /*! Seconds */
};

/* simulated weight scale measurements in lbs */
static const uint16_t appHwWeightLbs[APP_NUM_WSM] =
{
  15010,
  15120,
  15230
};

/* simulated weight scale measurements in kg */
static const uint16_t appHwWeightKg[APP_NUM_WSM] =
{
  6823,
  6873,
  6923
};

/* simulated measurement index */
static uint8_t appHwWsmIdx = 0;

#if defined(keil6)
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wshift-negative-value"
#endif

/* simulated temperature measurements in C */
static const uint32_t appHwTempC[APP_NUM_TM] =
{
  FLT_TO_UINT32(369, -1),
  FLT_TO_UINT32(370, -1),
  FLT_TO_UINT32(371, -1),
  FLT_TO_UINT32(372, -1),
  FLT_TO_UINT32(373, -1),
  FLT_TO_UINT32(374, -1)
};

/* simulated temperature measurements in F */
static const uint32_t appHwTempF[APP_NUM_TM] =
{
  FLT_TO_UINT32(985, -1),
  FLT_TO_UINT32(986, -1),
  FLT_TO_UINT32(987, -1),
  FLT_TO_UINT32(988, -1),
  FLT_TO_UINT32(989, -1),
  FLT_TO_UINT32(990, -1)
};

#if defined(keil6)
  #pragma clang diagnostic pop
#endif

/* simulated measurement index */
static uint8_t appHwTmIdx = 0;

/* temperature units */
static int32_t appHwLastTempC100 = 0;
static uint8_t appHwTmUnits = CH_TM_FLAG_UNITS_C;

/* weight units */
static uint8_t appHwWmUnits = CH_WSM_FLAG_UNITS_LBS;

/* pulse oximeter continuous measurement */
static const appPlxCm_t appHwPlxCm =
{
  0,                                /*! Flags */
  SFLT_TO_UINT16(98, 0),            /*! SpO2PR-Spot-Check - SpO2 */
  SFLT_TO_UINT16(60, 0),            /*! SpO2PR-Spot-Check - Pulse Rate */
  SFLT_TO_UINT16(98, 0),            /*! SpO2PR-Spot-Check Fast - SpO2 */
  SFLT_TO_UINT16(60, 0),            /*! SpO2PR-Spot-Check Fast - Pulse Rate */
  SFLT_TO_UINT16(98, 0),            /*! SpO2PR-Spot-Check Slow - SpO2 */
  SFLT_TO_UINT16(60, 0),            /*! SpO2PR-Spot-Check Slow - Pulse Rate */
  0,                                /*! Measurement Status */
  0,                                /*! Device and Sensor Status */
  SFLT_TO_UINT16(60, 0)             /*! Pulse Amplitude Index */
};

/* pulse oximeter spot check measurement */
static const appPlxScm_t appHwPlxScm =
{
  0,                                /*! Flags */
  SFLT_TO_UINT16(98, 0),            /*! SpO2PR-Spot-Check - SpO2 */
  SFLT_TO_UINT16(60, 0),            /*! SpO2PR-Spot-Check - Pulse Rate */
  {
    2016,                           /*! Year */
    7,                              /*! Month */
    18,                             /*! Day */
    13,                             /*! Hour */
    10,                             /*! Minutes */
    0                               /*! Seconds */
  },
  0,                                /*! Measurement Status */
  0,                                /*! Device and Sensor Status */
  SFLT_TO_UINT16(60, 0)             /*! Pulse Amplitude Index */
};

/*************************************************************************************************/
/*!
 *  \brief  Read the battery level.  The battery level value returned in pLevel is the
 *          percentage of remaining battery capacity (0-100%).
 *
 *  \param  pLevel   Battery level return value.
 *
 *  \return None.
 */
/*************************************************************************************************/
void AppHwBattRead(uint8_t *pLevel)
{
  // Change battery level to trigger battery notification simulation for test purposes
  if(appHwBattLevel == 0)
  {
    appHwBattLevel = 100;
  }
  else
  {
    appHwBattLevel--;
  }
 *pLevel = appHwBattLevel;
}

/*************************************************************************************************/
/*!
 *  \brief  Set the battery level, for test purposes.
 *
 *  \param  level   Battery level (0-100%).
 *
 *  \return None.
 */
/*************************************************************************************************/
void AppHwBattTest(uint8_t level)
{
  appHwBattLevel = level;
}

/*************************************************************************************************/
/*!
 *  \brief  Perform a heart rate measurement.  Return the heart rate along with any RR interval
 *          data.
 *
 *  \param  pHrm   Heart rate measurement return value.
 *
 *  \return None.
 */
/*************************************************************************************************/
void AppHwHrmRead(appHrm_t *pHrm)
{
  pHrm->heartRate = appHwHeartRate;

  /* calculate simulated RR intervals from heart rate */
  appHwRrInterval[0] = APP_HR_MEAS_SIM_RR(appHwHeartRate);
  appHwRrInterval[1] = APP_HR_MEAS_SIM_RR(appHwHeartRate);

  pHrm->pRrInterval = appHwRrInterval;
  pHrm->numIntervals = 2;
}

/*************************************************************************************************/
/*!
 *  \brief  Set the heart rate, for test purposes.
 *
 *  \param  heartRate Heart rate.
 *
 *  \return None.
 */
/*************************************************************************************************/
void AppHwHrmTest(uint8_t heartRate)
{
  appHwHeartRate = heartRate;
}

/*************************************************************************************************/
/*!
 *  \brief  Perform a blood pressure measurement.  Return the measurement data.
 *
 *  \param  intermed  TRUE if this is an intermediate measurement.
 *  \param  pHrm      Blood pressure measurement return value.
 *
 *  \return None.
 */
/*************************************************************************************************/
void AppHwBpmRead(bool_t intermed, appBpm_t *pBpm)
{
  appHwSimBpm_t *pMeas;

  /* set pointer to simulated measurement data */
  if (intermed == TRUE)
  {
    pMeas = (appHwSimBpm_t *) &appHwIntBpm[appHwIntBpmIdx];

    /* increment index to set up for next measurement */
    if (++appHwIntBpmIdx == APP_NUM_INT_BPM)
    {
      appHwIntBpmIdx = 0;
    }
  }
  else
  {
    pMeas = (appHwSimBpm_t *) &appHwBpm[appHwBpmIdx];

    /* increment index to set up for next measurement */
    if (++appHwBpmIdx == APP_NUM_BPM)
    {
      appHwBpmIdx = 0;
    }
  }

  /* set measurement values */
  pBpm->diastolic = pMeas->diastolic;
  pBpm->systolic = pMeas->systolic;
  pBpm->map = pMeas->map;
  pBpm->pulseRate = pMeas->pulseRate;
  pBpm->measStatus = pMeas->measStatus;

  /* set the timestamp */
  pBpm->timestamp = appHwDateTime;

  /* increment seconds field to simulate time */
  appHwDateTime.sec += 2;
  if (appHwDateTime.sec > 59)
  {
    appHwDateTime.sec = 0;
  }
}

/*************************************************************************************************/
/*!
 *  \brief  Perform a weight scale measurement.  Return the measurement data.
 *
 *  \param  pWsm      Weight scale measurement return value.
 *
 *  \return None.
 */
/*************************************************************************************************/
void AppHwWsmRead(appWsm_t *pWsm)
{

  /* set measurement values */
  if (appHwWmUnits == CH_WSM_FLAG_UNITS_LBS)
  {
    pWsm->weight = appHwWeightLbs[appHwWsmIdx];
  }
  else
  {
    pWsm->weight = appHwWeightKg[appHwWsmIdx];
  }

  /* set the timestamp */
  pWsm->timestamp = appHwDateTime;

  /* increment minutes field to simulate time */
  appHwDateTime.min++;
  if (appHwDateTime.min > 59)
  {
    appHwDateTime.min = 0;
  }

  /* increment index to set up for next simulated measurement */
  if (++appHwWsmIdx == APP_NUM_BPM)
  {
    appHwWsmIdx = 0;
  }
}

/*************************************************************************************************/
/*!
 *  \brief  Read the TMP119 temperature into the measurement structure used by Cordio HTS.
 *  \details TMP119_read_temp_c100() returns 0 on success and 1 on failure; on failure the last good reading is reused.
 *  \param  intermed  TRUE for an intermediate measurement; unused by this implementation.
 *  \param  pTm       Output measurement structure: temperature, measurement site and timestamp.
 *
 *  \return None; this function returns void.
 */
/*************************************************************************************************/
void AppHwTmRead(bool_t intermed, appTm_t *pTm)
{
  int32_t i32TempC100 = TMP119_TEMP_INVALID;
  int32_t i32Value;

  /*
   * Reads only the cache maintained by SensorTask; no I2C access happens here.
   *
   * Cordio's HTPS profile calls this from the RadioTask/WSF context. Reading
   * the TMP119 directly would contend with SensorTask over IOM1, and the
   * am_hal_iom handle is not thread-safe. Going through the cache keeps
   * RadioTask off the bus entirely: no blocking, no contention.
   *
   * Time scale: HTS indications and SensorTask sampling both run on a 1 s
   * period but their phases are independent, so the value read here is 0 to 1 s
   * old. The TMP119 itself only produces a new conversion once per second, so
   * reading the cache does not make the data any staler.
   *
   * A stale cache (consecutive read failures beyond TEMP_CACHE_MAX_AGE_TICKS =
   * 3 s) returns non-zero; the last good reading is then reused, which matches
   * the original behaviour on a failed read.
   */
  if ((temp_cache_get(&i32TempC100, TEMP_CACHE_MAX_AGE_TICKS) == 0) &&
      (i32TempC100 != TMP119_TEMP_INVALID))
  {
    appHwLastTempC100 = i32TempC100;
  }

  if (appHwTmUnits == CH_TM_FLAG_UNITS_C)
  {
    i32Value = appHwLastTempC100;
  }
  else
  {
    /* Convert hundredths of a degree Celsius (C x100) to hundredths of a degree Fahrenheit (F x100) */
    i32Value = (appHwLastTempC100 * 9) / 5 + 3200;
  }

  /*
   * IEEE-11073 32-bit FLOAT: the low 24 bits are a signed mantissa, the high 8
   * bits a signed exponent. Temperatures are in hundredths of a degree, so the
   * exponent is set to -2.
   *
   * FLT_TO_UINT32() merges the two fields with OR. For a negative mantissa sign
   * extension fills the top byte and corrupts the exponent, so the mantissa is
   * masked to 24 bits before being combined with the exponent.
   */
  pTm->temperature = ((uint32_t) i32Value & 0x00FFFFFFU) |
                     ((uint32_t) ((uint8_t) -2) << 24);

  /* Set the measurement site; same as the nRF52832 version, the armpit (CH_TT_ARMPIT) */
  pTm->tempType = CH_TT_ARMPIT;

  /* Set the timestamp */
  pTm->timestamp = appHwDateTime;

  /*
   * This board has no RTC, so the timestamp uses a software clock advanced by
   * one measurement period after each read. Seconds past 59 must carry into
   * minutes and hours, or the reported time would stay inside the same minute.
   */
  appHwDateTime.sec += APP_HW_TM_PERIOD_SEC;
  while (appHwDateTime.sec > 59)
  {
    appHwDateTime.sec -= 60;

    if (++appHwDateTime.min > 59)
    {
      appHwDateTime.min = 0;

      if (++appHwDateTime.hour > 23)
      {
        appHwDateTime.hour = 0;
      }
    }
  }
}

/*************************************************************************************************/
/*!
 *  \brief  Set the temperature measurement units.
 *
 *  \param  units     CH_TM_FLAG_UNITS_C or CH_TM_FLAG_UNITS_F.
 *
 *  \return None.
 */
/*************************************************************************************************/
void AppHwTmSetUnits(uint8_t units)
{
  appHwTmUnits = units;
}


/*************************************************************************************************/
/*!
 *  \brief  Set the weight measurement units.
 *
 *  \param  units     CH_WSM_FLAG_UNITS_KG or CH_WSM_FLAG_UNITS_LBS.
 *
 *  \return None.
 */
/*************************************************************************************************/
void AppHwWmSetUnits(uint8_t units)
{
  appHwWmUnits = units;
}

/*************************************************************************************************/
/*!
 *  \brief  Perform a pulse oximeter measurement.
 *
 *  \param  pPlxcm   Pulse Oximeter measurement return value.
 *
 *  \return None.
 */
/*************************************************************************************************/
void AppHwPlxcmRead(appPlxCm_t *pPlxcm)
{
  memcpy(pPlxcm, &appHwPlxCm, sizeof(appPlxCm_t));
}

/*************************************************************************************************/
/*!
 *  \brief  Perform a pulse oximeter spot check measurement.
 *
 *  \param  pPlxcm   Pulse Oximeter spot check measurement return value.
 *
 *  \return None.
 */
/*************************************************************************************************/
void AppHwPlxscmRead(appPlxScm_t *pPlxscm)
{
  memcpy(pPlxscm, &appHwPlxScm, sizeof(appPlxScm_t));
}
