//*****************************************************************************
//
//! @file map_task.c
//!
//! @brief Map tile cache (MRAM -> PSRAM) and navigation simulation.
//!
//! The interface and its rationale are in map_task.h.
//
//*****************************************************************************

#include <math.h>
#include <string.h>

#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

#include "FreeRTOS.h"
#include "task.h"

#include "nemagfx_watchface.h"
#include "map_task.h"
#include "map_route.h"
#include "psram.h"
#include "asset_store.h"

//*****************************************************************************
//
// Task handle.
//
//*****************************************************************************
TaskHandle_t MapTaskHandle;

//*****************************************************************************
//
// Tile size in bytes and its layout in PSRAM.
//
// The cache slots are aligned to 4 KB simply so the addresses read well in the
// log, and so a later switch to MSPI DMA needs no thought about alignment. One
// tile is 438,048 bytes and occupies 442,368 after alignment; two slots come to
// under 1 MB -- ample in 64 MB of PSRAM.
//
//*****************************************************************************
#define MAP_TILE_BYTES          (MAP_TILE_W * MAP_TILE_H * 2)
#define MAP_SLOT_STRIDE         (((MAP_TILE_BYTES + 0xFFF) / 0x1000) * 0x1000)
#define MAP_SLOT_ADDR(slot)     (PSRAM_MAP_BASE + (uint32_t)(slot) * MAP_SLOT_STRIDE)

//
// The whole cache must stay inside the region psram.h gives the map, or it would
// overwrite the weather page's images that follow. The slot count is derived
// from the minimum zoom (MAP_CACHE_SLOTS), so a change there is checked here.
//
typedef char map_cache_fits_psram_region
    [((MAP_CACHE_SLOTS * MAP_SLOT_STRIDE) <= PSRAM_MAP_SIZE) ? 1 : -1];

//*****************************************************************************
//
// Storage layer: the tiles live on the eMMC FAT as MAPTILE0.BIN .. MAPTILE2.BIN.
//
// These used to be three const arrays in MRAM. After the switch to files,
// nothing above map_cache_fetch() changed at all -- which is exactly why the
// storage layer was isolated in the first place.
//
//*****************************************************************************

//*****************************************************************************
//
// PSRAM tile cache.
//
//*****************************************************************************
typedef struct
{
    //
    // Which tile this slot currently holds; -1 means empty.
    //
    int32_t  i32TileIdx;

    //
    // Serial number of the last use. A monotonically increasing counter rather
    // than a tick: two tiles used in the same frame share a tick, which would
    // make it impossible to tell which is older.
    //
    uint32_t ui32LastUse;
} map_slot_t;

static map_slot_t g_sSlots[MAP_CACHE_SLOTS];
static uint32_t   g_ui32UseCounter;

//
// Cache statistics, for the log only -- loads far outnumbering boundary crossings means thrashing.
//
static uint32_t   g_ui32Hits;
static uint32_t   g_ui32Misses;

//
// Whether each tile is currently in the "cannot be read" state. Used only to
// throttle the log (see map_tile_load_from_store) -- the load itself is still
// retried every frame.
//
static bool       g_bTileFailed[MAP_TILE_COUNT];

//*****************************************************************************
//
// Navigation simulation state.
//
//*****************************************************************************

//
// Which segment we are on (g_sMapRoute[i] -> g_sMapRoute[i+1]) and how many
// pixels have been travelled along it.
//
static uint32_t   g_ui32Seg;
static float      g_fSegPos;

//
// Length of each segment and the total route length, computed once at boot.
//
static float      g_pfSegLen[MAP_ROUTE_PT_COUNT - 1];
static float      g_fRouteLen;

//
// The snapshot exposed to the outside.
//
static map_view_t g_sView;

//
// Whether the map page is on screen. If not, the simulation pauses.
//
static bool       g_bActive;

//
// When the user last dragged, used to decide when to return to follow mode.
//
static uint32_t   g_ui32LastDragTick;

//
// State transition notification flags.
//
// Navigation advance and camera update both happen inside a critical section
// (the snapshot has to be consistent), and am_util_debug_printf() does not
// belong there -- an RTT write is only a memcpy, but with interrupts off it is
// still unnecessary latency, and the log content has nothing to do with
// snapshot consistency. So those two places only raise a flag and the printing
// happens after leaving the critical section.
//
static bool       g_bLogArrived;
static bool       g_bLogFollow;

