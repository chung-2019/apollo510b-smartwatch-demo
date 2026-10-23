//*****************************************************************************
//
//! @file asset_store.c
//!
//! @brief Storage layer for the graphics assets: a FAT filesystem on eMMC.
//!
//! The interface and its rationale are in asset_store.h.
//
//*****************************************************************************

#include <string.h>

#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include "asset_store.h"

#include "ff.h"
#include "mmc_apollo5.h"

//
// The SDK provides no such macro; the emmc_bm_fatfs example defines its own, so do the same.
//
#define ALIGN(x)    __attribute__((aligned(x)))

//*****************************************************************************
//
// eMMC device and filesystem objects.
//
// All three are marked AM_SHARED_RW and aligned to 32 bytes: SDIO transfers are
// DMA and the FatFs sector buffers live inside the FATFS / FIL structures, so
// placing them in TCM or at an unaligned address leaves ADMA unable to reach
// them or reaching the wrong data. This follows the SDK's emmc_bm_fatfs example.
//
//*****************************************************************************
AM_SHARED_RW am_hal_card_t g_sEmmcCard[AM_REG_SDIO_NUM_MODULES] ALIGN(32);

static AM_SHARED_RW FATFS   g_sFatFs ALIGN(32);
static AM_SHARED_RW FIL     g_sFile ALIGN(32);
static AM_SHARED_RW char    g_cDrivePath[4] ALIGN(32);

//
// Work buffer for mkfs. Only the first boot uses it (when eMMC has no filesystem
// yet), but it has to be static -- DMA uses this area during mkfs, so it cannot
// sit on the caller's stack.
//
static AM_SHARED_RW uint8_t g_pui8WorkBuf[FF_MAX_SS] ALIGN(32);

static bool g_bStoreReady;

//*****************************************************************************
//
// The stage and FatFs error code of the last failure.
//
// They are stored because this log is easily missed: the PSRAM DDR timing scan
// floods dozens of printf lines, the RTT ring buffer in NO_BLOCK_TRIM mode drops
// whatever does not fit, and boot-phase messages often disappear wholesale. The
// display is never dropped, so the error code is carried to the map page instead.
//
//*****************************************************************************
static const char *g_pcFailStage = "";
static int         g_i32FailCode;

//*****************************************************************************
//
// Filesystem mutex.
//
// FatFs is built with FF_FS_REENTRANT = 0, so it does no locking of its own --
// not even "two FILs used separately" is safe, because the FATFS object holds a
// shared sector buffer. Every path into FatFs must go through this lock.
//
// While assets are loaded at boot the scheduler has not started; g_xLock is NULL
// then and lock/unlock are skipped -- that phase is single-threaded by
// construction. The lock is created only after AssetStoreInit() succeeds.
//
//*****************************************************************************
static SemaphoreHandle_t g_xLock;

static void
store_lock(void)
{
    if ( g_xLock != NULL )
    {
        (void)xSemaphoreTake(g_xLock, portMAX_DELAY);
    }
}

static void
store_unlock(void)
{
    if ( g_xLock != NULL )
    {
        (void)xSemaphoreGive(g_xLock);
    }
}

