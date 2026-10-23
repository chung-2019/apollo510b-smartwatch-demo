//*****************************************************************************
//
//! @file map_task.h
//!
//! @brief Map tile cache and navigation simulation.
//!
//! Corresponds to the MapLoaderTask / MapCacheTask / NavigationTask roles in the
//! specification document. They are merged into one task here because their
//! cadence is in fact identical -- every position update determines which part
//! of the map is needed, and splitting that across three tasks would only add
//! two queues and their synchronization.
//!
//! The data path:
//!
//!   MRAM (const tile arrays) -> PSRAM tile cache -> GPU sampling -> framebuffer
//!
//! The first leg is the copy this file performs; the second is RenderTask
//! handing the PSRAM address straight to nema_bind_src_tex(). The cache in
//! between exists for capacity, not speed: the GPU cannot sample MRAM, the
//! three tiles total 1.25 MB, and the SSRAM drawing pool cannot hold that on top
//! of the framebuffer and the existing pages' assets -- which is exactly why the
//! specification puts the tiles in PSRAM.
//!
//! A real product would replace the MRAM layer with eMMC (8 GB on board, over
//! SDIO0). Only map_tile_load_from_store() changes for that; the cache and the
//! eviction logic stay as they are -- which is the point of isolating the
//! storage layer.
//
//*****************************************************************************

#ifndef MAP_TASK_H
#define MAP_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"

//
// Takes FB_RESX / FB_RESY: the number of cache slots is derived from the window
// size (see MAP_CACHE_SLOTS), and the window is the screen.
//
#include "nemagfx_watchface.h"

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
//! @brief Tile and world map dimensions.
//!
//! @details Three 468x468 tiles side by side form a 1404x468 world. The tile
//! edge matching the screen edge is deliberate: the screen can span at most two
//! tiles at once, which keeps the cache small.
//!
//! The world height equals the screen height exactly, so the map can only pan
//! horizontally. That is not a limitation but the shape of this artwork -- the
//! route is a band running from south-east to north-west, and the source image
//! was landscape to begin with.
//
//*****************************************************************************
#define MAP_TILE_W              (468)
#define MAP_TILE_H              (468)
#define MAP_TILE_COUNT          (3)
#define MAP_WORLD_W             (MAP_TILE_W * MAP_TILE_COUNT)    // 1404
#define MAP_WORLD_H             (MAP_TILE_H)                     // 468

//*****************************************************************************
//
//! @brief Zoom range.
//!
//! @details 1.0 is the tile's native resolution, one screen pixel per tile pixel.
//!
//! Upper bound 2.5: beyond that it only stretches RGB565 pixels and street names
//! blur into colour blocks. At 2.5x one junction fills about half the screen,
//! which is already the limit of this artwork.
//!
//! Lower bound 0.6: the world is a flat 1404x468 strip while the screen is a
//! 468x468 square, so zooming out runs off the map vertically first -- at 0.6x
//! the map is only 281 px tall, leaving a 93 px band of background above and
//! below. Any smaller and it is no longer "an edge strip" but "the map floating
//! in the middle". Horizontally it is not the constraint: a 0.6x window covers
//! 780 px of world width, well short of the full 1404.
//
//*****************************************************************************
//
// The lower bound is a fraction rather than 0.6f, because MAP_CACHE_SLOTS uses
// it to size an array.
//
// An expression containing a float is not an integer constant expression in C,
// and GCC then treats the whole array as a VLA ("variably modified at file
// scope") -- the value folds correctly, but that is compiler tolerance rather
// than a language guarantee. A numerator and denominator keep it integral.
//
#define MAP_ZOOM_MIN_NUM        (3)
#define MAP_ZOOM_MIN_DEN        (5)

#define MAP_ZOOM_MIN            ((float)MAP_ZOOM_MIN_NUM / (float)MAP_ZOOM_MIN_DEN)
#define MAP_ZOOM_MAX            (2.5f)

//*****************************************************************************
//
//! @brief Number of tile cache slots in PSRAM.
//!
//! @details Derived from "how many tiles the screen can need at once", not from a guessed number:
//!
//!   world width covered by the window = FB_RESX / MAP_ZOOM_MIN = 468 / 0.6 = 780 px
//!   tiles spanned                     = ceil(780 / 468) + 1 = 3
//!
//! The final +1 is because the left edge of the window does not land on a tile
//! boundary, so the same width spans one more tile in the worst position.
//! Writing it as an expression means changing MAP_ZOOM_MIN updates this
//! automatically -- forgetting to would show up as the screen edge flickering
//! with missing blocks when zoomed out, hard to connect back to the cache.
//!
//! Before pinch zoom was added this was 2 (the window width equalled the tile
//! width, so at most two were spanned) and the eviction logic genuinely ran.
//! With 3 tiles in 3 slots everything is now resident and LRU never triggers --
//! it stays for the day there are more tiles, since a real offline map (the
//! specification estimates 5 to 6 GB) can never be resident.
//
//*****************************************************************************
#define MAP_VIEW_W_MAX          ((FB_RESX * MAP_ZOOM_MIN_DEN) / MAP_ZOOM_MIN_NUM)
#define MAP_CACHE_SLOTS         (((MAP_VIEW_W_MAX + MAP_TILE_W - 1) / MAP_TILE_W) + 1)