//*****************************************************************************
//
//! @brief How long after the finger lifts before follow mode resumes.
//!
//! @details 3 seconds: long enough to study the junction that was dragged into
//! view, short enough not to look like broken follow. Phone navigation apps
//! mostly land between 3 and 5 seconds.
//
//*****************************************************************************
#define MAP_FOLLOW_RESUME_MS    (3000)

//*****************************************************************************
//
//! @brief Update period of the simulation.
//!
//! @details 33 ms is roughly the display cadence (measured fps is around 30).
//! Faster than the frame rate is pointless -- RenderTask reads the snapshot once
//! per frame; slower and the vehicle visibly steps along.
//
//*****************************************************************************
#define MAP_TICK_MS             (33)

//*****************************************************************************
//
//! @brief The range the camera may stop in.
//!
//! @details The window is as wide as the screen and the world is 1404 wide, so
//! the camera's x falls in 0..936. Clamping it keeps blank space beyond the map
//! off the screen.
//
//*****************************************************************************
//*****************************************************************************
//
//! @brief Where on screen the vehicle must stay.
//!
//! @details Only two things on screen can obscure the vehicle: the HUD panel at
//! the top (44..114) and the progress bar at the bottom (406..412). The vehicle
//! with its ring is 16 px, leaving 6 px of margin each side, giving 136..384.
//!
//! The route's y spans 105 (the Taipei Main Station segment) to 340 (the
//! Taipei 101 segment). The upper bound of 136 still catches the destination
//! segment, so the camera must still be able to push up; with the lower bound
//! relaxed to 384, the start's 340 is already inside the band -- that end needs
//! no push and so no longer exposes an edge.
//!
//! (The lower bound used to be 332, because the lower half of the screen carried
//! a symmetric HUD panel. That panel and its distance / time text have been
//! removed, which widened the band.)
//!
//! These two values are **screen** coordinates and are independent of zoom;
//! dividing by fZoom happens only when converting to how far the camera must
//! move in world pixels. They are screen coordinates because what has to be
//! avoided is fixed on the screen and does not scale with the map.
//
//*****************************************************************************
#define MAP_CAR_SCREEN_MIN_Y    (136.0f)
#define MAP_CAR_SCREEN_MAX_Y    (384.0f)

//*****************************************************************************
//
//! @brief Copy a tile from the storage layer into one PSRAM slot.
//!
//! @details Reads MAPTILEn.BIN on eMMC straight into that PSRAM slot. The
//! destination is an XIP address, so FatFs writes the data all the way into
//! PSRAM with no SSRAM staging in between.
//!
//! The D-cache must be flushed afterwards. FatFs may have moved the data with
//! the CPU, leaving those bytes in cache, while the GPU is a separate bus master
//! reading PSRAM itself: without the flush the GPU samples pre-copy contents and
//! the screen shows an old tile or noise, intermittently -- the kind of problem
//! that is very hard to trace afterwards, so flushing every time is the cheaper
//! choice.
//!
//! On a read failure (file missing or wrong length) the slot is marked empty and the drawing side skips it.
//!
//! @param ui32TileIdx Tile index.
//! @param ui32Slot    Destination slot.
//!
//! @return true when the tile is in the slot.
//
//*****************************************************************************
static bool
map_tile_load_from_store(uint32_t ui32TileIdx, uint32_t ui32Slot)
{
    uint32_t ui32Addr  = MAP_SLOT_ADDR(ui32Slot);
    uint32_t ui32Start = xTaskGetTickCount();
    char     cName[16];

    am_hal_cachectrl_range_t sRange;

    //
    // Zero it before formatting.
    //
    // am_util_stdio_snprintf() is not the standard snprintf: it only copies
    // characters and **writes no NUL terminator** (the loop in vsnprintf in
    // am_util_stdio.c simply returns when it ends). Without this memset, cName is
    // stack garbage appended to "MAPTILE0.BIN", f_open() then fails to find the
    // file and returns FR_NO_FILE -- and since stack contents vary, the symptom
    // is "it works occasionally on a retry", with the map taking seconds to appear.
    //
    memset(cName, 0, sizeof(cName));
    am_util_stdio_snprintf(cName, sizeof(cName), ASSET_MAP_TILE_FMT,
                           (unsigned)ui32TileIdx);

    if ( !AssetStoreRead(cName, (void *)ui32Addr, MAP_TILE_BYTES) )
    {
        //
        // Say it once per run of consecutive failures on the same tile.
        //
        // An unreadable tile is retried every frame (deliberately: once the user
        // supplies the file over USB it recovers without a reboot), but one line
        // per frame would be log spam -- measured, it fills the RTT buffer and
        // squeezes out every other task's messages.
        //
        if ( !g_bTileFailed[ui32TileIdx] )
        {
            g_bTileFailed[ui32TileIdx] = true;
            am_util_stdio_printf("[map] %s: load failed (will keep retrying)\n",
                                 cName);
        }
        return false;
    }

    g_bTileFailed[ui32TileIdx] = false;

    sRange.ui32StartAddr = ui32Addr;
    sRange.ui32Size      = MAP_TILE_BYTES;
    am_hal_cachectrl_dcache_clean(&sRange);

    am_util_debug_printf("[map] %s -> slot %u @0x%08X (%u ms)\n",
                         cName, ui32Slot, ui32Addr,
                         (uint32_t)(xTaskGetTickCount() - ui32Start));

    return true;
}

