//*****************************************************************************
//
//! @file asset_store.h
//!
//! @brief Storage layer for the graphics assets: a FAT filesystem on eMMC.
//!
//! The 8 GB eMMC on the board (IS21EF08G, over SDIO0) is formatted as FAT with
//! one file per asset. At boot the firmware reads what it needs into SSRAM or
//! PSRAM, and the GPU samples from there.
//!
//! Why the assets moved out of the firmware: the home image, the watch faces,
//! the hands and the three map tiles used to be const arrays in MRAM totalling
//! 2.16 MB -- over half of the 4 MB of MRAM spent on pictures. On eMMC that
//! space all returns to code, and swapping an image needs no reflash.
//!
//! The data path therefore becomes:
//!
//!     eMMC (FAT files) -> SSRAM / PSRAM -> GPU sampling -> framebuffer
//!
//! The GPU cannot sample eMMC directly (it is not in the address space at all),
//! so the copy in the middle is a matter of feasibility, not efficiency.
//!
//! @note Asset files use 8.3 names. FatFs does not enable long file names by
//!       default, and doing so costs a conversion table and a unicode table;
//!       these names are an internal convention and need not read nicely.
//
//*****************************************************************************

#ifndef ASSET_STORE_H
#define ASSET_STORE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
//! @brief Asset file names.
//!
//! @details Centralized here rather than scattered across call sites: the
//! PC-side generator (tools/asset_export.py) emits exactly these names, and the
//! two sides have to agree. Rename here and in that script together.
//
//*****************************************************************************
#define ASSET_HOME          "HOME.BIN"      // 454x454 RGB565
#define ASSET_WATCH_BG      "WATCHBG.BIN"   // 454x454 RGB565
#define ASSET_HAND_HOUR     "HANDHR.BIN"    // 15x90   RGBA8888
#define ASSET_HAND_MIN      "HANDMIN.BIN"   // 15x150  RGBA8888
#define ASSET_HAND_SEC      "HANDSEC.BIN"   // 13x179  RGBA8888

//
// Second watch face (deep blue dial). The artwork comes from 0.png and 1..3.rgba.h in src/generated.
//
// It differs from the first face by more than the background: the dial is
// 390x390 rather than 454x454, and each of the three hands has its own size and
// pivot. So ASSET_WATCH_BG_* cannot be shared; the drawing side works from the
// description in watchface_t (see render_task.c).
//
// The same directory also holds a w0..w3.png set of similar size and appearance,
// but **do not use it**: those hands are a different design (the pivot is at the
// centre of the counterweight at the tail), and on this dial the hour hand would
// grow out of the central knob. The matching set is 1..3.rgba.h; see
// tools/face2_prep.py.
//
// **This face is not currently built into the firmware**: its slot went to the
// Ambiq face (ASSET_FACE4_*), because the GPU pool cannot hold a fourth face
// (see VMEM_SIZE in gcc/Makefile). The files are still on eMMC and these macros
// remain; switching back needs only a change to g_sFaces[] in render_task.c.
//
#define ASSET_FACE2_BG      "WF2BG.BIN"     // 390x390 RGB565
#define ASSET_FACE2_HOUR    "WF2HR.BIN"     // 22x104  RGBA8888
#define ASSET_FACE2_MIN     "WF2MIN.BIN"    // 28x153  RGBA8888
#define ASSET_FACE2_SEC     "WF2SEC.BIN"    // 18x219  RGBA8888

//
// Third watch face (blue numeral dial).
//
// The hands share their artwork with the second face but **are separate assets**:
// those three were drawn for a 390 px dial, while this face's dial is 454, so
// tools/face3_prep.py multiplies by 454/390 on the PC before export. Baking the
// upscale into the asset instead of letting the GPU scale every frame is
// sharper (LANCZOS over bilinear) and spares the drawing side an extra factor.
//
#define ASSET_FACE3_BG      "WF3BG.BIN"     // 454x454 RGB565
#define ASSET_FACE3_HOUR    "WF3HR.BIN"     // 23x107  RGBA8888 (shortened to clear the numerals)
#define ASSET_FACE3_MIN     "WF3MIN.BIN"    // 33x178  RGBA8888
#define ASSET_FACE3_SEC     "WF3SEC.BIN"    // 18x213  RGBA8888 (shortened to clear the numerals)

