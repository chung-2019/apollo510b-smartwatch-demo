//*****************************************************************************
//
//! @file asset_provision.c
//!
//! @brief Provision the built-in assets onto eMMC on first boot.
//!
//! The interface and its rationale are documented in asset_provision.h.
//!
//! This file is the only place in the firmware that still includes the asset
//! arrays. The drawing side (render_task.c, map_task.c) now reads everything
//! from eMMC, so with WATCHFACE_ASSET_PROVISION disabled the linker drops all
//! 2.16 MB at once, without having to strip includes file by file.
//
//*****************************************************************************

#include <stdint.h>
#include <stdio.h>

#include "am_mcu_apollo.h"
#include "am_util.h"

#include "asset_store.h"
#include "asset_provision.h"

#if WATCHFACE_ASSET_PROVISION
//
// Built-in assets. Used by provisioning only; the drawing path never touches
// these symbols.
//
#include "Generated_2/test_image_rgb565.h"
#include "generated/Watch_face/Watch7Background_noglow_rgb565.h"
#include "generated/Watch_face/Watch7Hour_rgba.h"
#include "generated/Watch_face/Watch7Minute_rgba.h"
#include "generated/Watch_face/Watch7Second_rgba.h"
#include "map/map_tile_00.h"
#include "map/map_tile_01.h"
#include "map/map_tile_02.h"
#endif

//*****************************************************************************
//
// Asset table.
//
// Name, size and built-in data live in one table, which removes any chance of
// renaming an entry and forgetting to update its size. pui8Data is NULL when
// provisioning is disabled; the table is then used for verification only.
//
//*****************************************************************************
typedef struct
{
    const char          *pcName;
    uint32_t             ui32Bytes;
    const uint8_t       *pui8Data;
} asset_entry_t;

#define ASSET_RGB565_BYTES(w, h)    ((uint32_t)(w) * (uint32_t)(h) * 2u)
#define ASSET_RGBA_BYTES(w, h)      ((uint32_t)(w) * (uint32_t)(h) * 4u)

#if WATCHFACE_ASSET_PROVISION
#define ASSET_SRC(p)    (p)
#else
#define ASSET_SRC(p)    (NULL)
#endif

static const asset_entry_t g_sAssets[] =
{
    {
        ASSET_HOME,
        ASSET_RGB565_BYTES(ASSET_HOME_W, ASSET_HOME_H),
        ASSET_SRC(g_ui8TestImageRGB565)
    },
    {
        ASSET_WATCH_BG,
        ASSET_RGB565_BYTES(ASSET_WATCH_BG_W, ASSET_WATCH_BG_H),
        ASSET_SRC(Watch7Background_noglow_rgb565)
    },
    {
        ASSET_HAND_HOUR,
        ASSET_RGBA_BYTES(ASSET_HAND_HOUR_W, ASSET_HAND_HOUR_H),
        ASSET_SRC(Watch7Hour_rgba)
    },
    {
        ASSET_HAND_MIN,
        ASSET_RGBA_BYTES(ASSET_HAND_MIN_W, ASSET_HAND_MIN_H),
        ASSET_SRC(Watch7Minute_rgba)
    },
    {
        ASSET_HAND_SEC,
        ASSET_RGBA_BYTES(ASSET_HAND_SEC_W, ASSET_HAND_SEC_H),
        ASSET_SRC(Watch7Second_rgba)
    },
    {
        "MAPTILE0.BIN",
        ASSET_RGB565_BYTES(ASSET_MAP_TILE_W, ASSET_MAP_TILE_H),
        ASSET_SRC(map_tile_00)
    },
    {
        "MAPTILE1.BIN",
        ASSET_RGB565_BYTES(ASSET_MAP_TILE_W, ASSET_MAP_TILE_H),
        ASSET_SRC(map_tile_01)
    },
    {
        "MAPTILE2.BIN",
        ASSET_RGB565_BYTES(ASSET_MAP_TILE_W, ASSET_MAP_TILE_H),
        ASSET_SRC(map_tile_02)
    }
};

#define ASSET_COUNT     (sizeof(g_sAssets) / sizeof(g_sAssets[0]))

//*****************************************************************************
//
// Verify the assets and fill in whatever is missing.
//
//*****************************************************************************
uint32_t
AssetProvisionRun(void)
{
    uint32_t i;
    uint32_t ui32Missing = 0;
    uint32_t ui32Written = 0;

    if ( !AssetStoreIsReady() )
    {
        am_util_debug_printf("[provision] store not ready, skipped\n");
        return ASSET_COUNT;
    }

#if WATCHFACE_ASSET_FORCE_WRITE
    //
    // Say this loudly. This mode rewrites 2 MB on every boot and must not ship
    // in production firmware.
    //
    am_util_stdio_printf("[provision] FORCE WRITE enabled - overwriting all\n");
#endif

    for ( i = 0; i < ASSET_COUNT; i++ )
    {
        const asset_entry_t *psEntry = &g_sAssets[i];

#if !WATCHFACE_ASSET_FORCE_WRITE
        if ( AssetStoreExists(psEntry->pcName, psEntry->ui32Bytes) )
        {
            //
            // Already present, so leave it completely alone. Artwork the user
            // replaced must not be overwritten by the boot path -- which is why
            // the test is "exists with the right length" and not a content compare.
            //
            // The flip side is that swapping artwork gets stuck: old and new
            // images usually have identical dimensions, so a matching length
            // skips the write. Flash once with WATCHFACE_ASSET_FORCE_WRITE set
            // to 1 and this entire check compiles out into an unconditional write.
            //
            continue;
        }
#endif

        if ( psEntry->pui8Data == NULL )
        {
            //
            // Provisioning is off and the file is absent. Resolving that is up
            // to the user (copy the file onto eMMC); the firmware can only state
            // clearly what is missing.
            //
            am_util_debug_printf("[provision] MISSING %s (%u bytes)\n",
                                 psEntry->pcName,
                                 (unsigned)psEntry->ui32Bytes);
            ui32Missing++;
            continue;
        }

        am_util_debug_printf("[provision] writing %s (%u bytes)...\n",
                             psEntry->pcName, (unsigned)psEntry->ui32Bytes);

        if ( AssetStoreWrite(psEntry->pcName, psEntry->pui8Data,
                             psEntry->ui32Bytes) )
        {
            ui32Written++;
        }
        else
        {
            ui32Missing++;
        }
    }

    if ( ui32Written > 0 )
    {
        am_util_debug_printf("[provision] %u asset(s) written\n",
                             (unsigned)ui32Written);
    }

    if ( ui32Missing > 0 )
    {
        am_util_debug_printf("[provision] %u asset(s) unavailable\n",
                             (unsigned)ui32Missing);
    }
    else
    {
        am_util_debug_printf("[provision] all %u assets present\n",
                             (unsigned)ASSET_COUNT);
    }

    return ui32Missing;
}