//*****************************************************************************
//
//! @brief Ensure a tile is in the cache, evicting the least recently used slot if necessary.
//!
//! @param ui32TileIdx Tile index.
//!
//! @return The slot holding that tile.
//
//*****************************************************************************
static uint32_t
map_cache_fetch(uint32_t ui32TileIdx)
{
    uint32_t i;
    uint32_t ui32Victim = 0;

    //
    // Already cached: just update the use serial.
    //
    for ( i = 0; i < MAP_CACHE_SLOTS; i++ )
    {
        if ( g_sSlots[i].i32TileIdx == (int32_t)ui32TileIdx )
        {
            g_sSlots[i].ui32LastUse = ++g_ui32UseCounter;
            g_ui32Hits++;
            return i;
        }
    }

    //
    // Otherwise look for an empty slot, and only pick the least recently used one if there is none.
    //
    // Both cases are handled in one loop: an empty slot has ui32LastUse of 0,
    // which is below any used slot, so "smallest ui32LastUse" naturally picks the
    // empty ones first.
    //
    for ( i = 1; i < MAP_CACHE_SLOTS; i++ )
    {
        if ( g_sSlots[i].ui32LastUse < g_sSlots[ui32Victim].ui32LastUse )
        {
            ui32Victim = i;
        }
    }

    if ( g_sSlots[ui32Victim].i32TileIdx >= 0 )
    {
        am_util_debug_printf("[map] evict tile %d from slot %u\n",
                             g_sSlots[ui32Victim].i32TileIdx, ui32Victim);
    }

    if ( map_tile_load_from_store(ui32TileIdx, ui32Victim) )
    {
        g_sSlots[ui32Victim].i32TileIdx = (int32_t)ui32TileIdx;
    }
    else
    {
        //
        // On a read failure mark the slot empty. Keeping the old index would make
        // the drawing side think there is an image there and draw the previous
        // tile's contents in the wrong place -- harder to spot than a missing block.
        //
        g_sSlots[ui32Victim].i32TileIdx = -1;
    }

    g_sSlots[ui32Victim].ui32LastUse = ++g_ui32UseCounter;
    g_ui32Misses++;

    return ui32Victim;
}

//*****************************************************************************
//
//! @brief Prepare the tiles needed for the current camera position.
//!
//! @details Whichever tiles the left and right edges of the window fall on, plus
//! everything in between. At native resolution that is at most two (the window
//! is as wide as a tile); zoomed out it becomes three -- the slot count was
//! derived from MAP_ZOOM_MIN, so there are always enough.
//!
//! Why eviction here cannot overwrite a tile the GPU is sampling: the drawing
//! side calls MapTaskGetTileAddr() every frame for every tile it draws, and that
//! call updates the use serial, so whatever is visible on screen is always the
//! most recently touched -- and those one or two are exactly what this function
//! wants. The "least recently used" slot is therefore always an off-screen tile
//! -- unless the window skipped a whole tile (468 px) within a single frame, and
//! a drag moves tens of pixels per frame at most, with the simulation slower still.
//!
//! @param fCamX The camera's world x.
//
//*****************************************************************************
static void
map_cache_prepare(float fCamX)
{
    //
    // The world width the window covers changes with zoom: the further out, the
    // wider the view and the more tiles needed. Before pinch zoom this was a
    // hard-coded FB_RESX, spanning two at most.
    //
    float   fViewW   = (float)FB_RESX / g_sView.fZoom;

    int32_t i32Left  = (int32_t)(fCamX / (float)MAP_TILE_W);
    int32_t i32Right = (int32_t)((fCamX + fViewW - 1.0f) / (float)MAP_TILE_W);
    int32_t i32Idx;

    if ( i32Left < 0 )
    {
        i32Left = 0;
    }
    if ( i32Right > (MAP_TILE_COUNT - 1) )
    {
        i32Right = MAP_TILE_COUNT - 1;
    }

    //
    // Fetched left to right. The order matters: when none are cached, the left
    // one takes the older slot first, so the right one does not evict what was
    // just loaded. The slot count is derived from MAP_ZOOM_MIN (see
    // MAP_CACHE_SLOTS in map_task.h), so this range never exceeds it.
    //
    for ( i32Idx = i32Left; i32Idx <= i32Right; i32Idx++ )
    {
        (void)map_cache_fetch((uint32_t)i32Idx);
    }
}

