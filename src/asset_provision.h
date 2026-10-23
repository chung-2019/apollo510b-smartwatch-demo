//*****************************************************************************
//
//! @file asset_provision.h
//!
//! @brief Provision the built-in assets onto eMMC on first boot.
//!
//! eMMC ships blank, and the assets are no longer on the firmware's drawing path
//! -- with nobody putting the files there, boot ends in a screen with no images.
//! This module performs factory provisioning: it checks whether each asset file
//! is present and writes the copy built into the firmware when it is not.
//!
//! This is a transitional step. Once the USB MSC path works, the real flow is
//! for the PC to copy files straight onto eMMC; WATCHFACE_ASSET_PROVISION then
//! gets turned off and the 2.16 MB of built-in const arrays disappear from the
//! firmware entirely -- which is the whole point of moving the assets to eMMC.
//!
//! @note With this option enabled, MRAM usage is the same as before the move
//!       (the images exist in both places). Its value is that every stage can be
//!       flashed and verified on its own, instead of finding out what broke only
//!       after everything has changed at once.
//
//*****************************************************************************

#ifndef ASSET_PROVISION_H
#define ASSET_PROVISION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
//! @brief Whether to compile the built-in assets into the firmware and provision them at boot.
//!
//! @details Set to 0 and the firmware contains no images at all; the assets then
//! depend entirely on files already present on eMMC. Before switching, confirm
//! the eMMC content is complete (the boot log lists the check result per file).
//
//*****************************************************************************
#ifndef WATCHFACE_ASSET_PROVISION
#define WATCHFACE_ASSET_PROVISION   (1)
#endif

//*****************************************************************************
//
//! @brief Unconditionally overwrite assets that already exist on eMMC.
//!
//! @details Normally 0: provisioning only fills in files that are missing or the
//! wrong length, and leaves existing ones completely alone -- artwork the user
//! replaced must not be overwritten by the boot path.
//!
//! That, however, makes swapping artwork impossible: old and new images usually
//! have the same dimensions (map tiles are all 468x468 RGB565), so provisioning
//! sees a matching length, skips the file, and the new image is never written.
//!
//! So when swapping artwork, set this to 1, flash once, and boot writes the
//! firmware's copy unconditionally; then set it back to 0. It only has meaning
//! with WATCHFACE_ASSET_PROVISION=1 -- without built-in assets there is nothing
//! to overwrite with.
//!
//! @note Do not leave it at 1. Every boot would then rewrite 2 MB to eMMC, which
//!       both slows the boot and burns write cycles for nothing.
//
//*****************************************************************************
#ifndef WATCHFACE_ASSET_FORCE_WRITE
#define WATCHFACE_ASSET_FORCE_WRITE (0)
#endif

//*****************************************************************************
//
//! @brief Verify and complete the assets on eMMC.
//!
//! @details Checks each asset file for presence and correct length; only missing
//! ones are written and existing ones are left completely alone -- artwork the
//! user replaced over USB should not be overwritten by the boot path.
//!
//! Must be called after AssetStoreInit() succeeds and before any asset is loaded.
//!
//! With WATCHFACE_ASSET_PROVISION disabled this function only prints the check
//! results and writes nothing.
//!
//! @return The number of assets missing or failed to write; 0 means all ready.
//
//*****************************************************************************
extern uint32_t AssetProvisionRun(void);

#ifdef __cplusplus
}
#endif

#endif // ASSET_PROVISION_H
