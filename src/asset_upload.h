//*****************************************************************************
//
//! @file asset_upload.h
//!
//! @brief Write asset files from the PC into eMMC over RTT.
//!
//! Swapping artwork used to need three firmware flashes (seeded build ->
//! verify -> slimmed build, see docs/EMMC_ASSET_STORE.md). This path lets the
//! PC push files straight into eMMC with no firmware rebuild at all.
//!
//! Why RTT and not USB: the J-Link connection to the board is already there (the
//! log goes over RTT), the PC side needs no new driver or library, and the
//! firmware does not have to fit a USB stack into a project that already carries
//! BLE and FreeRTOS. The price is speed -- RTT throughput is far below USB and
//! 2 MB takes somewhere over ten seconds, but swapping artwork was never a
//! high-frequency operation.
//!
//! **Up and down are separate buffers**, so the log (target -> host) and the
//! file data (host -> target) can share channel 0 without opening another
//! channel; the RTT telnet port of JLinkGDBServer happens to work both ways.
//!
//! The matching PC-side tool is tools/asset_upload.py.
//
//*****************************************************************************

#ifndef ASSET_UPLOAD_H
#define ASSET_UPLOAD_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
//! @brief Whether to compile in the RTT upload feature.
//!
//! @details Production firmware can turn it off -- it lets anyone who can attach
//! SWD rewrite the contents of eMMC. With it off the task is not created and the
//! associated code is not linked in.
//
//*****************************************************************************
#ifndef WATCHFACE_ASSET_UPLOAD
#define WATCHFACE_ASSET_UPLOAD  (1)
#endif

//*****************************************************************************
//
//! @brief Transfer protocol.
//!
//! @details One file at a time. The host sends:
//!
//! @code
//!     magic    4 bytes   "ASU1"
//!     namelen  1 byte    Name length (1..12, 8.3 format)
//!     name     namelen   File name, without the trailing NUL
//!     size     4 bytes   File size in bytes, little-endian
//!     data     size      File contents
//!     sum      4 bytes   Unsigned sum of every data byte, little-endian
//! @endcode
//!
//! Target responses all go through the existing log (`[upload] ...`). The host
//! sees them on the upstream side of the same RTT connection, so no separate
//! reply format is needed.
//!
//! ## Flow control (required for large files)
//!
//! The target prints one line for every 1 KB received:
//!
//! @code
//!     [upload] ack <bytes received so far>
//! @endcode
//!
//! The host's unacknowledged window must stay below the RTT down buffer (4 KB),
//! because **once that buffer is full J-Link discards the excess** rather than
//! stalling the host. Without this feedback the host sends at its own pace, this
//! lowest-priority task cannot keep up, and everything past the first 4 KB
//! vanishes -- the symptom is `abort <name>: timeout` on every file over 4 KB.
//!
//! The host-side window is WINDOW in tools/asset_upload.py; older firmware sends
//! no ack, and the host falls back to timed sending when none arrives.
//!
//! Why a magic: the RTT down buffer is shared and the user may be typing in a
//! terminal. The magic is what identifies "this is an upload" among the noise;
//! a byte that does not match is dropped and the search continues, so terminal
//! input is never written into a file.
//!
//! Why a sum and not CRC32: the failure modes on this path are "transfer
//! interrupted" and "misalignment", both of which a sum catches. The random bit
//! flips that genuinely need a CRC essentially do not happen over SWD and are
//! not worth an extra table.
//
//*****************************************************************************
#define ASSET_UPLOAD_MAGIC      "ASU1"
#define ASSET_UPLOAD_MAGIC_LEN  (4)
#define ASSET_UPLOAD_NAME_MAX   (12)

//*****************************************************************************
//
//! @brief Command that lists the eMMC contents.
//!
//! @details The host sends these four bytes and the target prints the root
//! directory listing and the free space to the log:
//!
//! @code
//!     [list] begin
//!     [list] HOME.BIN 412232
//!     ...
//!     [list] end 8 files, 7654321 KB free of 7812500 KB
//! @endcode
//!
//! A second magic rather than a command code inside the upload protocol: the two
//! have completely different lengths (a command is 4 bytes, an upload has a
//! header and a payload), and one shared format would only add branches to the
//! state machine.
//!
//! The first three characters match the upload magic ("ASU1" vs "ASL1"), so they
//! only diverge at the 4th -- the state machine has to track both at once, see
//! upload_feed().
//
//*****************************************************************************
#define ASSET_LIST_MAGIC        "ASL1"

//*****************************************************************************
//
//! @brief Command that deletes one asset file.
//!
//! @details The format is magic + namelen(1) + name:
//!
//! @code
//!     "ASD1" 0x07 "OLD.BIN"
//! @endcode
//!
//! The target reports the result to the log once the delete is done:
//!
//! @code
//!     [delete] ok OLD.BIN
//!     [delete] fail OLD.BIN
//! @endcode
//!
//! There is no checksum and no confirmation step -- the command is barely a
//! dozen bytes, and a corrupted one yields a name matching no file, so the
//! result is a failure rather than the wrong deletion. The real protection is
//! the name itself: it must match exactly, there are no wildcards, so exactly
//! one named file can be removed at a time.
//!
//! Why this command exists: after artwork has been swapped, eMMC often keeps a
//! batch of old files the firmware no longer recognizes. They do not affect
//! operation, they only take space and make the [list] output hard to read.
//! Before this, the only way to clear them was reformatting the whole eMMC.
//!
//! Only the 4th character separates it from the other two magics ("ASU1" /
//! "ASL1" / "ASD1"), so the state machine tracks all three at once, see upload_feed().
//
//*****************************************************************************
#define ASSET_DEL_MAGIC         "ASD1"

//*****************************************************************************
//
//! @brief The upload task itself.
//!
//! @details Polls the RTT down buffer and writes a file to eMMC once it is
//! complete. Lowest priority: it must not affect drawing, and since the host
//! waits for the target to drain before sending more, being slow only means a
//! longer transfer.
//!
//! @param pvParameters Unused.
//!
//! @return Does not return.
//
//*****************************************************************************
extern void AssetUploadTask(void *pvParameters);

extern TaskHandle_t AssetUploadTaskHandle;

//*****************************************************************************
//
//! @brief Whether a file is currently being received.
//!
//! @details For the drawing side to show an indication -- something on screen
//! should explain what is happening during an upload, or the user just sees the
//! map suddenly missing a piece (the tile being overwritten).
//!
//! @return true while an upload is in progress.
//
//*****************************************************************************
extern bool AssetUploadIsActive(void);

#ifdef __cplusplus
}
#endif

#endif // ASSET_UPLOAD_H