//*****************************************************************************
//
//! @brief Compute the length of every segment of the route.
//!
//! @details Every segment is horizontal or vertical (that is how the route is
//! drawn on the base map), so the length is really the absolute coordinate
//! difference; sqrtf keeps it general so that adding diagonals later needs no
//! change here, and nothing else uses anything but the length.
//
//*****************************************************************************
static void
map_route_measure(void)
{
    uint32_t i;

    g_fRouteLen = 0.0f;

    for ( i = 0; i < (MAP_ROUTE_PT_COUNT - 1); i++ )
    {
        float fDx = (float)(g_sMapRoute[i + 1].i16X - g_sMapRoute[i].i16X);
        float fDy = (float)(g_sMapRoute[i + 1].i16Y - g_sMapRoute[i].i16Y);

        g_pfSegLen[i] = sqrtf(fDx * fDx + fDy * fDy);
        g_fRouteLen  += g_pfSegLen[i];
    }
}

//*****************************************************************************
//
//! @brief Decide whether the turn from segment n into n+1 is left or right.
//!
//! @details Uses the z component of the cross product of the two direction
//! vectors: screen y runs downward, so a positive cross product means clockwise,
//! i.e. a right turn. The last segment has no successor and returns "arrived".
//!
//! @param ui32Seg The current segment.
//!
//! @return The turn direction.
//
//*****************************************************************************
static map_turn_e
map_turn_at(uint32_t ui32Seg)
{
    float fAx, fAy, fBx, fBy, fCross;

    if ( (ui32Seg + 2) >= MAP_ROUTE_PT_COUNT )
    {
        return MAP_TURN_ARRIVE;
    }

    fAx = (float)(g_sMapRoute[ui32Seg + 1].i16X - g_sMapRoute[ui32Seg].i16X);
    fAy = (float)(g_sMapRoute[ui32Seg + 1].i16Y - g_sMapRoute[ui32Seg].i16Y);
    fBx = (float)(g_sMapRoute[ui32Seg + 2].i16X - g_sMapRoute[ui32Seg + 1].i16X);
    fBy = (float)(g_sMapRoute[ui32Seg + 2].i16Y - g_sMapRoute[ui32Seg + 1].i16Y);

    fCross = (fAx * fBy) - (fAy * fBx);

    if ( fCross > 0.5f )
    {
        return MAP_TURN_RIGHT;
    }
    if ( fCross < -0.5f )
    {
        return MAP_TURN_LEFT;
    }

    return MAP_TURN_STRAIGHT;
}

