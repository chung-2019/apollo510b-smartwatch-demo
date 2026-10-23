//*****************************************************************************
//
//! @file tmp119.c
//!
//! @brief TI TMP119 high-accuracy digital temperature sensor driver (AmbiqSuite port)
//!
//! Ported from the nRF52832 nrf_drv_twi version. The IOM offset mechanism
//! already produces the "write register address, repeated start, read data"
//! sequence by itself, so the original's separate tx/rx pair is merged into a
//! single transaction here.
//
//*****************************************************************************

#include "tmp119.h"
#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

//*****************************************************************************
//
// The IOM this driver uses.
//
// GP8/GP9 are AM_BSP_GPIO_IOM1_SCL / AM_BSP_GPIO_IOM1_SDA, hence IOM1.
// The touch panel is on IOM2, so the two do not share a bus.
//
//*****************************************************************************
#define TMP119_IOM_MODULE       1
#define TMP119_I2C_FREQ         AM_HAL_IOM_400KHZ

//
// Addresses tried in order during init, one per ADD0 wiring option.
//
static const uint8_t g_ui8AddrCandidates[] =
{
    TMP119_ADDR,        // 0x48 ADD0 -> GND
    TMP119_ADDR_VDD,    // 0x49 ADD0 -> V+
    TMP119_ADDR_SDA,    // 0x4A ADD0 -> SDA
    TMP119_ADDR_SCL,    // 0x4B ADD0 -> SCL
};

static void    *g_pTmp119IomHandle = NULL;
static uint8_t  g_ui8Tmp119Addr    = TMP119_ADDR;
static bool     g_bTmp119Ready     = false;

//
// IOM init state is tracked in two parts, because their retry semantics differ completely:
//
//   g_bIomInitDone - am_hal_iom_initialize() has succeeded. The HAL records
//                    occupancy in a module-level bInit flag that is never
//                    cleared on its own (short of uninitialize), so a repeat
//                    call always returns AM_HAL_STATUS_INVALID_OPERATION. This
//                    step can only succeed once, hence its own latching flag.
//
//   g_bIomReady    - power_ctrl / configure / enable have all completed and the
//                    bus can transfer. If any of the three fails the HAL does
//                    not set bEnable, so the next attempt can repeat them
//                    verbatim (am_hal_iom_configure() only refuses while
//                    bEnable is true).
//
static bool     g_bIomInitDone     = false;
static bool     g_bIomReady        = false;

//
// Whether the address scan failure message has already been printed.
// TMP119_init() may be retried periodically; this flag limits the failure
// message to one line per state change.
//
static bool     g_bScanFailLogged  = false;

//*****************************************************************************
//
//! @brief Read one 16-bit register from the TMP119.
//!
//! @details The IOM sends the register address as an offset (instruction), issues
//! an automatic repeated start and reads back 2 bytes. The TMP119 is MSB first.
//!
//! @param ui8Addr Register address
//! @param pui16Data Receives the result
//!
//! @return 0 on success
//
//*****************************************************************************
static uint32_t
tmp119_read_word(uint8_t ui8Addr, uint16_t *pui16Data)
{
    am_hal_iom_transfer_t Transaction;

    //
    // The IOM RX buffer must be 32-bit aligned, hence the uint32_t.
    //
    uint32_t ui32RxBuffer = 0;

    if ( (g_pTmp119IomHandle == NULL) || (pui16Data == NULL) )
    {
        return 1;
    }

    Transaction.ui8Priority          = 1;
    Transaction.ui32InstrLen         = 1;
    Transaction.ui64Instr            = ui8Addr;
    Transaction.eDirection           = AM_HAL_IOM_RX;
    Transaction.ui32NumBytes         = 2;
    Transaction.pui32RxBuffer        = &ui32RxBuffer;
    Transaction.pui32TxBuffer        = NULL;
    Transaction.bContinue            = false;
    Transaction.ui8RepeatCount       = 0;
    Transaction.ui32PauseCondition   = 0;
    Transaction.ui32StatusSetClr     = 0;
    Transaction.uPeerInfo.ui32I2CDevAddr = g_ui8Tmp119Addr;

    if ( am_hal_iom_blocking_transfer(g_pTmp119IomHandle, &Transaction) != AM_HAL_STATUS_SUCCESS )
    {
        return 1;
    }

    //
    // The byte received first is the MSB.
    //
    const uint8_t *pui8Rx = (const uint8_t *)&ui32RxBuffer;
    *pui16Data = ((uint16_t)pui8Rx[0] << 8) | pui8Rx[1];

    return 0;
}