//
// Ambiq Apollo510B face. The artwork comes from src/generated/Ambiq_Watch_face/
// and the whole set is converted by tools/face4_prep.py. It **replaces the
// second face (aurora)** rather than adding a fourth -- the pool cannot hold
// four (see VMEM_SIZE in gcc/Makefile).
//
// The FACE4 / WF4 naming is kept instead of renumbering to FACE2: these numbers
// mean "which asset set" and not "which slot in the carousel", and renaming
// would force every generated .BIN and every tool to follow.
//
// This face has one extra asset: a **centre cap**. Earlier faces did not need
// one, and the reason is in the design -- the cap in the reference composite is
// painted above the hands, to cover the hubs of all three. The firmware draws
// dial then hands, so a cap painted on the dial is simply covered by the hands.
// Leaving it out costs more than decoration: the hour and minute hubs are rings,
// and three stacked turn into a cross at the centre that also buries the cyan ring.
//
// The dial cannot use the source image from Ambiq_Watch_face/ as-is: a vertical
// hand ghost was left uncleaned in the middle, and the outer circle's centre is
// 5.6 px from the knob (centring must follow the knob, since the hands rotate
// about the asset centre). All three hands have their tails cropped to 21 px --
// an isolated render shows the whole hand, while in the design the section near
// the pivot is hidden by the cap; without the crop the minute hand would stick
// out 12 px beyond it.
//
#define ASSET_FACE4_BG      "WF4BG.BIN"     // 454x454 RGB565 (centred, ghost repaired)
#define ASSET_FACE4_HOUR    "WF4HR.BIN"     // 30x130  RGBA8888 (tail cropped)
#define ASSET_FACE4_MIN     "WF4MIN.BIN"    // 30x193  RGBA8888 (tail cropped)
#define ASSET_FACE4_SEC     "WF4SEC.BIN"    // 18x126  RGBA8888
#define ASSET_FACE4_CAP     "WF4CAP.BIN"    // 46x46   RGBA8888 (centre cap)

#define ASSET_MAP_TILE_FMT  "MAPTILE%u.BIN" // 468x468 RGB565, %u is the tile index

//
// The weather page, in two layers (see draw_weather() in render_task.c):
//
//   background  five plain skies (src/Generate_3/weather_cloud_0N.png); the
//               drawing side pans them slowly left and right and moves on to
//               the next one only after each full sweep
//   overlay     date, temperature, forecast cards and the sun's glow: one
//               transparent RGBA image that never moves. Drawn by
//               tools/weather_prep.py, and only the part with content is stored
//               (not the whole 468x468)
//
// The time and the Bluetooth symbol are not in the assets; the drawing side
// draws them every frame.
//
// The early WEATHER0..4.BIN (the overlay baked into the sky) are no longer used
// and can be deleted from eMMC. The names were deliberately not reused: had they
// been, the old composites would be read in as backgrounds until the new files
// were uploaded, and the overlay would appear twice.
//
#define ASSET_WEATHER_BG_FMT "WTHRBG%u.BIN"  // 468x468 RGB565, %u is the background index 0..4
#define ASSET_WEATHER_COUNT  (5)
#define ASSET_WEATHER_UI     "WTHRUI.BIN"    // ASSET_WEATHER_UI_W x _H RGBA8888

//*****************************************************************************
//
//! @brief Asset dimensions.
//!
//! @details With the assets moved from the firmware to eMMC there is nowhere
//! else to look the sizes up -- macros like TEST_IMAGE_W in the generated
//! headers used to serve, but those headers are exactly what is being removed.
//! The files hold raw pixel data with no header, so the reader has to know how
//! much to read.
//!
//! This is the single source: the drawing side builds its img_obj_t from these
//! values, and the PC-side tools/asset_export.py exports at the same dimensions.
//! A mismatch shows up as a "short read" in the boot log, not as half an image.
//
//*****************************************************************************
#define ASSET_HOME_W        (454)
#define ASSET_HOME_H        (454)