//*****************************************************************************
//
//! @brief Advance the navigation state by one tick and update the exposed snapshot.
//!
//! @param fDtSec Seconds since the last update (real time, before the simulation scale).
//
//*****************************************************************************
static void
map_nav_step(float fDtSec)
{
    //
    // Pixel speed: 40 km/h in metres per second, divided by the scale to get
    // pixels per second, then multiplied by the simulation factor.
    //
    const float fPxPerSec = (MAP_SIM_SPEED_KMH / 3.6f)
                            / MAP_METERS_PER_PX * MAP_SIM_TIME_SCALE;

    float    fRemainPx;
    float    fT;
    uint32_t i;

    //
    // While stopped the position does not advance, but all the conversions below
    // still run -- remaining distance, next turn and so on must stay valid, since
    // a stopped HUD still shows "how far there is to go".
    //
    if ( (!g_sView.bArrived) && (!g_sView.bPaused) )
    {
        g_fSegPos += fPxPerSec * fDtSec;

        //
    // Crossing into the next segment. A while rather than an if: at a high
    // simulation factor one tick can cover a whole short segment (the shortest is
    // only 45 px, under a second at 47 px/s).
        //
        while ( g_fSegPos >= g_pfSegLen[g_ui32Seg] )
        {
            g_fSegPos -= g_pfSegLen[g_ui32Seg];
            g_ui32Seg++;

            if ( g_ui32Seg >= (MAP_ROUTE_PT_COUNT - 1) )
            {
                g_ui32Seg        = MAP_ROUTE_PT_COUNT - 2;
                g_fSegPos        = g_pfSegLen[g_ui32Seg];
                g_sView.bArrived = true;
                g_bLogArrived    = true;
                break;
            }
        }
    }

    //
    // Vehicle position: interpolated along the current segment.
    //
    fT = (g_pfSegLen[g_ui32Seg] > 0.0f)
         ? (g_fSegPos / g_pfSegLen[g_ui32Seg]) : 0.0f;

    g_sView.fCarX = (float)g_sMapRoute[g_ui32Seg].i16X
                    + fT * (float)(g_sMapRoute[g_ui32Seg + 1].i16X
                                   - g_sMapRoute[g_ui32Seg].i16X);
    g_sView.fCarY = (float)g_sMapRoute[g_ui32Seg].i16Y
                    + fT * (float)(g_sMapRoute[g_ui32Seg + 1].i16Y
                                   - g_sMapRoute[g_ui32Seg].i16Y);

    //
    // Heading: the direction of the current segment. atan2 takes y in screen
    // coordinates (positive downward), so the angle feeds the drawing side's
    // rotation matrix directly.
    //
    g_sView.fHeading = atan2f(
        (float)(g_sMapRoute[g_ui32Seg + 1].i16Y - g_sMapRoute[g_ui32Seg].i16Y),
        (float)(g_sMapRoute[g_ui32Seg + 1].i16X - g_sMapRoute[g_ui32Seg].i16X))
        * (180.0f / 3.14159265f);

    //
    // Remaining distance: what is left of this segment plus the full length of each one after it.
    //
    fRemainPx = g_pfSegLen[g_ui32Seg] - g_fSegPos;
    for ( i = g_ui32Seg + 1; i < (MAP_ROUTE_PT_COUNT - 1); i++ )
    {
        fRemainPx += g_pfSegLen[i];
    }

    g_sView.ui32RemainM  = (uint32_t)(fRemainPx * MAP_METERS_PER_PX);
    g_sView.ui32EtaSec   = (uint32_t)((float)g_sView.ui32RemainM
                                      / (MAP_SIM_SPEED_KMH / 3.6f));
    g_sView.ui32Progress = (g_fRouteLen > 0.0f)
                           ? (uint32_t)((1.0f - (fRemainPx / g_fRouteLen)) * 100.0f)
                           : 0;

    //
    // The next manoeuvre and its distance.
    //
    g_sView.eNextTurn     = g_sView.bArrived ? MAP_TURN_ARRIVE
                                             : map_turn_at(g_ui32Seg);
    g_sView.ui32NextTurnM = (uint32_t)((g_pfSegLen[g_ui32Seg] - g_fSegPos)
                                       * MAP_METERS_PER_PX);
}

//*****************************************************************************
//
//! @brief Clamp the camera's x into the world.
//!
//! @param pfCamX The camera x to clamp.
//
//*****************************************************************************
static void
map_camera_clamp_x(float *pfCamX)
{
    //
    // The world width the window covers changes with zoom, and the movable range follows.
    //
    float fSpan = (float)MAP_WORLD_W - ((float)FB_RESX / g_sView.fZoom);

    if ( fSpan <= 0.0f )
    {
        //
        // Zoomed out far enough that the whole map is narrower than the window:
        // there is nothing to look left or right at, so centre it. The current
        // MAP_ZOOM_MIN never reaches here (a 0.6x window is 780 px and the map is
        // 1404), but it stays so that relaxing the bound later needs no addition.
        //
        *pfCamX = fSpan * 0.5f;
        return;
    }

    if ( *pfCamX < 0.0f )
    {
        *pfCamX = 0.0f;
    }
    if ( *pfCamX > fSpan )
    {
        *pfCamX = fSpan;
    }
}