//*****************************************************************************
//
//! @brief Write one 16-bit register.
//!
//! @param ui8Addr Register address
//! @param ui16Data Value to write
//!
//! @return 0 on success
//
//*****************************************************************************
static uint32_t
tmp119_write_word(uint8_t ui8Addr, uint16_t ui16Data)
{
    am_hal_iom_transfer_t Transaction;

    //
    // Data goes out MSB first.
    //
    uint32_t ui32TxBuffer = 0;
    uint8_t *pui8Tx = (uint8_t *)&ui32TxBuffer;

    if ( g_pTmp119IomHandle == NULL )
    {
        return 1;
    }

    pui8Tx[0] = (uint8_t)(ui16Data >> 8);
    pui8Tx[1] = (uint8_t)(ui16Data & 0xFF);

    Transaction.ui8Priority          = 1;
    Transaction.ui32InstrLen         = 1;
    Transaction.ui64Instr            = ui8Addr;
    Transaction.eDirection           = AM_HAL_IOM_TX;
    Transaction.ui32NumBytes         = 2;
    Transaction.pui32TxBuffer        = &ui32TxBuffer;
    Transaction.pui32RxBuffer        = NULL;
    Transaction.bContinue            = false;
    Transaction.ui8RepeatCount       = 0;
    Transaction.ui32PauseCondition   = 0;
    Transaction.ui32StatusSetClr     = 0;
    Transaction.uPeerInfo.ui32I2CDevAddr = g_ui8Tmp119Addr;

    if ( am_hal_iom_blocking_transfer(g_pTmp119IomHandle, &Transaction) != AM_HAL_STATUS_SUCCESS )
    {
        return 1;
    }

    return 0;
}

//*****************************************************************************
//
//! @brief Initialize the TMP119.
//
//*****************************************************************************
uint32_t
TMP119_init(void)
{
    am_hal_iom_config_t sIomCfg;

    if ( g_bTmp119Ready )
    {
        return 0;
    }

    //
    // Configuration is only needed while the bus is not ready. The address scan
    // fails when the sensor is absent and the caller may retry this function
    // periodically; the IOM is long since ready by then and must not be reconfigured.
    //
    // Note that "the IOM works" and "a TMP119 answers" are two different things:
    // the former is MCU-side bus state, the latter depends on a device replying
    // on the wire. A failed scan must never tear down a bus that already works.
    //
    if ( !g_bIomReady )
    {
        //
        // This step can only succeed once; see the note on g_bIomInitDone.
        //
        if ( !g_bIomInitDone )
        {
            //
            // Hardware connections of the TMP119 temperature sensor:
            //   I2C: IOM1 (SCL=GPIO8, SDA=GPIO9)
            //   Power: VDD_MBIF, GND (from the mikroBUS header)
            //   ALERT: unused by this driver, no GPIO interrupt configured.
            // GPIO8/GPIO9 are set to IOM1 I2C pins first (open-drain with the
            // built-in 1.5k pull-ups), then the IOM is enabled -- the same order
            // as am_devices_chsc5816_init().
            //
            am_bsp_iom_pins_enable(TMP119_IOM_MODULE, AM_HAL_IOM_I2C_MODE);

            if ( am_hal_iom_initialize(TMP119_IOM_MODULE, &g_pTmp119IomHandle) )
            {
                am_util_debug_printf("[tmp119] IOM%d initialize failed\n",
                                     TMP119_IOM_MODULE);
                g_pTmp119IomHandle = NULL;
                return 1;
            }

            g_bIomInitDone = true;
        }

        //
        // Configure IOM1 for I2C. Everything uses blocking transfers, so no
        // non-blocking DMA transfer control buffer is needed.
        //
        sIomCfg.eInterfaceMode      = AM_HAL_IOM_I2C_MODE;
        sIomCfg.ui32ClockFreq       = TMP119_I2C_FREQ;
        sIomCfg.eSpiMode            = AM_HAL_IOM_SPI_MODE_0;
        sIomCfg.pNBTxnBuf           = NULL;
        sIomCfg.ui32NBTxnBufLength  = 0;

        //
        // If any of the three steps fails the HAL does not set bEnable, so the
        // next attempt can repeat them unchanged. Keep the handle and
        // g_bIomInitDone -- clearing the handle to NULL would make the next retry
        // believe initialize() must run again, and that really is unrecoverable.
        //
        if ( am_hal_iom_power_ctrl(g_pTmp119IomHandle, AM_HAL_SYSCTRL_WAKE, false) ||
             am_hal_iom_configure(g_pTmp119IomHandle, &sIomCfg) ||
             am_hal_iom_enable(g_pTmp119IomHandle) )
        {
            am_util_debug_printf("[tmp119] IOM%d config failed, will retry\n",
                                 TMP119_IOM_MODULE);
            return 1;
        }

        g_bIomReady = true;

        //
        // The TMP119 needs about 1.5 ms after power-up before it responds.
        //
        am_util_delay_ms(5);
    }

    //
    // Scan the four possible addresses and confirm the sensor by device ID.
    //
    for ( uint32_t ui32Idx = 0; ui32Idx < sizeof(g_ui8AddrCandidates); ui32Idx++ )
    {
        uint16_t ui16DeviceId = 0;

        g_ui8Tmp119Addr = g_ui8AddrCandidates[ui32Idx];

        if ( tmp119_read_word(TMP119_REG_DEVICE_ID, &ui16DeviceId) == 0 )
        {
            if ( (ui16DeviceId & TMP119_DEVICE_ID_MASK) == TMP119_DEVICE_ID_DID )
            {
                g_bTmp119Ready = true;
                g_bScanFailLogged = false;
                am_util_debug_printf("[tmp119] found at 0x%02X, ID 0x%04X\n",
                                     g_ui8Tmp119Addr, ui16DeviceId);
                break;
            }

            if ( !g_bScanFailLogged )
            {
                am_util_debug_printf("[tmp119] 0x%02X responded, ID 0x%04X (not TMP119)\n",
                                     g_ui8Tmp119Addr, ui16DeviceId);
            }
        }
    }

    if ( !g_bTmp119Ready )
    {
        //
        // The caller retries periodically, so the scan failure message is printed
        // only on a state change, instead of flooding RTT every sampling period.
        //
        if ( !g_bScanFailLogged )
        {
            am_util_debug_printf("[tmp119] not found on IOM%d (GP8/GP9)\n", TMP119_IOM_MODULE);
            g_bScanFailLogged = true;
        }

        return 1;
    }

    //
    // Continuous conversion, 1 s period, 8-sample average. One screen update per second is enough.
    //
    if ( TMP119_write_config(TMP119_CFG_MOD_CC |
                             TMP119_CFG_CONV_1S |
                             TMP119_CFG_AVG_8) != 0 )
    {
        am_util_debug_printf("[tmp119] config write failed\n");
    }

    return 0;
}