//*****************************************************************************
//
//! @brief Bus configurations to try.
//!
//! @details Why a table instead of one hard-coded set of parameters:
//!
//! The SDK default (1.8 V / SDR50 / 48 MHz / 8-bit) makes am_hal_card_cfg_set()
//! fail on this board -- the card itself is fine, the same boot already read its
//! serial number and EXT CSD (7.84 GB); it is the bus configuration step that
//! fails. The prime suspect is I/O voltage: the eMMC VCCQ is tied to SYS_VDD and
//! the schematic does not say whether that is 1.8 V or 3.3 V, while VCC is
//! clearly VDD_3V3.
//!
//! The HAL has AM_HAL_CARD_DEBUG, which would say whether voltage, speed or bus
//! width failed, but it expands to am_util_debug_printf and the HAL ships
//! pre-built as libam_hal.a -- those messages are not compiled in and cannot be seen.
//!
//! So the firmware tries for itself: the first combination that works is kept and printed.
//!
//! **On-target result**: this board works with `1.8V legacy 24M x8`. 1.8 V is
//! correct (3.3 V fails as well); what cannot be negotiated is the SDR50 mode --
//! both voltages with SDR50 make cfg_set fail. The verified combination is
//! therefore first in the table, so boot does not waste two failed attempts
//! (each of which includes a full card init).
//!
//! The later entries remain as fallbacks: with a different batch of cards or a
//! board revision, the firmware can still find a working configuration by itself
//! instead of losing eMMC entirely.
//!
//! 24 MHz is more than enough here: the assets total around 2 MB and are read once at boot.
//
//*****************************************************************************
typedef struct
{
    am_hal_host_bus_voltage_e eVoltage;
    am_hal_host_uhs_mode_e    eUHSMode;
    uint32_t                  ui32Clock;
    am_hal_host_bus_width_e   eBusWidth;
    const char               *pcLabel;
} emmc_bus_cfg_t;

static const emmc_bus_cfg_t g_sBusCfgs[] =
{
    //
    // The combination verified on target, placed first.
    //
    { AM_HAL_HOST_BUS_VOLTAGE_1_8, AM_HAL_HOST_UHS_NONE,  24000000,
      AM_HAL_HOST_BUS_WIDTH_8, "1.8V legacy 24M x8" },

    //
    // Fallbacks. The two SDR50 entries are rejected by cfg_set on this board; they
    // remain so a faster mode is still reachable after a card or board change.
    //
    { AM_HAL_HOST_BUS_VOLTAGE_1_8, AM_HAL_HOST_UHS_SDR50, 48000000,
      AM_HAL_HOST_BUS_WIDTH_8, "1.8V SDR50 48M x8" },
    { AM_HAL_HOST_BUS_VOLTAGE_3_3, AM_HAL_HOST_UHS_NONE,  24000000,
      AM_HAL_HOST_BUS_WIDTH_8, "3.3V legacy 24M x8" },
    { AM_HAL_HOST_BUS_VOLTAGE_3_3, AM_HAL_HOST_UHS_NONE,  24000000,
      AM_HAL_HOST_BUS_WIDTH_4, "3.3V legacy 24M x4" },
    { AM_HAL_HOST_BUS_VOLTAGE_1_8, AM_HAL_HOST_UHS_NONE,  12000000,
      AM_HAL_HOST_BUS_WIDTH_1, "1.8V legacy 12M x1" },
};

#define EMMC_BUS_CFG_COUNT  (sizeof(g_sBusCfgs) / sizeof(g_sBusCfgs[0]))

//*****************************************************************************
//
//! @brief Write one set of bus parameters into the FatFs driver's global configuration.
//!
//! @details mmc_disk_initialize() uses these values to initialize the SDHC and
//! the card, so they must be set before f_mount().
//!
//! @param psCfg The configuration to apply.
//
//*****************************************************************************
static void
asset_store_config_emmc(const emmc_bus_cfg_t *psCfg)
{
    g_sEMMC_hw.pDevHandle = &g_sEmmcCard[AM_BSP_SDIO_INSTANCE];

    g_sEMMC_config.eHost              = AM_BSP_SDIO_INSTANCE;
    g_sEMMC_config.ui32Clock          = psCfg->ui32Clock;
    g_sEMMC_config.eBusWidth          = psCfg->eBusWidth;
    g_sEMMC_config.eUHSMode           = psCfg->eUHSMode;
    g_sEMMC_config.eBusVoltage        = psCfg->eVoltage;
    g_sEMMC_config.eXferMode          = AM_HAL_HOST_XFER_ADMA;
    g_sEMMC_config.eCardType          = AM_HAL_CARD_TYPE_EMMC;
    g_sEMMC_config.eCardPwrCtrlPolicy = AM_HAL_CARD_PWR_CTRL_NONE;

    //
    // Synchronous mode: f_read() returns only once the read is complete. Assets
    // are read once at boot and there is nothing to do while waiting;
    // asynchronous mode would only add a completion flag to look after.
    //
    g_sEMMC_config.bAsync = false;
}