//*****************************************************************************
//
//! @brief Clamp the camera's y into a sensible range.
//!
//! @details Two constraints, and their order matters:
//!
//!   1. First clamp to "no exposed edge" -- keep the top and bottom of the map off screen. That is the default to honour.
//!   2. Only when that would leave the vehicle under the HUD is it relaxed, just far enough to push the vehicle back into the safe band.
//!
//! The other way round (vehicle position first, edges second) would push the
//! camera away on the long main-road stretch as well, leaving a band of
//! background along the screen edge for the whole trip; in the present order,
//! only the start and the destination expose anything.
//!
//! @param pfCamY  The camera y to clamp.
//! @param bKeepCar Whether to guarantee the vehicle stays in the safe band (not
//!                 needed while browsing freely, where the vehicle may be off screen).
//
//*****************************************************************************
static void
map_camera_clamp_y(float *pfCamY, bool bKeepCar)
{
    float fViewH = (float)FB_RESY / g_sView.fZoom;
    float fSpan  = (float)MAP_WORLD_H - fViewH;
    float fCarScreenY;

    //
    // First layer: no exposed edge. When the world is shorter than the window
    // (zoomed out) fSpan is negative, and centring is then the only position that
    // exposes the least.
    //
    if ( fSpan <= 0.0f )
    {
        *pfCamY = fSpan * 0.5f;
    }
    else
    {
        if ( *pfCamY < 0.0f )
        {
            *pfCamY = 0.0f;
        }
        if ( *pfCamY > fSpan )
        {
            *pfCamY = fSpan;
        }
    }

    if ( !bKeepCar )
    {
        return;
    }

    //
    // Second layer: the vehicle must not be hidden by the HUD. The division by
    // fZoom is because the safe band is in screen coordinates while the camera
    // moves in world coordinates.
    //
    fCarScreenY = (g_sView.fCarY - *pfCamY) * g_sView.fZoom;

    if ( fCarScreenY > MAP_CAR_SCREEN_MAX_Y )
    {
        *pfCamY = g_sView.fCarY - (MAP_CAR_SCREEN_MAX_Y / g_sView.fZoom);
    }
    else if ( fCarScreenY < MAP_CAR_SCREEN_MIN_Y )
    {
        *pfCamY = g_sView.fCarY - (MAP_CAR_SCREEN_MIN_Y / g_sView.fZoom);
    }
}

//*****************************************************************************
//
//! @brief Update the camera.
//!
//! @details Follow mode centres the vehicle; free mode keeps where the user
//! dragged to until the finger has been up for MAP_FOLLOW_RESUME_MS.
//!
//! Returning to follow moves the camera by linear interpolation rather than
//! jumping -- a jump would skip a long way with no sense of where to. The 0.12
//! factor is the share of the remaining gap closed each frame, converging in
//! about half a second.
//
//*****************************************************************************
static void
map_camera_update(void)
{
    float fTargetX;
    float fTargetY;

    if ( g_sView.eMode == MAP_MODE_FREE )
    {
        if ( (xTaskGetTickCount() - g_ui32LastDragTick)
             < pdMS_TO_TICKS(MAP_FOLLOW_RESUME_MS) )
        {
            return;
        }

        g_sView.eMode = MAP_MODE_FOLLOW;
        g_bLogFollow  = true;
    }

    //
    // The ideal position centres the vehicle. The window's size in the world
    // changes with zoom, hence the division by fZoom rather than a fixed FB_RESX/2.
    //
    fTargetX = g_sView.fCarX - ((float)FB_RESX / g_sView.fZoom) * 0.5f;
    fTargetY = g_sView.fCarY - ((float)FB_RESY / g_sView.fZoom) * 0.5f;

    map_camera_clamp_x(&fTargetX);
    map_camera_clamp_y(&fTargetY, true);

    g_sView.fCamX += (fTargetX - g_sView.fCamX) * 0.12f;
    g_sView.fCamY += (fTargetY - g_sView.fCamY) * 0.12f;
}

