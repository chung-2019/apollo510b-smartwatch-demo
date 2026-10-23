//*****************************************************************************
//
//! @file render_task.h
//!
//! @brief Functions and variables related to the render task.
//!
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

#ifndef RENDER_TASK_H
#define RENDER_TASK_H

//*****************************************************************************
//
// render task handle.
//
//*****************************************************************************
extern TaskHandle_t RenderTaskHandle;

//*****************************************************************************
//
// External variable definitions
//
//*****************************************************************************
extern img_obj_t g_sFrameBuffer;
//*****************************************************************************
//
// External function definitions.
//
//*****************************************************************************
extern void RenderTask(void *pvParameters);

//*****************************************************************************
//
//! @brief Request a return to the home page.
//!
//! @details Used by Button1 (SW3). Works on every page -- pressing it anywhere
//! returns home, which is what a Home key should do.
//!
//! It exists because of the map page: horizontal swipes there pan the map
//! rather than change pages, and the only way out is to drag the map to the
//! edge of the world, which takes five or six swipes from the middle of a
//! route. The button is the escape route that always works.
//!
//! Safe to call from interrupt context: it only raises a flag, and the actual
//! page change is performed by RenderTask before it begins the next frame
//! (page state and overlays belong to that task; writing them from an ISR
//! would be an unsynchronized cross-context write).
//!
//! Calling it with touch disabled (WATCHFACE_ENABLE_TOUCH = 0) is safe but has
//! no effect -- that configuration has no page switching to begin with.
//
//*****************************************************************************
extern void RenderTaskGoHome(void);

//*****************************************************************************
//
//! @brief Toggle the page overview (Button0 / SW1).
//!
//! @details The current page shrinks to a thumbnail at the centre of the screen
//! with a row of page dots below it. Swiping left and right then moves between
//! pages quickly; a tap, or another press of this button, zooms back to full screen.
//!
//! Available on every page and particularly needed on the map page, where
//! horizontal swipes pan the map instead of changing pages -- only the overview
//! makes swiping between pages possible there.
//!
//! There is no "cancel" semantics: page changes made inside the overview take
//! effect immediately, and closing it merely zooms back in on whichever page you are on.
//!
//! Safe to call from interrupt context, for the same reason as RenderTaskGoHome():
//! it only raises a flag, and RenderTask opens or closes the overview before it
//! begins the next frame.
//!
//! Calling it with touch disabled (WATCHFACE_ENABLE_TOUCH = 0) is safe but has
//! no effect. The same holds when the thumbnail buffer fails to allocate, which
//! prints an explanatory line at boot.
//
//*****************************************************************************
extern void RenderTaskTogglePageOverview(void);

#endif // RENDER_TASK_H