//*****************************************************************************
//
// Mount the filesystem.
//
//*****************************************************************************
bool
AssetStoreInit(void)
{
    FRESULT  eRes = FR_NOT_READY;
    uint32_t ui32Cfg;

    g_bStoreReady = false;

    //
    // Power up the eMMC first.
    //
    // The VCCQ and VCC of this eMMC each sit behind a load switch whose enable
    // pin is GPIO220 / GPIO221 (AM_BSP_GPIO_VCCQ_EMMC0 / VCC_EMMC0 in the BSP).
    // At boot both are low and the card is completely unpowered -- without this
    // step am_hal_card_host_find_card() finds no card, and the FatFs driver
    // responds by "retrying once a second, ten times", so boot wastes 10 seconds
    // before declaring failure.
    //
    // The function already includes the ordering and delay: VCCQ first, then VCC 50 ms later.
    //
    am_bsp_emmc_power_on(AM_BSP_SDIO_INSTANCE);

    //
    // Give the card one more hardware reset.
    //
    // The eMMC RSTn is on GPIO13 (AM_BSP_GPIO_SDIO0_RST), and the resistor that
    // would pull it high on the board (R46) is not fitted -- which means the pin
    // is entirely up to the firmware. GPIO does not default to output-high at
    // boot, so the card may sit in reset indefinitely: it has power, CMD may even
    // respond, but init cannot obtain the EXT CSD.
    //
    // am_bsp_sdio_reset() drives it low for 10 ms and then high, the reset pulse
    // the eMMC specification requires. Resetting an already healthy card is harmless.
    //
    am_bsp_sdio_reset(AM_BSP_SDIO_INSTANCE);

    //
    // Try the bus configurations one by one until the mount succeeds (see g_sBusCfgs).
    //
    // A mount parameter of 1 means "mount immediately", so every attempt really
    // touches the eMMC. FR_NO_FILESYSTEM means the bus is already working and the
    // card simply has no filesystem, so stop trying other combinations and format.
    //
    for ( ui32Cfg = 0; ui32Cfg < EMMC_BUS_CFG_COUNT; ui32Cfg++ )
    {
        asset_store_config_emmc(&g_sBusCfgs[ui32Cfg]);

        am_util_stdio_printf("[asset] mounting eMMC (%s)...\n",
                             g_sBusCfgs[ui32Cfg].pcLabel);

        eRes = f_mount(&g_sFatFs, (TCHAR const *)g_cDrivePath, 1);

        if ( (eRes == FR_OK) || (eRes == FR_NO_FILESYSTEM) )
        {
            am_util_stdio_printf("[asset] bus ok: %s (res=%d)\n",
                                 g_sBusCfgs[ui32Cfg].pcLabel, (int)eRes);
            break;
        }

        am_util_stdio_printf("[asset]   failed (%d)\n", (int)eRes);
    }

    //
    // The most common cause of failure is that the card has no filesystem yet
    // (eMMC ships blank), so format it once.
    //
    // Format only when the mount failed. The SDK example calls f_mkfs()
    // unconditionally on every boot, which is reasonable for a test program and
    // not here -- it would erase every asset the user placed over USB.
    //
    if ( eRes != FR_OK )
    {
        am_util_stdio_printf("[asset] mount failed (%d), formatting\n",
                             (int)eRes);

        //
        // Distinguish "no filesystem" from "cannot read the card".
        //
        // Only the former is a candidate for formatting. The latter
        // (FR_NOT_READY / FR_DISK_ERR) means the lower layer is not working at
        // all, mkfs would fail too, and reporting it as-is is clearer -- the
        // original code ran mkfs regardless, so the error finally seen was mkfs's
        // and hid the real cause.
        //
        if ( eRes != FR_NO_FILESYSTEM )
        {
            //
            // Every combination was tried and none worked. The error code is from
            // the last one, and the display shows "MOUNT ERR n".
            //
            g_pcFailStage = "MOUNT";
            g_i32FailCode = (int)eRes;
            am_util_stdio_printf("[asset] disk not ready, not formatting\n");
            return false;
        }

        eRes = f_mkfs((TCHAR const *)g_cDrivePath, 0,
                      g_pui8WorkBuf, sizeof(g_pui8WorkBuf));
        if ( eRes != FR_OK )
        {
            g_pcFailStage = "MKFS";
            g_i32FailCode = (int)eRes;
            am_util_stdio_printf("[asset] mkfs failed (%d)\n", (int)eRes);
            return false;
        }

        eRes = f_mount(&g_sFatFs, (TCHAR const *)g_cDrivePath, 1);
        if ( eRes != FR_OK )
        {
            g_pcFailStage = "REMOUNT";
            g_i32FailCode = (int)eRes;
            am_util_stdio_printf("[asset] mount after mkfs failed (%d)\n",
                                 (int)eRes);
            return false;
        }

        am_util_stdio_printf("[asset] formatted, empty volume\n");
    }

    g_bStoreReady = true;

    am_util_debug_printf("[asset] eMMC ready\n");

    return true;
}