//*****************************************************************************
//
//! @brief Reset the navigation to the start.
//
//*****************************************************************************
static void
map_nav_reset(void)
{
    g_ui32Seg        = 0;
    g_fSegPos        = 0.0f;
    g_sView.bArrived = false;
    g_sView.bPaused  = false;
    g_sView.eMode    = MAP_MODE_FOLLOW;

    map_nav_step(0.0f);

    //
    // The zoom returns to native resolution as well. A restart means "the whole
    // thing again", and keeping the user's half-finished pinch would only confuse.
    //
    g_sView.fZoom = 1.0f;

    //
    // The camera jumps into place rather than sliding from where it was -- a restart should be clean.
    //
    g_sView.fCamX = g_sView.fCarX - ((float)FB_RESX / g_sView.fZoom) * 0.5f;
    g_sView.fCamY = g_sView.fCarY - ((float)FB_RESY / g_sView.fZoom) * 0.5f;

    map_camera_clamp_x(&g_sView.fCamX);
    map_camera_clamp_y(&g_sView.fCamY, true);
}

//*****************************************************************************
//
// The map task itself.
//
//*****************************************************************************
void
MapTask(void *pvParameters)
{
    uint32_t i;
    uint32_t ui32LastTick;

    (void)pvParameters;

    for ( i = 0; i < MAP_CACHE_SLOTS; i++ )
    {
        g_sSlots[i].i32TileIdx  = -1;
        g_sSlots[i].ui32LastUse = 0;
    }

    map_route_measure();
    map_nav_reset();

    //
    // Without PSRAM there is nowhere to put the tiles. The task stays alive and
    // maintains navigation state so the map page can draw "PSRAM unavailable"
    // instead of a black screen; every other page is unaffected.
    //
    //
    // Both conditions are required: PSRAM is where the tiles go, eMMC is where they come from.
    //
    g_sView.bReady = psram_is_ready() && AssetStoreIsReady();

    if ( !g_sView.bReady )
    {
        am_util_debug_printf("[map] tiles unavailable (psram %d, store %d)\n",
                             (int)psram_is_ready(), (int)AssetStoreIsReady());
    }
    else
    {
        am_util_debug_printf("[map] world %ux%u, %u tiles, %u slots"
                             " (%u KB each)\n",
                             MAP_WORLD_W, MAP_WORLD_H, MAP_TILE_COUNT,
                             MAP_CACHE_SLOTS, MAP_TILE_BYTES / 1024);

        //
        // Preload the area around the start, so the first visit to the map page does not wait.
        //
        map_cache_prepare(g_sView.fCamX);
    }

    ui32LastTick = xTaskGetTickCount();

    while (1)
    {
        uint32_t ui32Now;
        float    fDt;

        vTaskDelay(pdMS_TO_TICKS(MAP_TICK_MS));

        ui32Now      = xTaskGetTickCount();
        fDt          = (float)(ui32Now - ui32LastTick) / (float)configTICK_RATE_HZ;
        ui32LastTick = ui32Now;

        if ( !g_bActive )
        {
            //
            // Not on the map page: the simulation pauses. The cache contents are
            // kept, so switching back needs no reload.
            //
            continue;
        }

        taskENTER_CRITICAL();
        map_nav_step(fDt);
        map_camera_update();
        taskEXIT_CRITICAL();

        //
        // The two functions above only raise flags; the log is printed here (see where the flags are declared).
        //
        if ( g_bLogArrived )
        {
            g_bLogArrived = false;
            am_util_debug_printf("[map] arrived at %s\n", MAP_ROUTE_DEST_NAME);
        }
        if ( g_bLogFollow )
        {
            g_bLogFollow = false;
            am_util_debug_printf("[map] back to follow\n");
        }

        if ( g_sView.bReady )
        {
            map_cache_prepare(g_sView.fCamX);
        }
    }
}

//*****************************************************************************
//
// Get the map state for this frame.
//
//*****************************************************************************
void
MapTaskGetView(map_view_t *psView)
{
    taskENTER_CRITICAL();
    *psView = g_sView;
    taskEXIT_CRITICAL();
}

//*****************************************************************************
//
// Get a tile's address in PSRAM.
//
//*****************************************************************************
uint32_t
MapTaskGetTileAddr(uint32_t ui32TileIdx)
{
    uint32_t i;

    if ( (!g_sView.bReady) || (ui32TileIdx >= MAP_TILE_COUNT) )
    {
        return 0;
    }

    for ( i = 0; i < MAP_CACHE_SLOTS; i++ )
    {
        if ( g_sSlots[i].i32TileIdx == (int32_t)ui32TileIdx )
        {
            //
            // The LRU touch. The drawing side asks once per frame for the tile it
            // is drawing, so "recently used" needs no separate notification.
            //
            g_sSlots[i].ui32LastUse = ++g_ui32UseCounter;
            return MAP_SLOT_ADDR(i);
        }
    }

    //
    // Not loaded yet. Return 0 so the drawing side skips this block -- it will
    // arrive on the next map_cache_prepare(), usually the next frame.
    //
    return 0;
}