//*****************************************************************************
//
//! @brief Whether the sensor initialized successfully.
//
//*****************************************************************************
bool
TMP119_is_ready(void)
{
    return g_bTmp119Ready;
}

//*****************************************************************************
//
//! @brief Read the device ID.
//
//*****************************************************************************
uint32_t
TMP119_read_device_id(uint16_t *pdata)
{
    return tmp119_read_word(TMP119_REG_DEVICE_ID, pdata);
}

//*****************************************************************************
//
//! @brief Read the raw temperature value.
//
//*****************************************************************************
uint32_t
TMP119_read_temp_raw(int16_t *pdata)
{
    uint16_t ui16Raw = 0;
    uint32_t ui32Status;

    if ( pdata == NULL )
    {
        return 1;
    }

    ui32Status = tmp119_read_word(TMP119_REG_TEMP_RESULT, &ui16Raw);
    if ( ui32Status == 0 )
    {
        *pdata = (int16_t)ui16Raw;
    }

    return ui32Status;
}

//*****************************************************************************
//
//! @brief Read the temperature in hundredths of a degree Celsius.
//!
//! @details Conversion: temperature(C)    = raw * 0.0078125
//!          temperature(x100) = raw * 0.78125 = raw * 100 / 128
//
//*****************************************************************************
uint32_t
TMP119_read_temp_c100(int32_t *pdata)
{
    int16_t  i16Raw = 0;
    uint32_t ui32Status;

    if ( pdata == NULL )
    {
        return 1;
    }

    if ( !g_bTmp119Ready )
    {
        *pdata = TMP119_TEMP_INVALID;
        return 1;
    }

    ui32Status = TMP119_read_temp_raw(&i16Raw);
    if ( ui32Status == 0 )
    {
        *pdata = ((int32_t)i16Raw * 100) / 128;
    }
    else
    {
        *pdata = TMP119_TEMP_INVALID;
    }

    return ui32Status;
}

//*****************************************************************************
//
//! @brief Read the configuration register.
//
//*****************************************************************************
uint32_t
TMP119_read_config(uint16_t *pdata)
{
    return tmp119_read_word(TMP119_REG_CONFIGURATION, pdata);
}

//*****************************************************************************
//
//! @brief Write the configuration register.
//
//*****************************************************************************
uint32_t
TMP119_write_config(uint16_t data)
{
    return tmp119_write_word(TMP119_REG_CONFIGURATION, data);
}