#define ASSET_WATCH_BG_W    (454)
#define ASSET_WATCH_BG_H    (454)

#define ASSET_HAND_HOUR_W   (15)
#define ASSET_HAND_HOUR_H   (90)

#define ASSET_HAND_MIN_W    (15)
#define ASSET_HAND_MIN_H    (150)

#define ASSET_HAND_SEC_W    (13)
#define ASSET_HAND_SEC_H    (179)

//
// Second watch face. The 390x390 dial is smaller than the panel (468) and the
// drawing side always scales it to fill, so these sizes only serve to compute
// the file size and build the img_obj_t; they are not the on-screen size.
//
#define ASSET_FACE2_BG_W    (390)
#define ASSET_FACE2_BG_H    (390)

#define ASSET_FACE2_HOUR_W  (22)
#define ASSET_FACE2_HOUR_H  (104)

#define ASSET_FACE2_MIN_W   (28)
#define ASSET_FACE2_MIN_H   (153)

#define ASSET_FACE2_SEC_W   (18)
#define ASSET_FACE2_SEC_H   (219)

//
// Third watch face. The 454x454 dial is within 3% of the panel's 468, so the drawing side blits it 1:1 and centred.
//
// The hands are the second face's three multiplied by 454/390 (see
// tools/face3_prep.py). The numbers look arbitrary because they are computed,
// not hand-picked -- change the factor and that script has to be re-run and
// these values updated.
//
#define ASSET_FACE3_BG_W    (454)
#define ASSET_FACE3_BG_H    (454)

#define ASSET_FACE3_HOUR_W  (23)
#define ASSET_FACE3_HOUR_H  (107)

#define ASSET_FACE3_MIN_W   (33)
#define ASSET_FACE3_MIN_H   (178)

#define ASSET_FACE3_SEC_W   (18)
#define ASSET_FACE3_SEC_H   (213)

//
// Ambiq face. The 454x454 dial is within 3% of the panel (468), so bFit is false and a 1:1 blit is sharpest.
//
// These sizes are the output of tools/face4_prep.py, not the dimensions of the
// source PNGs: the hour and minute hands have had their tails cropped
// (132 -> 130, 205 -> 193). Changing TAIL_MAX in that tool means changing these
// and TARGETS in tools/png_to_bin.py too; a mismatch between the three shows up
// as a short read, with that hand not drawn at all.
//
#define ASSET_FACE4_BG_W    (454)
#define ASSET_FACE4_BG_H    (454)

#define ASSET_FACE4_HOUR_W  (30)
#define ASSET_FACE4_HOUR_H  (130)

#define ASSET_FACE4_MIN_W   (30)
#define ASSET_FACE4_MIN_H   (193)

#define ASSET_FACE4_SEC_W   (18)
#define ASSET_FACE4_SEC_H   (126)

#define ASSET_FACE4_CAP_W   (46)
#define ASSET_FACE4_CAP_H   (46)

#define ASSET_MAP_TILE_W    (468)
#define ASSET_MAP_TILE_H    (468)

#define ASSET_WEATHER_W     (468)
#define ASSET_WEATHER_H     (468)

//
// The weather overlay stores only the part with content: both sides are largely
// transparent, and storing the whole frame would double what is read from PSRAM
// every frame. These sizes must match UI_W/UI_H in tools/weather_prep.py and
// TARGETS in tools/png_to_bin.py; where it lands on screen is WEATHER_UI_X/Y in
// render_task.c.
//
#define ASSET_WEATHER_UI_W  (348)
#define ASSET_WEATHER_UI_H  (392)