//*****************************************************************************
//
// Drag the map.
//
//*****************************************************************************
void
MapTaskDrag(float fDeltaX)
{
    taskENTER_CRITICAL();

    //
    // Dragging right moves the map right, which moves the camera left -- hence the subtraction.
    //
    // The division by fZoom converts screen displacement into world displacement:
    // at 2x, 100 px of finger travel should scroll only 50 px of world, or the
    // map flies away faster the further it is zoomed in and becomes uncontrollable.
    //
    g_sView.fCamX -= fDeltaX / g_sView.fZoom;

    map_camera_clamp_x(&g_sView.fCamX);

    g_sView.eMode      = MAP_MODE_FREE;
    g_ui32LastDragTick = xTaskGetTickCount();

    taskEXIT_CRITICAL();
}

//*****************************************************************************
//
// Zoom the map about an anchor.
//
//*****************************************************************************
void
MapTaskZoom(float fScale, float fAnchorX, float fAnchorY)
{
    float fNewZoom;
    float fWorldAnchorX;
    float fWorldAnchorY;

    if ( fScale <= 0.0f )
    {
        return;
    }

    taskENTER_CRITICAL();

    //
    // Which world point is currently under the anchor. After the zoom, the same point must return to the same screen position.
    //
    fWorldAnchorX = g_sView.fCamX + (fAnchorX / g_sView.fZoom);
    fWorldAnchorY = g_sView.fCamY + (fAnchorY / g_sView.fZoom);

    fNewZoom = g_sView.fZoom * fScale;

    if ( fNewZoom < MAP_ZOOM_MIN )
    {
        fNewZoom = MAP_ZOOM_MIN;
    }
    if ( fNewZoom > MAP_ZOOM_MAX )
    {
        fNewZoom = MAP_ZOOM_MAX;
    }

    g_sView.fZoom = fNewZoom;

    //
    // Solve for the new camera position: world = cam + screen/zoom, so cam = world - screen/zoom.
    //
    g_sView.fCamX = fWorldAnchorX - (fAnchorX / fNewZoom);
    g_sView.fCamY = fWorldAnchorY - (fAnchorY / fNewZoom);

    //
    // The clamp does not require the vehicle to be in the safe band: the user is
    // choosing what to look at, and pulling the camera back to the vehicle now
    // would be grabbing the wheel. map_camera_update() handles it on the return
    // to follow mode.
    //
    map_camera_clamp_x(&g_sView.fCamX);
    map_camera_clamp_y(&g_sView.fCamY, false);

    g_sView.eMode      = MAP_MODE_FREE;
    g_ui32LastDragTick = xTaskGetTickCount();

    taskEXIT_CRITICAL();
}

//*****************************************************************************
//
// Enter and leave the map page.
//
//*****************************************************************************
void
MapTaskSetActive(bool bActive)
{
    if ( bActive == g_bActive )
    {
        return;
    }

    g_bActive = bActive;

    if ( bActive )
    {
        am_util_debug_printf("[map] active (cache hit %u / miss %u)\n",
                             g_ui32Hits, g_ui32Misses);
    }
}

//*****************************************************************************
//
// Stop / resume the navigation.
//
//*****************************************************************************
void
MapTaskTogglePause(void)
{
    bool bPaused;

    //
    // After arrival this action is meaningless: the vehicle is already at the
    // destination and "resume" has nowhere to go. The caller takes
    // MapTaskRestart() in that case.
    //
    if ( g_sView.bArrived )
    {
        return;
    }

    taskENTER_CRITICAL();
    g_sView.bPaused = !g_sView.bPaused;
    bPaused         = g_sView.bPaused;
    taskEXIT_CRITICAL();

    am_util_debug_printf("[map] %s\n", bPaused ? "paused" : "resumed");
}

//*****************************************************************************
//
// Restart the navigation.
//
//*****************************************************************************
void
MapTaskRestart(void)
{
    taskENTER_CRITICAL();
    map_nav_reset();
    taskEXIT_CRITICAL();

    am_util_debug_printf("[map] restart from %s\n", MAP_ROUTE_ORIGIN_NAME);
}
