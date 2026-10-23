//*****************************************************************************
//
//! @file sensor_task.h
//!
//! @brief Functions and variables related to the sensor task.
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

#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

//*****************************************************************************
//
// This header is self-contained.
//
// The temperature cache serves both the render path and BLE (app_hw_tmp119.c),
// crossing module boundaries, so it pulls in the FreeRTOS types and
// configTICK_RATE_HZ it needs here; callers do not have to include the
// project's common headers first.
//
//*****************************************************************************
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

//*****************************************************************************
//
// sensor task handle.
//
//*****************************************************************************
extern TaskHandle_t SensorTaskHandle;

//*****************************************************************************
//
// Temperature sampling period.
//
// The TMP119 is configured for a 1 s conversion period (TMP119_CFG_CONV_1S).
// Sampling faster than that only re-reads the same conversion, so the two are
// kept in step.
//
//*****************************************************************************
#define TEMP_SAMPLE_PERIOD_TICKS    (configTICK_RATE_HZ)

//*****************************************************************************
//
// Maximum age at which a cached value is still considered valid.
//
// Three times the sampling period: this tolerates the occasional single failed
// I2C read without making the display flicker, while sustained failures beyond
// that window must be reported to the caller as stale data.
//
//*****************************************************************************
#define TEMP_CACHE_MAX_AGE_TICKS    (3 * TEMP_SAMPLE_PERIOD_TICKS)

//*****************************************************************************
//
// External function definitions.
//
//*****************************************************************************

//*****************************************************************************
//
//! @brief Get the most recent successfully read temperature (Celsius x100).
//!
//! @details Reads only the cache maintained by SensorTask and never touches
//! I2C, so it may be called from any task (including the BLE WSF context)
//! without blocking and without contending with SensorTask over IOM1.
//!
//! @param pi32TempC100 Temperature output pointer; set to TMP119_TEMP_INVALID
//!                     on failure (unless the pointer itself is NULL).
//! @param ui32MaxAgeTicks Maximum acceptable data age in ticks. The cache is
//!                     treated as stale once this much time has elapsed since
//!                     the last successful update. Pass portMAX_DELAY to skip
//!                     the age check entirely.
//!
//! @return 0 on success (pass); 1 on failure (fail), covering an invalid
//!         pointer, no valid reading yet, or a stale cache.
//
//*****************************************************************************
extern uint32_t temp_cache_get(int32_t *pi32TempC100, uint32_t ui32MaxAgeTicks);

//*****************************************************************************
//
//! @brief Sensor acquisition task.
//
//*****************************************************************************
extern void SensorTask(void *pvParameters);

#endif // SENSOR_TASK_H