//*****************************************************************************
//
// Query whether the filesystem is usable.
//
//*****************************************************************************
bool
AssetStoreIsReady(void)
{
    return g_bStoreReady;
}

//*****************************************************************************
//
// Get the stage and error code of the last failure.
//
//*****************************************************************************
int
AssetStoreGetFailure(const char **ppcStage)
{
    if ( ppcStage != NULL )
    {
        *ppcStage = g_pcFailStage;
    }

    return g_i32FailCode;
}

//*****************************************************************************
//
// Whether an asset file exists with the correct length.
//
//*****************************************************************************
bool
AssetStoreExists(const char *pcName, uint32_t ui32Size)
{
    FILINFO sInfo;
    bool    bOk;

    if ( (!g_bStoreReady) || (pcName == NULL) )
    {
        return false;
    }

    store_lock();
    bOk = (f_stat(pcName, &sInfo) == FR_OK) &&
          (sInfo.fsize == (FSIZE_t)ui32Size);
    store_unlock();

    return bOk;
}

//*****************************************************************************
//
// Delete an asset.
//
//*****************************************************************************
bool
AssetStoreDelete(const char *pcName)
{
    FRESULT eRes;

    if ( (!g_bStoreReady) || (pcName == NULL) )
    {
        return false;
    }

    store_lock();
    eRes = f_unlink(pcName);
    store_unlock();

    if ( eRes != FR_OK )
    {
        am_util_debug_printf("[asset] %s: unlink failed (%d)\n", pcName,
                             (int)eRes);
        return false;
    }

    am_util_debug_printf("[asset] %s: deleted\n", pcName);

    return true;
}

//*****************************************************************************
//
// Read an asset.
//
//*****************************************************************************
bool
AssetStoreRead(const char *pcName, void *pvBuf, uint32_t ui32Size)
{
    FRESULT eRes;
    UINT    uRead = 0;
    bool    bOk   = false;

    if ( (!g_bStoreReady) || (pcName == NULL) || (pvBuf == NULL) )
    {
        return false;
    }

    eRes = f_open(&g_sFile, pcName, FA_READ);
    if ( eRes != FR_OK )
    {
        am_util_debug_printf("[asset] %s: open failed (%d)\n", pcName,
                             (int)eRes);
        return false;
    }

    eRes = f_read(&g_sFile, pvBuf, (UINT)ui32Size, &uRead);

    if ( eRes != FR_OK )
    {
        am_util_debug_printf("[asset] %s: read failed (%d)\n", pcName,
                             (int)eRes);
    }
    else if ( uRead != (UINT)ui32Size )
    {
        //
    // Short file. Half an image is not accepted: a missing image is obvious at a
    // glance, while a half-drawn one can pass for a normal screen and is very
    // hard to trace later.
        //
        am_util_debug_printf("[asset] %s: short read %u/%u\n", pcName,
                             (unsigned)uRead, (unsigned)ui32Size);
    }
    else
    {
        bOk = true;
    }

    (void)f_close(&g_sFile);

    store_unlock();

    return bOk;
}

