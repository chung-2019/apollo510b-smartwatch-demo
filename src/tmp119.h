//*****************************************************************************
//
//! @file tmp119.h
//!
//! @brief TI TMP119 high-accuracy digital temperature sensor driver
//!
//! Ported from the nRF52832 version (nrf_drv_twi) to AmbiqSuite. The interface
//! matches the original; internally it uses am_hal_iom blocking transfers.
//!
//! Wiring (AP510BEVB Rev2.0):
//!   GP8 (SCL) --R81--> TCA9416 SCLA ... SCLB --R82--> HDR_SCL --> TMP119 SCL
//!   GP9 (SDA) --R83--> TCA9416 SDAA ... SDAB --R84--> HDR_SDA --> TMP119 SDA
//!   Power VDD_MBIF and GND come from the mikroBUS header.
//!
//! GP8/GP9 are AM_BSP_GPIO_IOM1_SCL / AM_BSP_GPIO_IOM1_SDA in the BSP, so this
//! driver uses IOM1 and does not interfere with the touch panel (IOM2).
//
//*****************************************************************************

#ifndef TMP119_H
#define TMP119_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
// I2C address (selected by the ADD0 pin)
//
//*****************************************************************************
#define TMP119_ADDR                 (0x48)  // ADD0 tied to GND (default address)
#define TMP119_ADDR_VDD             (0x49)  // ADD0 tied to V+
#define TMP119_ADDR_SDA             (0x4A)  // ADD0 tied to SDA
#define TMP119_ADDR_SCL             (0x4B)  // ADD0 tied to SCL

//*****************************************************************************
//
// Register addresses
//
//*****************************************************************************
#define TMP119_REG_TEMP_RESULT      0x00    // Temperature result (R)
#define TMP119_REG_CONFIGURATION    0x01    // Configuration (R/W)
#define TMP119_REG_THIGH_LIMIT      0x02    // High limit (R/W)
#define TMP119_REG_TLOW_LIMIT       0x03    // Low limit (R/W)
#define TMP119_REG_EEPROM_UNLOCK    0x04    // EEPROM unlock (R/W)
#define TMP119_REG_EEPROM1          0x05    // EEPROM1 (R/W)
#define TMP119_REG_EEPROM2          0x06    // EEPROM2 (R/W)
#define TMP119_REG_TEMP_OFFSET      0x07    // Temperature offset (R/W)
#define TMP119_REG_EEPROM3          0x08    // EEPROM3 (R/W)
#define TMP119_REG_DEVICE_ID        0x0F    // Device ID (R)

//*****************************************************************************
//
// Device ID
//
//*****************************************************************************
#define TMP119_DEVICE_ID_VALUE      0x2117  // Full device ID
#define TMP119_DEVICE_ID_MASK       0x0FFF  // DID mask (low 12 bits)
#define TMP119_DEVICE_ID_DID        0x0117  // Expected DID

//*****************************************************************************
//
// Configuration register bit definitions
//
//*****************************************************************************
#define TMP119_CFG_HIGH_ALERT       (1 << 15)   // High alert flag (R)
#define TMP119_CFG_LOW_ALERT        (1 << 14)   // Low alert flag (R)
#define TMP119_CFG_DATA_READY       (1 << 13)   // Data ready flag (R)
#define TMP119_CFG_EEPROM_BUSY      (1 << 12)   // EEPROM busy flag (R)

// Bits 11-10: MOD[1:0] conversion mode
#define TMP119_CFG_MOD_CC           (0x00 << 10)    // Continuous conversion
#define TMP119_CFG_MOD_SD           (0x01 << 10)    // Shutdown
#define TMP119_CFG_MOD_OS           (0x03 << 10)    // One-shot

// Bits 9-7: CONV[2:0] conversion period
#define TMP119_CFG_CONV_15_5MS      (0x00 << 7)
#define TMP119_CFG_CONV_125MS       (0x01 << 7)
#define TMP119_CFG_CONV_250MS       (0x02 << 7)
#define TMP119_CFG_CONV_500MS       (0x03 << 7)
#define TMP119_CFG_CONV_1S          (0x04 << 7)     // Default
#define TMP119_CFG_CONV_4S          (0x05 << 7)
#define TMP119_CFG_CONV_8S          (0x06 << 7)
#define TMP119_CFG_CONV_16S         (0x07 << 7)

// Bits 6-5: AVG[1:0] averaging mode
#define TMP119_CFG_AVG_NONE         (0x00 << 5)
#define TMP119_CFG_AVG_8            (0x01 << 5)     // Default
#define TMP119_CFG_AVG_32           (0x02 << 5)
#define TMP119_CFG_AVG_64           (0x03 << 5)

//*****************************************************************************
//
// Constants
//
//*****************************************************************************
#define TMP119_RESOLUTION           0.0078125f      // Degrees C per LSB

//
// Sentinel returned by TMP119_read_temp_c100() when no temperature is available.
//
#define TMP119_TEMP_INVALID         (-9999)

//*****************************************************************************
//
//! @brief Initialize the TMP119
//!
//! @details Enables IOM1 (I2C at 400 kHz) and verifies the device ID.
//! If the default address 0x48 does not respond, 0x49 / 0x4A / 0x4B are tried in
//! turn, and the address found is recorded for subsequent reads.
//!
//! @return 0 on success, non-zero on failure.
//
//*****************************************************************************
extern uint32_t TMP119_init(void);

//*****************************************************************************
//
//! @brief Whether the sensor initialized successfully.
//
//*****************************************************************************
extern bool TMP119_is_ready(void);

//*****************************************************************************
//
//! @brief Read the temperature in hundredths of a degree Celsius.
//!
//! @details For example, 3650 means 36.50 degrees C.
//!
//! @param pdata Pointer that receives the result; set to TMP119_TEMP_INVALID on failure.
//!
//! @return 0 on success, non-zero when the I2C read fails.
//
//*****************************************************************************
extern uint32_t TMP119_read_temp_c100(int32_t *pdata);

//*****************************************************************************
//
//! @brief Read the raw temperature value (signed 16-bit).
//
//*****************************************************************************
extern uint32_t TMP119_read_temp_raw(int16_t *pdata);

//*****************************************************************************
//
//! @brief Read the device ID.
//
//*****************************************************************************
extern uint32_t TMP119_read_device_id(uint16_t *pdata);

//*****************************************************************************
//
//! @brief Read the configuration register.
//
//*****************************************************************************
extern uint32_t TMP119_read_config(uint16_t *pdata);

//*****************************************************************************
//
//! @brief Write the configuration register.
//
//*****************************************************************************
extern uint32_t TMP119_write_config(uint16_t data);

#ifdef __cplusplus
}
#endif

#endif // TMP119_H