//*****************************************************************************
//
//! @brief Mount the filesystem on eMMC.
//!
//! @details Three steps in order: SDIO0 pin and eMMC initialization (handled by
//! the FatFs mmc_apollo5 driver), mounting the FAT, and confirming the asset
//! directory exists.
//!
//! Must be called before the FreeRTOS scheduler starts, for the same reason as
//! psram_init(): eMMC init resets the SDIO controller repeatedly and must not
//! interleave with anyone else's accesses.
//!
//! A failure is reported but does not hang the boot. The screen has no images,
//! but vector-drawn content such as the watch hands and the digits on the
//! temperature page is entirely unaffected -- a deliberate line: with eMMC
//! unmountable, the watch is still a watch that tells the time.
//!
//! @return true when the filesystem is mounted.
//
//*****************************************************************************
extern bool AssetStoreInit(void);

//*****************************************************************************
//
//! @brief Create the filesystem mutex.
//!
//! @details FatFs does no locking of its own (FF_FS_REENTRANT = 0), while
//! MapTask, RenderTask and the upload task all come in, so every access goes
//! through this one lock.
//!
//! Must be called after the scheduler has started and before any task that uses
//! assets is created -- currently setup_task() in rtos.c. It cannot go into
//! AssetStoreInit(): creating a mutex before the scheduler starts masks
//! interrupts until the scheduler starts, which would affect the boot
//! provisioning and psram_init() that follow (see g_xLock in asset_store.c).
//!
//! Does nothing when the filesystem is not mounted; calling it twice is harmless.
//
//*****************************************************************************
extern void AssetStoreLockInit(void);

//*****************************************************************************
//
//! @brief Whether the filesystem is usable.
//!
//! @return true when AssetStoreInit() has succeeded.
//
//*****************************************************************************
extern bool AssetStoreIsReady(void);

//*****************************************************************************
//
//! @brief Get the stage and error code of the last initialization failure.
//!
//! @details For display. The boot-phase log is often lost entirely when the
//! PSRAM timing scan floods the RTT buffer, while the screen is not -- when the
//! map page is unavailable these two values are printed straight onto it,
//! instead of gambling on the log being visible.
//!
//! @param ppcStage Receives the stage name ("MOUNT" / "MKFS" / "REMOUNT"), an
//!                 empty string on success. May be NULL.
//!
//! @return The FatFs FRESULT value, 0 when there has been no failure.
//
//*****************************************************************************
extern int AssetStoreGetFailure(const char **ppcStage);

//*****************************************************************************
//
//! @brief Read one whole asset file into the given memory.
//!
//! @details The caller allocates the space and knows how much it needs -- asset
//! sizes are fixed at compile time (the w/h/format of img_obj_t), and deriving
//! them from the file length is more likely to disagree.
//!
//! A file larger than ui32Size reads only the first ui32Size bytes and reports
//! the length mismatch; a smaller one is treated as corrupt and fails outright
//! -- half an image on screen is harder to notice than a missing one.
//!
//! @param pcName   Asset file name (one of the ASSET_* macros).
//! @param pvBuf    Destination, in SSRAM or PSRAM.
//! @param ui32Size Expected byte count.
//!
//! @return true when all ui32Size bytes were read.
//
//*****************************************************************************
extern bool AssetStoreRead(const char *pcName, void *pvBuf, uint32_t ui32Size);

//*****************************************************************************
//
//! @brief Write a block of data as an asset file (overwriting).
//!
//! @details Used only by first-boot provisioning: when eMMC has no assets yet,
//! the copy built into the firmware is written out (see asset_provision.c). Once
//! the USB MSC path works, the real flow is the PC copying files directly and
//! this function is left with the factory-provisioning role alone.
//!
//! @param pcName   Asset file name.
//! @param pvBuf    Source data.
//! @param ui32Size Byte count.
//!
//! @return true when everything was written.
//
//*****************************************************************************
extern bool AssetStoreWrite(const char *pcName, const void *pvBuf,
                            uint32_t ui32Size);