//*****************************************************************************
//
// Write an asset.
//
//*****************************************************************************
bool
AssetStoreWrite(const char *pcName, const void *pvBuf, uint32_t ui32Size)
{
    FRESULT eRes;
    UINT    uWritten = 0;
    bool    bOk      = false;

    if ( (!g_bStoreReady) || (pcName == NULL) || (pvBuf == NULL) )
    {
        return false;
    }

    eRes = f_open(&g_sFile, pcName, FA_CREATE_ALWAYS | FA_WRITE);
    if ( eRes != FR_OK )
    {
        am_util_debug_printf("[asset] %s: create failed (%d)\n", pcName,
                             (int)eRes);
        return false;
    }

    eRes = f_write(&g_sFile, pvBuf, (UINT)ui32Size, &uWritten);

    if ( (eRes == FR_OK) && (uWritten == (UINT)ui32Size) )
    {
        bOk = true;
    }
    else
    {
        am_util_debug_printf("[asset] %s: write failed (%d, %u/%u)\n", pcName,
                             (int)eRes, (unsigned)uWritten,
                             (unsigned)ui32Size);
    }

    //
    // The close is mandatory: FatFs writes the directory entry and the FAT back
    // here, and without it the file appears to be 0 bytes.
    //
    if ( f_close(&g_sFile) != FR_OK )
    {
        bOk = false;
    }

    store_unlock();

    return bOk;
}

//*****************************************************************************
//
// List the contents of the root directory.
//
//*****************************************************************************
int32_t
AssetStoreList(asset_list_cb_t pfnEntry, uint32_t *pui32Total,
               uint32_t *pui32Free)
{
    DIR     sDir;
    FILINFO sInfo;
    int32_t i32Count = 0;
    FRESULT eRes;

    if ( !g_bStoreReady )
    {
        return -1;
    }

    store_lock();

    //
    // Capacity first. f_getfree() returns the number of free clusters, which must
    // be multiplied by sectors per cluster and then by sector size to get bytes.
    //
    if ( (pui32Total != NULL) || (pui32Free != NULL) )
    {
        FATFS   *psFs = NULL;
        DWORD    dwFreeClust = 0;

        if ( f_getfree((const TCHAR *)g_cDrivePath, &dwFreeClust, &psFs)
             == FR_OK )
        {
            //
            // n_fatent is the number of FAT entries; the first two are reserved,
            // so subtract them for the data cluster count. The multiplication goes
            // through uint64: an 8 GB card overflows 32 bits.
            //
            uint64_t ui64Total = (uint64_t)(psFs->n_fatent - 2)
                               * psFs->csize * FF_MAX_SS;
            uint64_t ui64Free  = (uint64_t)dwFreeClust
                               * psFs->csize * FF_MAX_SS;

            if ( pui32Total != NULL )
            {
                *pui32Total = (uint32_t)(ui64Total / 1024u);    // KB
            }
            if ( pui32Free != NULL )
            {
                *pui32Free = (uint32_t)(ui64Free / 1024u);      // KB
            }
        }
    }

    eRes = f_opendir(&sDir, (const TCHAR *)"");
    if ( eRes != FR_OK )
    {
        store_unlock();
        am_util_stdio_printf("[asset] opendir failed (%d)\n", (int)eRes);
        return -1;
    }

    while (1)
    {
        eRes = f_readdir(&sDir, &sInfo);

        //
        // At the end FatFs does not return an error but an empty file name.
        //
        if ( (eRes != FR_OK) || (sInfo.fname[0] == 0) )
        {
            break;
        }

        //
        // List files only, skip directories. All assets are in the root directory,
        // so there is no recursion here.
        //
        if ( (sInfo.fattrib & AM_DIR) == 0 )
        {
            i32Count++;
            if ( pfnEntry != NULL )
            {
                pfnEntry(sInfo.fname, (uint32_t)sInfo.fsize);
            }
        }
    }

    (void)f_closedir(&sDir);

    store_unlock();

    return i32Count;
}