//*****************************************************************************
//
//! @brief Vehicle speed and time scale of the navigation simulation.
//!
//! @details The speed is the number shown to the user (the HUD km/h and ETA are
//! both derived from it); the time scale only spares the demo a ten-minute wait.
//! Driving this 6.5 km route at 40 km/h takes 587 seconds; at 20x that is about
//! 29 seconds for the whole thing -- long enough to watch the navigation through
//! and short enough not to be a wait.
//!
//! Set the scale to 1 for a real-time simulation.
//
//*****************************************************************************
#define MAP_SIM_SPEED_KMH       (40.0f)
#define MAP_SIM_TIME_SCALE      (20.0f)

//*****************************************************************************
//
//! @brief Map interaction mode.
//!
//! @details The only difference is whether the camera follows the vehicle. One
//! touch enters FREE, and after a while with no further touch it returns to
//! FOLLOW -- the same behaviour as phone navigation apps: looking ahead at a
//! junction does not fight a camera pulling back, and the user still never has
//! to press "return to current position".
//
//*****************************************************************************
typedef enum
{
    MAP_MODE_FOLLOW = 0,    // The camera follows the vehicle
    MAP_MODE_FREE           // The user is dragging, or just released and not yet timed out
} map_mode_e;

//*****************************************************************************
//
//! @brief The next manoeuvre.
//
//*****************************************************************************
typedef enum
{
    MAP_TURN_STRAIGHT = 0,
    MAP_TURN_LEFT,
    MAP_TURN_RIGHT,
    MAP_TURN_ARRIVE
} map_turn_e;

//*****************************************************************************
//
//! @brief Snapshot of the map state RenderTask takes each frame.
//!
//! @details Taken as one piece rather than through a pile of getters: these
//! fields are related (the vehicle position and the camera position must be from
//! the same instant, or the vehicle jitters on screen), and reading them
//! separately could mix values from two different updates.
//
//*****************************************************************************
typedef struct
{
    //
    // Whether the tiles are usable. False when PSRAM init failed, and the map
    // page then shows the reason instead of a black screen.
    //
    bool        bReady;

    //
    // World coordinates of the window's top-left corner.
    //
    // fCamY may be negative -- zoomed out, the world is shorter than the window,
    // so pushing up or down exposes the area beyond the map at the screen edge.
    // It is still worth pushing because the route's y spans 105 to 340 and both
    // ends sit right under the two HUD panels; without it, the vehicle at the
    // start and at the destination would be hidden behind them.
    //
    float       fCamX;
    float       fCamY;

    //
    // Zoom factor, MAP_ZOOM_MIN..MAP_ZOOM_MAX; 1.0 is the tile's native resolution.
    //
    // The world-to-screen conversion rests entirely on these three fields:
    //
    //     screen = (world - cam) * zoom
    //     world  = cam + screen / zoom
    //
    // The window therefore covers FB_RESX/zoom x FB_RESY/zoom of the world, and
    // the smaller the zoom the wider the view. Route coordinates, tile indices
    // and the cache all still work in world coordinates; only the final blit and
    // the vehicle drawing convert to screen coordinates.
    //
    float       fZoom;

    //
    // Current position and heading (degrees, 0 = right / east, clockwise positive).
    //
    float       fCarX;
    float       fCarY;
    float       fHeading;

    map_mode_e  eMode;

    //
    // Whether the user has stopped the navigation.
    //
    // Different from MapTaskSetActive(false) (switching away): that means "not
    // looking at this page" and resumes automatically on return, while this is an
    // explicit stop that survives leaving and returning and needs another tap to
    // resume. Both freeze the vehicle, but they mean different things, so they
    // are recorded separately.
    //
    bool        bPaused;

    //
    // Remaining distance (metres) and estimated seconds to arrival. Both are
    // real-world numbers derived from MAP_SIM_SPEED_KMH and unaffected by
    // MAP_SIM_TIME_SCALE -- what is sped up is the simulation, not the road.
    //
    uint32_t    ui32RemainM;
    uint32_t    ui32EtaSec;

    //
    // Direction of and distance to the next turn.
    //
    map_turn_e  eNextTurn;
    uint32_t    ui32NextTurnM;

    //
    // Overall progress 0..100, for the progress bar at the bottom.
    //
    uint32_t    ui32Progress;

    bool        bArrived;
} map_view_t;