//*****************************************************************************
//
//! @brief Whether an asset file exists with the correct length.
//!
//! @param pcName   Asset file name.
//! @param ui32Size Expected byte count.
//!
//! @return true when the file exists and the length matches exactly.
//
//*****************************************************************************
extern bool AssetStoreExists(const char *pcName, uint32_t ui32Size);

//*****************************************************************************
//
//! @brief Delete one asset file.
//!
//! @details For the PC-side maintenance command (see ASSET_DEL_MAGIC in
//! asset_upload.h): after artwork has been swapped, eMMC often keeps old files
//! the firmware no longer recognizes. They cause no errors, they only take space
//! and make the listing hard to read.
//!
//! The firmware never deletes anything on its own -- the assets were put there
//! by the user, and only the user knows when they are no longer wanted.
//!
//! @param pcName Asset file name.
//!
//! @return true when the file is gone. Returns false when it did not exist in
//!         the first place (FatFs returns FR_NO_FILE), which callers may ignore.
//
//*****************************************************************************
extern bool AssetStoreDelete(const char *pcName);

//*****************************************************************************
//
//! @brief List the contents of the eMMC root directory.
//!
//! @details Walks the directory once, calling the callback for each file. A
//! callback rather than a returned array, because callers want different things:
//! the upload tool prints it to the log, while an on-screen file browser would
//! want to keep it.
//!
//! It also reports the total and free disk space -- "what is on the eMMC" is
//! usually asked together with "how much room is left".
//!
//! @param pfnEntry   Called once per file with the name and the byte count. May be NULL.
//! @param pui32Total Receives the total capacity in bytes. May be NULL.
//! @param pui32Free  Receives the free space in bytes. May be NULL.
//!
//! @return The file count, or -1 on failure.
//
//*****************************************************************************
typedef void (*asset_list_cb_t)(const char *pcName, uint32_t ui32Bytes);

extern int32_t AssetStoreList(asset_list_cb_t pfnEntry,
                              uint32_t *pui32Total, uint32_t *pui32Free);

//*****************************************************************************
//
//! @brief Streaming write: open the file.
//!
//! @details For RTT upload (see asset_upload.h). Assets run to 400 KB and cannot
//! be collected in RAM and written in one go, hence the begin/chunk/end split.
//!
//! **On success it holds the filesystem lock until AssetStoreWriteEnd() /
//! Abort()** -- with FF_FS_REENTRANT=0, FatFs is not thread-safe, not even with
//! separate FILs (the FATFS object holds a shared sector buffer). So for the
//! whole duration of the transfer, everyone else's reads are blocked.
//!
//! The practical effect: during those ten-odd seconds the map tiles cannot be
//! swapped in and the screen may be missing a block. That is a deliberate
//! trade-off -- an upload is something the user started, and they are not
//! looking at the map at the same time.
//!
//! @param pcName Asset file name.
//!
//! @return true when the file is open; the caller must then call End or Abort.
//
//*****************************************************************************
extern bool AssetStoreWriteBegin(const char *pcName);

//*****************************************************************************
//
//! @brief Streaming write: write one chunk.
//!
//! @param pvBuf    Data.
//! @param ui32Size Byte count.
//!
//! @return true when everything was written.
//
//*****************************************************************************
extern bool AssetStoreWriteChunk(const void *pvBuf, uint32_t ui32Size);

//*****************************************************************************
//
//! @brief Streaming write: close the file and release the lock.
//!
//! @return true when f_close succeeded (directory entry and FAT both written back).
//
//*****************************************************************************
extern bool AssetStoreWriteEnd(void);

//*****************************************************************************
//
//! @brief Streaming write: abandon and release the lock.
//!
//! @details Called when a transfer is interrupted. The file keeps whatever was
//! written, at the wrong length -- the provisioning check at the next boot
//! (AssetStoreExists) sees the mismatch and treats it as missing, so half a file
//! is never taken for a valid asset.
//
//*****************************************************************************
extern void AssetStoreWriteAbort(void);

#ifdef __cplusplus
}
#endif

#endif // ASSET_STORE_H