//*****************************************************************************
//
// Streaming write.
//
// The three functions together hold one lock: Begin acquires it, End / Abort
// release it. Other readers are blocked during the transfer; see asset_store.h.
//
//*****************************************************************************
//
// The file name actually opened for a streaming write.
//
// The target file is not written directly: FA_CREATE_ALWAYS destroys the old
// contents immediately, so failing halfway (RTT disconnect, timeout) would
// destroy a perfectly good asset -- which did happen: one failed upload turned
// three map tiles into a few tens of KB of debris and left the screen with no map.
//
// Instead a temporary file is written first and renamed over the target only
// once it is complete and the length is right. On failure not one byte of the
// target file has been touched.
//
#define ASSET_TMP_NAME      "~UPLOAD.TMP"

static char g_cWriteTarget[16];

bool
AssetStoreWriteBegin(const char *pcName)
{
    FRESULT eRes;

    if ( (!g_bStoreReady) || (pcName == NULL) )
    {
        return false;
    }

    if ( strlen(pcName) >= sizeof(g_cWriteTarget) )
    {
        am_util_stdio_printf("[asset] %s: name too long\n", pcName);
        return false;
    }

    store_lock();

    //
    // Clear any debris left by a previous attempt; CREATE_ALWAYS does truncate,
    // but the state before f_rename would still not be clean.
    //
    (void)f_unlink(ASSET_TMP_NAME);

    eRes = f_open(&g_sFile, ASSET_TMP_NAME, FA_CREATE_ALWAYS | FA_WRITE);
    if ( eRes != FR_OK )
    {
        store_unlock();
        am_util_stdio_printf("[asset] %s: create failed (%d)\n", pcName,
                             (int)eRes);
        return false;
    }

    memset(g_cWriteTarget, 0, sizeof(g_cWriteTarget));
    strcpy(g_cWriteTarget, pcName);

    return true;
}

bool
AssetStoreWriteChunk(const void *pvBuf, uint32_t ui32Size)
{
    UINT uWritten = 0;

    if ( (pvBuf == NULL) || (ui32Size == 0) )
    {
        return false;
    }

    return (f_write(&g_sFile, pvBuf, (UINT)ui32Size, &uWritten) == FR_OK) &&
           (uWritten == (UINT)ui32Size);
}

bool
AssetStoreWriteEnd(void)
{
    bool    bOk = (f_close(&g_sFile) == FR_OK);
    FRESULT eRes;

    if ( bOk )
    {
        //
        // Swap it in. f_rename does not overwrite an existing file, so unlink
        // comes first -- for that instant the target really is absent, but the
        // temporary file is already complete, so the worst case on power loss is
        // a missing file at the next boot (handled by the provisioning check),
        // never half a file.
        //
        (void)f_unlink(g_cWriteTarget);

        eRes = f_rename(ASSET_TMP_NAME, g_cWriteTarget);
        if ( eRes != FR_OK )
        {
            am_util_stdio_printf("[asset] %s: rename failed (%d)\n",
                                 g_cWriteTarget, (int)eRes);
            bOk = false;
        }
    }

    if ( !bOk )
    {
        (void)f_unlink(ASSET_TMP_NAME);
    }

    store_unlock();

    return bOk;
}

void
AssetStoreWriteAbort(void)
{
    (void)f_close(&g_sFile);

    //
    // Only the temporary file is discarded; the target file was never touched.
    //
    (void)f_unlink(ASSET_TMP_NAME);

    store_unlock();
}