//*****************************************************************************
//
//! @brief The map task itself.
//!
//! @param pvParameters Unused.
//!
//! @return Does not return.
//
//*****************************************************************************
extern void MapTask(void *pvParameters);

extern TaskHandle_t MapTaskHandle;

//*****************************************************************************
//
//! @brief Get the map state for this frame.
//!
//! @details Called once per frame by RenderTask. The contents are the snapshot
//! the task maintains; scheduling is briefly suspended during the read to keep
//! the whole set consistent (see map_view_t).
//!
//! @param psView Receives the result, must not be NULL.
//
//*****************************************************************************
extern void MapTaskGetView(map_view_t *psView);

//*****************************************************************************
//
//! @brief Get the PSRAM address of one tile.
//!
//! @details This is all the drawing side needs: hand the return value to
//! nema_bind_src_tex() and the GPU samples straight from PSRAM. A return of 0
//! means the tile is not in the cache yet and the caller should skip that block
//! rather than draw random contents.
//!
//! Calling this also marks the tile as recently used so eviction will not pick
//! it -- the LRU touch lives at the point of use rather than in a separate API,
//! so it cannot be forgotten.
//!
//! @param ui32TileIdx Tile index, 0..MAP_TILE_COUNT-1.
//!
//! @return The physical address in PSRAM, or 0 when not loaded.
//
//*****************************************************************************
extern uint32_t MapTaskGetTileAddr(uint32_t ui32TileIdx);

//*****************************************************************************
//
//! @brief Drag the map with a finger.
//!
//! @details Called by RenderTask while the finger moves; the argument is this
//! frame's horizontal displacement (screen pixels, positive to the right). The
//! map moves with the finger, which means the camera moves the other way.
//!
//! Calling this switches the mode to MAP_MODE_FREE and restarts the timer back to follow mode.
//!
//! @param fDeltaX This frame's horizontal displacement.
//
//*****************************************************************************
extern void MapTaskDrag(float fDeltaX);

//*****************************************************************************
//
//! @brief Zoom the map about a point on screen.
//!
//! @details Called by RenderTask on every frame of a pinch; the argument is this
//! frame's relative scale (the ratio of the finger distance, above 1 meaning
//! spreading apart, i.e. zooming in).
//!
//! The anchor is the midpoint between the fingers, in screen coordinates. The
//! map content under that point stays put -- whichever junction the fingers are
//! on stays between them, the only natural behaviour for a pinch. Anchoring on
//! the screen centre or on the vehicle makes the map feel like it is running away.
//!
//! The factor is clamped to MAP_ZOOM_MIN..MAP_ZOOM_MAX and the camera is
//! re-clamped into the world, so the caller need not check any bounds.
//!
//! Like MapTaskDrag() it switches the mode to MAP_MODE_FREE: after a pinch the
//! user is looking at a chosen area and should not be yanked back to the vehicle.
//!
//! @param fScale   This frame's relative scale (new distance / old distance).
//! @param fAnchorX Anchor x in screen coordinates.
//! @param fAnchorY Anchor y in screen coordinates.
//
//*****************************************************************************
extern void MapTaskZoom(float fScale, float fAnchorX, float fAnchorY);

//*****************************************************************************
//
//! @brief Enter or leave the map page.
//!
//! @details On leaving, the navigation simulation pauses in place and resumes on
//! return. The simulation therefore never quietly finishes while another page is
//! showing -- that would look like a broken program.
//!
//! @param bActive true while the map page is on screen.
//
//*****************************************************************************
extern void MapTaskSetActive(bool bActive);

//*****************************************************************************
//
//! @brief Stop or resume the navigation.
//!
//! @details A tap anywhere on the map page toggles this. While stopped the
//! vehicle stays put and the map can still be dragged around -- stopping is
//! usually exactly for getting a clear look at the road ahead.
//!
//! Calling it after arrival does nothing: a tap then means restart (see MapTaskRestart()).
//
//*****************************************************************************
extern void MapTaskTogglePause(void);

//*****************************************************************************
//
//! @brief Restart the navigation.
//!
//! @details The vehicle returns to Taipei 101, the camera returns to follow mode
//! and the stopped flag is cleared. A tap after reaching the destination takes
//! this path.
//
//*****************************************************************************
extern void MapTaskRestart(void);

#ifdef __cplusplus
}
#endif

#endif // MAP_TASK_H
