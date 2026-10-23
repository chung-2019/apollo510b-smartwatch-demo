//*****************************************************************************
//
//! @file render_task.c
//!
//! @brief NemaGFX watch face rendering task
//!
//! This module is the FreeRTOS task that does the drawing: the home image and
//! the watch face (background, hour / minute / second hands, weather icon), and
//! it switches between pages on touch swipes. Rendering is paired with GPU power
//! gating to save energy, and frame buffer use is coordinated with the display
//! task through semaphores.
//!
//! @section render_task_features Main features
//!
//! - Real-time rotation of the hands
//! - Home image, watch face background and weather icon rendering
//! - GPU power management (woken for drawing, back to deepsleep afterwards)
//! - Frame buffer allocation and management
//! - Texture loading into SSRAM for the GPU to sample
//! - Page changes on horizontal touch swipes
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


//
// sqrtf(): pinch zoom needs the distance between the two fingers.
//
#include <math.h>

//*****************************************************************************
//
// Project common headers.
//
//*****************************************************************************
#include "nemagfx_watchface.h"
#include "render_task.h"
#include "backlight.h"
#include "ble_diag.h"
#include "ble_pair.h"
#include "display_task.h"
#include "rtos.h"
#include "touch_task.h"
#include "tmp119.h"
#include "sensor_task.h"
#include "led_task.h"
#include "map_task.h"
#include "asset_store.h"
#include "psram.h"

//
// Everything on UI_PAGE_DESIGN comes from the "Generate C" output of
// tools/nemagfx_ui_designer, in src/generated/ui_designer. Those three .c files
// are overwritten wholesale on every regeneration, so this file only calls their
// API and never edits them; application logic is attached by overriding weak
// callbacks (see ui_event_button_clicked() at the end of this file).
//
#include "ui_designer_generated.h"

//
// WATCHFACE_ENABLE_TOUCH is defined in touch_task.h (included above): with touch
// split into its own task, rtos.c has to see it too in order to decide whether
// to create TouchTask.
//

//*****************************************************************************
//
// WATCHFACE_SHOW_WEATHER_ICON
//
// 1 = draw the TSVG weather icon (blowing / climate / day), rotating once a second, in the lower middle of the face
// 0 = do not draw it (the default for the Generate_3 dial)
//
// Turned off by default after the switch to the Generate_3 dial, because the
// positions clash: the icon is fixed at (ICON_POS_X, ICON_POS_Y) = (184, 318),
// a 100x100 area at the bottom centre, which is exactly where the new dial's
// inner tick ring and 6 o'clock marker sit -- and the second hand sweeps across
// it twice a minute.
//
// With this at 0 the IconTimer is not even created, so the timer service task need not wake once a second.
// Setting this line back to 1 restores it all: the drawing, the timer and icon_change() are still here.
//
//*****************************************************************************
#define WATCHFACE_SHOW_WEATHER_ICON     0

//*****************************************************************************
//
// WATCHFACE_DESIGN_FONT_PROBE
//
// 1 = the first time page 1 is drawn, measure a few strings with nema_vg_string_get_bbox() and print the results
// 0 = do not measure (default)
//
// Purpose: calibrating the string width estimate of the UI Designer canvas
// preview (font_metrics.py in tools/nemagfx_ui_designer).
//
// The calibration has been done and the conclusions recorded here, for re-verification after a font or SDK change:
//
//   size=28  "A"     -> w=25.54  (xAdvance of "A" = 1401)
//   size=28  "test"  -> w=71.70  (advance total 3933)
//   size=28  any string -> h=43.46  (the font's size field = 2384)
//
//   All three independent expressions give the same denominator,
//   1536 = units_per_em(2048) * 3/4, i.e. the 72/96 relationship between point
//   and pixel at 96 DPI -- NemaVG treats the font size as points. This matches
//   the manual, where nema_vg_get_ascender_pt() returns "point units".
//
//   It also confirms that the bbox width is the sum of advances rather than the
//   actual ink extent, and that kerning is not fully reflected in the bbox.
//
// The measurement runs once and does not affect frame timing. It lives in the
// drawing path rather than in init: init happens within a second of boot, and in
// NO_BLOCK_TRIM mode RTT discards everything before the host connects, so those
// lines would be long gone by the time a viewer attaches.
//
// Output format (width and height in 1/100 pixel, since am_util_stdio_printf has no %f):
//
//   [render] font probe: size=28.00 "Ambiq_EdgePilot" w=311.71 h=43.46
//
//*****************************************************************************
#define WATCHFACE_DESIGN_FONT_PROBE     0

//*****************************************************************************
//
// Image assets.
//
// The home image and the watch face assets live on the eMMC FAT filesystem and
// are read into the SSRAM graphics pool by render_assets_load() at boot for the
// GPU to sample. Names and sizes are in asset_store.h; the firmware no longer
// contains these images (swapping artwork needs no reflash).
//
// Note: the GPU on this platform cannot sample MRAM, and certainly not eMMC, so
// textures must always be copied into SSRAM first (map tiles go to PSRAM, see
// map_task.h). Binding a const array's address straight to the GPU (which works
// on Apollo4P) produces a flickering all-red screen here.
//
// The watch face (page 2) uses the Watch_face asset set: black background, white
// outer ring, numerals 1-12, with a red hour / white minute / grey second hand,
// taken from watchface_test3 of apollo4p_evb_disp_shield_rev2, with the rotation
// centres as verified by that project. The dial is the de-glowed version
// (deglow2.py crushes the background inside the dial below tick brightness to 0,
// leaving ticks, numerals and the outer ring untouched).
//
// This set has no hub cap; each of the three hands draws its own small ring at
// its tail as the pivot, so the Generate_3 hub is no longer drawn.
//
//*****************************************************************************

#include "blowing.h"
#include "climate.h"
#include "day.h"

//
// The temperature page's font.
//
// Switched to a NemaVG vector font (DejaVuSans converted from TTF) in place of
// the DejaVuSerif24pt4b bitmap font. The reason is that this page needs three
// sizes -- large temperature digits, a six-digit comparison code and small
// status text -- and a bitmap font binds one size each, so three would mean
// three bitmaps; that one font alone took 358 KB of MRAM plus the same again in
// the SSRAM assets pool.
// The vector font is a single 168 KB const blob, the size is chosen by
// nema_vg_set_font_size(), and the GPU fills the paths directly, so even at
// 100 px there are no jagged edges.
//
// This font's glyph range is 0x20 to 0x7E and has no degree sign. The degree
// symbol is drawn as a small ring instead (see draw_temp_value()), which aligns
// better than the glyph would and avoids extending the range for one character.
//
// The bitmap font was used only by this page, so the whole thing left the build
// when it was replaced (DejaVuSerif24pt4b.c was dropped from SRC in the Makefile too).
//
#include "font/DejaVuSans_ttf_kern.h"

//*****************************************************************************
//
// Rotation pivot positions of the hands.
//
// Meaning of the coordinates (see draw_hand()): the offset of the asset's
// top-left corner relative to the rotation centre, so the centre lies at
// (-PIVOT_X, -PIVOT_Y) in asset coordinates. Negative values put the pivot below
// and to the right of the asset origin.
//
// The hour and minute hands use the SDK assets, and the values here are in the
// asset's original coordinate system; draw_hand() applies HAND_*_SCALE, so no
// manual conversion is needed.
//
//*****************************************************************************

//
// The pivots of the three hands, each the centre of the small ring at the tail, in asset coordinates.
//
// The values come from watchface_test3/watch2.c (verified on hardware). They were
// cross-checked with a flood fill for the centroid of the hole inside each ring,
// giving (7.0, 81.5) / (7.4, 140.4) / (6.5, 151.0) -- at most 1 px from the values
// below, invisible when rotating, so the already-verified set is used.
//
#define PIVOT_HOUR_X        (-8)
#define PIVOT_HOUR_Y        (-82)     // asset 15x90

#define PIVOT_MIN_X         (-8)
#define PIVOT_MIN_Y         (-140)    // asset 15x150

#define PIVOT_SEC_X         (-7)
#define PIVOT_SEC_Y         (-150)    // asset 13x179, with 29 px of tail below the pivot

//
// The three hands of the second watch face (deep blue dial).
//
// These values come straight from the example of the same name in
// boards/apollo510dL_evb -- that program uses exactly this dial and these three
// hands, so it is a verified combination rather than something derived here.
//
// Note the hour hand: the asset is only 104 px tall while the pivot is at 140.
// That is, **the pivot lies outside the asset**, the whole hand is above it and
// 36 px are empty in between. This is not a typo but the dial's design -- there
// is a knob about 70 px across at the centre, and all three hands start from its
// outer edge.
//
// This used to hold the pivots of a different set (w1..w3.png), whose pivots are
// at the centre of the counterweight at the tail. Mixing the two made the hour
// hand grow out of the knob, at the wrong length too.
//
#define PIVOT_F2_HOUR_X     (-11)
#define PIVOT_F2_HOUR_Y     (-140)    // asset 22x104, pivot 36 px below the asset

#define PIVOT_F2_MIN_X      (-14)
#define PIVOT_F2_MIN_Y      (-139)    // asset 28x153, 14 px of tail below the pivot

#define PIVOT_F2_SEC_X      (-9)
#define PIVOT_F2_SEC_Y      (-148)    // asset 18x219, 71 px of tail below the pivot

//
// The three hands of the third watch face (blue numeral dial).
//
// The artwork shares its origin with the second face, but the assets are their
// own: tools/face3_prep.py has already multiplied by 454/390 on the PC, so these
// pivots are the scaled values and the drawing side needs no further factor.
//
// Changing HAND_SCALE in that tool means re-running it and copying these three
// sets of numbers over (it prints them), along with ASSET_FACE3_*_W/H in
// asset_store.h -- a mismatch between the three shows up as hands circling an
// offset point, or a short read at boot with the hand not drawn at all.
//
#define PIVOT_F3_HOUR_X     (-11)
#define PIVOT_F3_HOUR_Y     (-144)    // asset 23x107, tip clears the numerals, tail meets the knob

#define PIVOT_F3_MIN_X      (-16)
#define PIVOT_F3_MIN_Y      (-162)    // asset 33x178

#define PIVOT_F3_SEC_X      (-9)
#define PIVOT_F3_SEC_Y      (-144)    // asset 18x213, 69 px of tail below the pivot

//
// The three hands of the Ambiq Apollo510B face.
//
// The pivots were measured differently from the earlier faces, because this set
// offers a more direct basis: the hubs of the hour and minute hands are rings
// with a transparent hole, so the transparent region enclosed by opaque pixels
// and not connected to the image border has a centroid that is the pivot. That
// measured hour (14.0, 109.0) and minute (14.5, 172.5).
//
// x takes the canvas centre (15) rather than the measured 14.0 / 14.5: the hands
// are left-right symmetric and the packing tool centres them in the canvas, so
// the axis is the canvas's vertical midline. The hole is only 37 to 44 pixels,
// and a 1 px error in its centroid is less reliable than that reasoning.
//
// The second hand has no ring: it is a constant-width rod with no shaping at the tail, and the tail end is the pivot (125).
//
// All three tails are 21 px, exactly the radius of the centre cap (see psCap) --
// not a coincidence; TAIL_MAX in tools/face4_prep.py is set from the cap radius.
//
#define PIVOT_F4_HOUR_X     (-15)
#define PIVOT_F4_HOUR_Y     (-109)    // asset 30x130, 21 px tail

#define PIVOT_F4_MIN_X      (-15)
#define PIVOT_F4_MIN_Y      (-172)    // asset 30x193, 21 px tail

#define PIVOT_F4_SEC_X      (-9)
#define PIVOT_F4_SEC_Y      (-125)    // asset 18x126, 1 px tail

//*****************************************************************************
//
// Size difference between the face assets and the panel.
//
// This asset set was drawn for a 454 px round panel while this board has 468 px,
// a 3% difference. The dial is either blitted 1:1 with a black border, or scaled
// to fill -- WATCH_DIAL_FIT chooses.
//
// The scaled version originally used point sampling (0 as the last argument to
// nema_bind_src_tex()), and since 468/454 is not an integer ratio, the thin white
// outer ring duplicated a row every few tens of pixels and looked jagged. Scaling
// requires bilinear (NEMA_FILTER_BL): at 3%, bilinear only softens the white ring
// slightly, which is far better than a staircase.
//
// The default is 1:1: no resampling at all, so the white ring looks exactly as
// drawn and is at its sharpest. The cost is a 7 px black border around the panel
// -- the outer area of this dial is black anyway and it is mostly invisible on
// hardware, but set WATCH_DIAL_FIT to 1 for the scale-to-fill version if it bothers.
//
// Why not scale the asset to 468 on the PC before converting it to a header: that
// would cost 25 KB more and still resample once, so the white ring would blur anyway.
//
//*****************************************************************************
//
// 0 = blit the dial 1:1 and centred (default); 1 = scale to fill the whole panel.
//
// This only affects the first face: the choice goes into the bFit field of
// g_sFaces[]. The second face's dial is 360 px and would leave a large gap
// unless filled, so its bFit is hard-coded to true.
//
// The hands must take the same factor as the dial, or they shrink relative to it
// and no longer line up with the ticks. That factor now comes from
// face_fit_scale() out of the dial's actual size, so the two cannot diverge.
//
#define WATCH_DIAL_FIT      (0)

//*****************************************************************************
//
// The watch face's starting time.
//
// The sec that draw_watch() receives is "seconds since boot", and without this
// offset all three hands would overlap pointing at 12 at the moment of boot,
// looking like initialization had not finished. With three hours added, the boot
// screen reads 3:00:00 -- hour at 3, minute and second at 12, obviously a running watch.
//
// This is still only a time relative to boot, not the real clock; once an RTC is
// attached this should read the current time from it and the constant can go.
//
//*****************************************************************************
#define WATCH_START_HOUR    (3)
#define WATCH_START_SEC     ((float)(WATCH_START_HOUR) * 3600.0f)

//*****************************************************************************
//
// Position and size of the weather icon.
//
//*****************************************************************************
#define ICON_WIDTH  (100)
#define ICON_HEIGHT (100)

#define ICON_POS_X  ((FB_RESX - ICON_WIDTH) / 2)
#define ICON_POS_Y  (FB_RESY - ICON_HEIGHT - 50)

#ifdef GPU_FORMAT_RGB888
#define FB_COLOR_FORMAT (NEMA_RGB24)
#else
#define FB_COLOR_FORMAT (NEMA_RGB565)
#endif

//*****************************************************************************
//
// The gesture thresholds and gesture_t have moved to touch_task.h.
//
// Touch is no longer polled by this file every frame; TouchTask samples on the
// GP98 interrupt and hands over a finished gesture (see the header of
// touch_task.h). The threshold constants moved with the classification logic;
// what this file still uses are CC_PULL_START_MAX_Y / BL_PULL_START_MIN_Y,
// derived from the edge bands, which ui_input_update() uses to separate an
// ordinary vertical swipe from pulling an overlay in from a screen edge.
//
//*****************************************************************************


//*****************************************************************************
//
// Temperature page configuration.
//
// The temperature is sampled on a fixed period by SensorTask and placed in a
// cache (see sensor_task.c); this file only draws the cached value and performs
// no I2C access.
//
//*****************************************************************************

//
// Colours are always built with TEMP_ABGR(). This page's palette is taken from
// the design as #RRGGBB, and once written as a 32-bit ABGR constant there is no
// telling which colour it was, making it easy to swap R and B when revising
// (CC_ABGR() exists for the same reason, for the control centre).
//
#define TEMP_ABGR(a, r, g, b) ((uint32_t)(((uint32_t)(a) << 24) | \
                                          ((uint32_t)(b) << 16) | \
                                          ((uint32_t)(g) <<  8) | \
                                           (uint32_t)(r)))

//
// Replace a colour's alpha and keep the RGB. The two haloes during pairing are
// the accent colour at different alphas -- copying the RGB again would mean a
// later change to the accent missed the haloes and left two different colours.
//
#define TEMP_WITH_ALPHA(c, a) ((uint32_t)((((uint32_t)(c)) & 0x00ffffffU) | \
                                          ((uint32_t)(a) << 24)))

//
// The temperature page palette.
//
// The design is a dark round face with the accent running from cyan (12 o'clock)
// to blue at the end of the arc, and status text in two tiers, "cyan label /
// white body". Red and green appear only for pairing results; everything else is
// cyan to blue.
//
#define TEMP_BG_COLOR        TEMP_ABGR(0xff, 0x03, 0x05, 0x08) // near-black background
#define TEMP_VALUE_COLOR     TEMP_ABGR(0xff, 0xff, 0xff, 0xff) // large digits: white
#define TEMP_UNIT_COLOR      TEMP_ABGR(0xff, 0xe4, 0xed, 0xf4) // unit: a cool white
#define TEMP_ERROR_COLOR     TEMP_ABGR(0xff, 0xff, 0x50, 0x50) // sensor fault: red

#define TEMP_ACCENT_COLOR    TEMP_ABGR(0xff, 0x22, 0xe0, 0xf0) // accent: cyan
#define TEMP_ACCENT_END      TEMP_ABGR(0xff, 0x0a, 0x5c, 0xff) // end of the arc: blue
#define TEMP_TRACK_COLOR     TEMP_ABGR(0xff, 0x10, 0x3a, 0x46) // the unfilled track
#define TEMP_RIM_COLOR       TEMP_ABGR(0xff, 0x2a, 0x2e, 0x7a) // outermost hairline: indigo
#define TEMP_TICK_COLOR      TEMP_ABGR(0xff, 0x2e, 0x9a, 0xaa) // ticks: dark cyan

#define TEMP_IDLE_COLOR      TEMP_ABGR(0xff, 0x8a, 0x9a, 0xb8) // not connected: grey-blue
#define TEMP_PAIR_PASS_COLOR TEMP_ABGR(0xff, 0x22, 0xdd, 0x55) // pairing succeeded: green
#define TEMP_PAIR_FAIL_COLOR TEMP_ABGR(0xff, 0xff, 0x44, 0x44) // pairing failed: red

//*****************************************************************************
//
// The temperature page's gauge (circular arc dial).
//
// The centre is the panel centre. Three layers, outside in:
//
//   1. TEMP_RIM_*   the outermost hairline; it only frames the screen and carries no value.
//   2. TEMP_ARC_*   the gauge itself. The whole circle is the track (dark),
//                   filled clockwise from 12 o'clock to the angle of the current
//                   temperature, in a cyan-to-blue gradient.
//   3. TEMP_TICK_*  12 short ticks at fixed positions that do not follow the value.
//
// Angle convention (see nema_vg_draw_ring()): 0 degrees is at 3 o'clock and
// angles increase clockwise (screen y runs down, so 90 degrees is 6 o'clock).
// 12 o'clock is therefore 270 degrees.
//
//*****************************************************************************
#define TEMP_DIAL_CX        (FB_RESX / 2.0f)            // 234
#define TEMP_DIAL_CY        (FB_RESY / 2.0f)            // 234

#define TEMP_RIM_R          (224.0f)
#define TEMP_RIM_W          (2.0f)

#define TEMP_ARC_R          (206.0f)                    // radius of the arc centreline
#define TEMP_ARC_W          (10.0f)

#define TEMP_TICK_COUNT     (12)
#define TEMP_TICK_R_OUT     (192.0f)
#define TEMP_TICK_R_IN      (176.0f)
#define TEMP_TICK_W         (3.0f)

#define TEMP_ARC_START_DEG  (270.0f)                    // 12 o'clock
#define TEMP_ARC_SWEEP_DEG  (360.0f)                    // the whole circle is the range

//
// How the gradient is done: the filled arc is cut into TEMP_ARC_SEGMENTS pieces,
// each drawn in one interpolated solid colour. NemaVG does have a conical
// gradient (nema_vg_paint_set_grad_conical(), used by the
// nemagfx_vg_circular_bar example), but that needs an extra gradient object and a
// set of stops, with the stop angles aligned to the ring's centre to avoid skew;
// segment interpolation has none of those prerequisites, and with enough segments
// the seams are invisible.
//
// 18 segments means 20 degrees each: at TEMP_ARC_R = 206 the chord of one segment
// is about 72 px, and the colour difference between neighbours is 1/18 of the
// gradient, which the eye cannot resolve as a step.
//
#define TEMP_ARC_SEGMENTS   (18)

//
// The range. Below MIN the arc is empty; above MAX it fills the whole circle.
//
// 20 to 45 degrees rather than the sensor's full range (the TMP119 covers -55 to
// 150): this span covers room temperature to body temperature, so the movement is
// visible. On the design, 36.8 degrees fills about a third of the circle, which
// treats the range as 0 to 100; change these two values to 0 and 100 to match the
// design exactly, and nothing else needs touching.
//
#define TEMP_GAUGE_MIN_C    (20.0f)
#define TEMP_GAUGE_MAX_C    (45.0f)

//*****************************************************************************
//
// Font sizes and layout of the temperature page.
//
// The layout from top to bottom is "Bluetooth icon -> large temperature digits ->
// status area", with the status area's contents following the BLE state (see
// temp_ble_view_e). Every row is given an absolute centre y rather than being
// derived from the row above: the five states have different numbers of rows, and
// relative offsets would couple them all together.
//
// How wide a row can be on a round panel varies with y: the nearer the top or
// bottom, the shorter the chord. So each row's width is not checked by a comment
// here but computed by temp_row_max_w() from that row's y, and draw_temp_line()
// shrinks the text automatically when the measured width exceeds it. The reason
// is that this page's font sizes and string lengths are still changing (the
// design's English wording, the number of digits in the code), and hand
// arithmetic in comments would eventually miss a row -- "CONFIRM THE CODE"
// overflowed exactly that way once.
//
//*****************************************************************************
//
// Font sizes.
//
// What the size given to nema_vg_set_font_size() means in actual glyph height is
// not stated in the documentation and is not the em either: this font's size
// field is 2384 against units_per_em of 2048, and measurement shows the glyphs
// come out considerably larger than an em-based estimate. So this page does not
// rely on the size to "come out right".
//
// The values below are all "how large it should be". The size actually drawn may
// be smaller: every row measures its text width and shrinks proportionally when
// it exceeds the width available inside the circle (temp_row_max_w()) -- status
// text through draw_temp_line(), the large digits through draw_temp_value().
//
// So raising a value stops having any effect past a point (it is clamped by the
// chord). Going genuinely larger means relaxing TEMP_TEXT_SAFE_R, which then
// encroaches on the gauge arc and the ticks.
//
// Note that the clamp only governs width, not height: whether two rows collide
// has to be checked by hand with TEMP_ROW_HALF_H() for each row's top and bottom
// edges (the arithmetic for the three-row set is in the TEMP_ST3_* comments).
//
#define TEMP_VALUE_FONT     (76.0f)                     // large temperature digits
#define TEMP_UNIT_FONT      (26.0f)                     // "C"
#define TEMP_CODE_FONT      (32.0f)                     // six-digit comparison code
#define TEMP_LABEL_FONT     (26.0f)                     // small status text
#define TEMP_ERROR_FONT     (40.0f)                     // sensor fault message

//
// Radius of the area available to text.
//
// The inner edge of the gauge arc (TEMP_ARC_R - TEMP_ARC_W / 2 = 201) pulled in
// by 8 px, so the edge of the text does not touch the arc.
//
// It is not pulled in to the inner edge of the ticks (176): there are only 12
// ticks, each 3 px wide, at fixed angles, and the longest rows would at most
// graze a few pixels of the two at 60 and 120 degrees. Cutting 50 px off the
// available width for that would visibly shrink every row -- not worth it.
//
#define TEMP_TEXT_SAFE_R    (193.0f)

//
// Half-height of one text row, used to find where its edge furthest from the centre lies.
//
// 0.6 of the font size: working back from the large digits, the em is about 1.23
// times the size, and DejaVuSans has a cap height of about 0.72 em and a
// descender of about 0.24 em, together about 1.18 em, roughly 1.45 sizes, half of
// which is 0.73. 0.6 is used here because the status rows are all uppercase with
// no descenders, and 0.73 would make the available width too conservative.
//
#define TEMP_ROW_HALF_H(sz) ((sz) * 0.6f)

#define TEMP_VALUE_CY       (216.0f)                    // centre row of the large digits
#define TEMP_UNIT_GAP       (10.0f)                     // gap between the digits and "degC"
#define TEMP_DEG_R          (5.5f)                      // radius of the degree symbol's circle
#define TEMP_DEG_W          (3.0f)                      // line width of the degree symbol

//
// How far the degree symbol sits above the digits' centreline, and the offset of
// "C" from that line. Both scale with the font size, so they are written as
// fractions of the size rather than fixed pixels.
//
#define TEMP_DEG_RISE_RATIO (0.20f)
#define TEMP_UNIT_DROP_RATIO (0.02f)

//
// Status area: two rows (not connected / connected). Both strings are short (the
// longest, "DISCONNECTED", is about 211 px on row 368 against 244 px available),
// so neither is clamped by width and neither has to move up to fit.
//
//   large digits  216 +- 45.6 (@76)   ->        .. 261.6
//   first row     330 +- 15.6 (@26)   -> 314.4 .. 345.6    52.8 below the digits
//   second row    368 +- 15.6 (@26)   -> 352.4 .. 383.6    6.8 gap above
//
// The 52.8 px below the large digits looks loose, but it is deliberate: with only
// two rows, the design has this group low, and raising it would look like the
// three-row version missing a row rather than the same layout.
//
#define TEMP_ST2_L1_CY      (330.0f)
#define TEMP_ST2_L2_CY      (368.0f)

//
// Status area: three rows (pairing, with the six-digit comparison code in the middle).
//
// The last row is "CONFIRM THE CODE", 16 characters and the longest string on the
// page, and it falls in the band with the shortest chord: at y = 386 the row has
// only about 190 px available, and the clamp would squeeze the font to 18 px to
// fit -- far from the 26 px of the other states. At 366 about 248 px are
// available and it only shrinks to about 23 px. This row therefore moves no
// further down, which fixes the vertical space of the whole group.
//
// Top and bottom edges of the three rows (half-height from TEMP_ROW_HALF_H(), i.e. 0.6 of the font size):
//
//   large digits  216 +- 45.6 (@76)   ->        .. 261.6
//   first row     286 +- 15.6 (@26)   -> 270.4 .. 301.6    8.8 below the digits
//   code          327 +- 19.2 (@32)   -> 307.8 .. 346.2    6.2 gap above
//   third row     366 +- 13.8 (@23)   -> 352.2 .. 379.8    6.0 gap above
//
// The 23 on the third row is the result of the width clamp, not TEMP_LABEL_FONT's 26.
//
// The code's font size follows from this vertical space rather than being picked:
// there are only 50.6 px between the bottom of the first row and the top of the
// third (301.6 to 352.2), and with the centre at 327 and 6 px of margin each side
// the half-height is at most 19.2, i.e. a size of 32. The earlier 46 (half-height
// 27.6) worked out to -5 px on each side, i.e. overlapping its neighbours -- on
// hardware it looked "just touching".
//
#define TEMP_ST3_L1_CY      (286.0f)
#define TEMP_ST3_CODE_CY    (327.0f)
#define TEMP_ST3_L2_CY      (366.0f)

//
// Status area: a badge plus two rows (pairing succeeded / failed).
//
// The edges of each layer are worked out the same way. The badge's height is its
// diameter and does not go through TEMP_ROW_HALF_H():
//
//   large digits  216 +- 45.6 (@76)   ->        .. 261.6
//   badge         296 +- 24           -> 272.0 .. 320.0    10.4 below the digits
//   first row     342 +- 15.6 (@26)   -> 326.4 .. 357.6    6.4 gap above
//   second row    380 +- 15.6 (@26)   -> 364.4 .. 395.6    6.8 gap above
//
// The longest second row is "PAIRING ERROR" (13 characters); at row 380 about
// 211 px are available and the width clamp shrinks it to about 23.6 px
// (half-height 14.2, looser than the table above). "PAIRED" is short and is not
// clamped, so the table uses the unclamped 26 -- whichever of the two is less favourable.
//
#define TEMP_BADGE_CY       (296.0f)
#define TEMP_BADGE_R        (24.0f)
#define TEMP_BADGE_W        (4.0f)
#define TEMP_STB_L1_CY      (342.0f)
#define TEMP_STB_L2_CY      (380.0f)

//
// nema_vg_print() needs a text block, and vertical centring is relative to that
// block. Twice the font size is certainly tall enough for a single line
// (ascender + descender < 1.2 em), and the width is simply the whole screen so
// horizontal centring lands on the panel centre.
//
#define TEMP_TEXT_BOX_H(sz) ((sz) * 2.0f)

//*****************************************************************************
//
// The Bluetooth icon.
//
// Reuses the Lucide icon from the EdgePilot Launcher (a Qt project on the TI
// AM62P); assets/icons/bluetooth.svg holds nothing but one path:
//
//   viewBox="0 0 24 24" stroke-width="2" stroke-linecap="round"
//   stroke-linejoin="round" fill="none"
//   <path d="m7 7 10 10-5 5V2l5 5L7 17"/>
//
// Expanded into absolute coordinates it is a five-segment polyline:
//
//   (7,7) -> (17,17) -> (12,22) -> (12,2) -> (17,7) -> (7,17)
//
// So it needs neither a PNG texture nor a TSVG conversion: NemaVG can stroke this
// path directly, the const data is a few tens of bytes, and being vector, resizing
// is only a change to BT_ICON_SIZE.
//
// The coordinates are converted to pixels at compile time (BT_ICON_S), the path
// itself sits at the origin, and the position comes only from a path matrix
// translation. Matrix scaling is avoided because NemaVG's documentation does not
// state whether the stroke width scales with the matrix; computing both in pixels
// removes the dependency on that behaviour.
//
//*****************************************************************************
#define BT_ICON_SIZE        (60.0f)                     // icon edge length in pixels
#define BT_ICON_VIEWBOX     (24.0f)                     // viewBox edge of the original SVG
#define BT_ICON_S(v)        ((v) * (BT_ICON_SIZE / BT_ICON_VIEWBOX))
#define BT_ICON_STROKE_W    BT_ICON_S(2.0f)             // corresponds to stroke-width="2"

//
// Placed directly above the large digits, like a phone's status bar.
//
// Vertical headroom: the icon occupies y 84 to 144, the large digit row is
// centred at 216 at size 100, and the top of the glyphs is around 216 - 36 = 180
// (cap height about 0.72 em), leaving 36 px between them.
//
// Horizontally there is no need to inset for the round panel: FB_RESX = 468 (radius 234), and the chord at y = 84
// half-width is sqrt(234^2 - 150^2) ~= 180, so x 54 to 414 is visible while the icon spans only 204 to 264.
// The largest halo during pairing has radius 50, reaching 184 to 284, also within range.
//
#define BT_ICON_POS_X       ((FB_RESX - BT_ICON_SIZE) / 2.0f)
#define BT_ICON_POS_Y       (84.0f)

//
// Centre of the icon. The halo circles and the connected dot are placed relative to it.
//
#define BT_ICON_CX          (BT_ICON_POS_X + (BT_ICON_SIZE / 2.0f))
#define BT_ICON_CY          (BT_ICON_POS_Y + (BT_ICON_SIZE / 2.0f))

//
// The two haloes shown while pairing. The design has a glow fading outward; two
// semi-transparent rings approximate it here -- a true radial fade would need
// either a texture or a dozen circles of decreasing alpha, and at 60 px two
// rings are enough to read as "pairing in progress".
//
// The inner ring is brighter and thicker, the outer fainter and thinner, which reads as spreading outward.
//
#define BT_GLOW_IN_R        (38.0f)
#define BT_GLOW_IN_W        (3.0f)
#define BT_GLOW_IN_ALPHA    (0x88)

#define BT_GLOW_OUT_R       (50.0f)
#define BT_GLOW_OUT_W       (2.0f)
#define BT_GLOW_OUT_ALPHA   (0x40)

//
// The small solid dot to the right of the icon when connected, meaning the same
// as "device connected" in a phone's status bar. It sits close to the icon's top
// right so it does not overlap the polyline itself.
//
#define BT_DOT_DX           (34.0f)
#define BT_DOT_DY           (-8.0f)
#define BT_DOT_R            (5.0f)

//*****************************************************************************
//
// The home page control centre (pull-down menu).
//
// The layout is a semi-transparent rounded panel holding a 2x2 grid:
//
//     +---------------------------+
//     |  [torch]      [torch]       |   LED2(GP11) / LED4(GP83)
//     |  [watch]      [thermometer] |   jump to page 2 / page 3
//     +---------------------------+
//
// All the geometry derives from CC_PANEL_*, so changing the panel size needs no recomputation of each cell.
//
// Why these numbers have to accommodate a round panel: the display is a 468x468
// circle, radius 234, centre (234,234). The four corners of a rectangle are its
// furthest points from the centre and each has to be confirmed inside the
// circle, or the corner is clipped. With the current values:
//
//   panel top-left (84, 90)       distance sqrt(150^2 + 144^2) ~= 208 < 234  OK
//   panel bottom-right (384, 378) the same by symmetry                       OK
//   bottom-right cell corner (368, 362) distance sqrt(134^2 + 128^2) ~= 185  OK
//
// That leaves over 26 px of margin, so the corner radius itself (CC_PANEL_R = 34) does not reach the edge either.
//
//*****************************************************************************
#define CC_PANEL_W          (300)
#define CC_PANEL_H          (288)
#define CC_PANEL_X          ((FB_RESX - CC_PANEL_W) / 2)    // 84
#define CC_PANEL_Y          (90)
#define CC_PANEL_R          (34)                            // corner radius

#define CC_PAD              (16)                            // panel padding
#define CC_GAP              (12)                            // gap between cells
#define CC_TILE_W           ((CC_PANEL_W - (2 * CC_PAD) - CC_GAP) / 2)  // 128
#define CC_TILE_H           ((CC_PANEL_H - (2 * CC_PAD) - CC_GAP) / 2)  // 122
#define CC_TILE_R           (24)

//
// Top-left coordinate of the cell at column col, row row.
//
#define CC_TILE_X(col)      (CC_PANEL_X + CC_PAD + ((col) * (CC_TILE_W + CC_GAP)))
#define CC_TILE_Y(row)      (CC_PANEL_Y + CC_PAD + ((row) * (CC_TILE_H + CC_GAP)))

//
// Palette.
//
// The panel and the inactive cells carry alpha and are composited over the home
// image with NEMA_BL_SIMPLE, for the frosted-glass feel of a phone control
// centre. A true Gaussian blur would mean sampling the background and drawing it
// again, which is not worth it in a loop that redraws the whole screen every
// frame, so semi-transparent darkening approximates it.
//
// Colours are always built with CC_ABGR() rather than written directly as
// 0xff9a8878 the way they are elsewhere in this file. The difference is that
// these values needed repeated tuning against hardware -- how much alpha it
// takes to subdue the background image is something you only learn by looking --
// and a 32-bit constant means mentally unpacking and reordering ABGR every time,
// which makes swapping R and B easy.
//
#define CC_ABGR(a, r, g, b)  ((uint32_t)(((uint32_t)(a) << 24) | \
                                         ((uint32_t)(b) << 16) | \
                                         ((uint32_t)(g) <<  8) | \
                                          (uint32_t)(r)))

//
// Opacity of the panel. The home image is bright, and with too little alpha the
// background pushes through the whole panel and the white icons lose contrast.
// 0xdc is about 86%, so only around 14% of the background shows -- enough to see
// something underneath without interfering with the foreground.
//
// Raise this towards 0xff for a deeper panel or lower it for more transparency; the base colour need not change.
//
#define CC_PANEL_ALPHA      (0xdc)

#define CC_PANEL_COLOR      CC_ABGR(CC_PANEL_ALPHA, 0x10, 0x18, 0x20)  // deep blue-grey
#define CC_TILE_OFF_COLOR   CC_ABGR(0x70, 0x48, 0x50, 0x58)  // inactive: pale grey
#define CC_TILE_ON_COLOR    CC_ABGR(0xe8, 0xff, 0xb3, 0x40)  // active: amber
#define CC_TILE_NAV_COLOR   CC_ABGR(0x88, 0x50, 0x80, 0xa0)  // navigation cell: bluish
#define CC_ICON_OFF_COLOR   CC_ABGR(0xff, 0xff, 0xff, 0xff)  // inactive icon: white
#define CC_ICON_ON_COLOR    CC_ABGR(0xff, 0x20, 0x20, 0x20)  // active icon: dark grey

//*****************************************************************************
//
// Shared geometry of the control centre icons.
//
// All three icons are described in a 24x24 viewBox, matching the convention of
// common SVG icon sets, and converted to pixels at compile time. This follows
// the BT_ICON approach -- coordinates converted up front, the matrix only
// translating -- rather than scaling in the matrix: in NemaVG the stroke width
// and the path coordinates share one space, so letting the matrix scale would
// change the line width and each of the four icons would need its own adjustment
// to look equally thick.
//
//*****************************************************************************
#define CC_ICON_VIEWBOX     (24.0f)
#define CC_ICON_SIZE        (56.0f)                     // icon edge length in pixels
#define CC_S(v)             ((v) * (CC_ICON_SIZE / CC_ICON_VIEWBOX))
#define CC_ICON_STROKE_W    CC_S(1.8f)

//
// Offset that centres an icon inside its cell.
//
#define CC_ICON_DX          ((CC_TILE_W - (int)CC_ICON_SIZE) / 2)   // 36
#define CC_ICON_DY          ((CC_TILE_H - (int)CC_ICON_SIZE) / 2)   // 33

//*****************************************************************************
//
// The brightness panel (dragged up from the bottom).
//
// The counterpart of the control centre: that one pulls down from the top edge
// and exists only on the home page, while this pulls up from the bottom edge and
// exists on every page. The two are mutually exclusive (opening one closes the
// other), since both are overlays covering the whole screen.
//
// The layout is a semi-transparent rounded panel with an upright capsule-shaped slider in the middle:
//
//     +-----------+
//     |   +---+   |   <- the upper part is the track (dark)
//     |   |   |   |
//     |   |###|   |   <- the lower part is filled (white); its height is the brightness
//     |   |#*#|   |   <- the sun icon, at a fixed position
//     |   +---+   |
//     +-----------+
//
// All the geometry derives from BL_PANEL_*, so changing the panel size needs no recomputation of the slider.
//
// Boundary check for the round panel (the display is a 468x468 circle, radius 234, centre (234,234)):
// The four corners of the rectangle are its furthest points from the centre. With the current values:
//
//   panel top-left (130, 56)      distance sqrt(104^2 + 178^2) ~= 206 < 234  OK
//   panel bottom-right (338, 412) the same by symmetry                       OK
//
// That leaves over 28 px of margin, so the corner radius (BL_PANEL_R = 34) does not reach the edge either.
//
//*****************************************************************************
#define BL_PANEL_W          (208)
#define BL_PANEL_H          (356)
#define BL_PANEL_X          ((FB_RESX - BL_PANEL_W) / 2)    // 130
#define BL_PANEL_Y          (56)
#define BL_PANEL_R          (34)                            // corner radius

//
// The slider itself. Capsule-shaped, so the corner radius is half the width.
//
#define BL_SLIDER_W         (96)
#define BL_SLIDER_H         (268)
#define BL_SLIDER_X         ((FB_RESX - BL_SLIDER_W) / 2)   // 186
#define BL_SLIDER_Y         (BL_PANEL_Y + ((BL_PANEL_H - BL_SLIDER_H) / 2))
#define BL_SLIDER_R         (BL_SLIDER_W / 2)               // 48
#define BL_SLIDER_BOTTOM    (BL_SLIDER_Y + BL_SLIDER_H)     // 368

//
// The cyan glow around the slider.
//
// The design has cyan light spreading outward around the slider. A real outer
// glow means blurring the shape, which is not worth it in a loop that redraws the
// whole screen every frame, so three progressively larger rounded rectangles of
// decreasing alpha approximate it -- drawn outside in, with the outermost faintest.
//
#define BL_GLOW_LAYERS      (3)
#define BL_GLOW_STEP        (7)     // how many px each layer extends outward
#define BL_GLOW_ALPHA       (0x22)  // alpha of the outermost layer, doubling for each layer inward

//
// The sun icon. Fixed in the lower part of the slider and does not move with the brightness (as on a phone).
//
// A 24x24 viewBox, done the same way as the control centre icons: coordinates
// converted to pixels at compile time, the matrix only translating. The solid
// circle at the centre is drawn separately with nema_vg_draw_circle(); the eight
// rays are the path.
//
#define BL_SUN_VIEWBOX      (24.0f)
#define BL_SUN_SIZE         (46.0f)
#define BL_SUN_S(v)         ((v) * (BL_SUN_SIZE / BL_SUN_VIEWBOX))
#define BL_SUN_STROKE_W     BL_SUN_S(2.0f)
#define BL_SUN_DOT_R        BL_SUN_S(4.2f)  // radius of the solid centre circle

//
// Distance from the icon's centre to the bottom of the slider. 22% of the slider height, matching the design.
//
#define BL_SUN_CY           (BL_SLIDER_BOTTOM - (BL_SLIDER_H * 22 / 100))

//
// Palette. The panel shares CC_ABGR() and the same opacity as the control centre.
//
// BL_PANEL_ALPHA refers to CC_PANEL_ALPHA rather than spelling out another 0xdc:
// the two panels are the same "frosted overlay" and should be tuned together.
// Split them if the brightness panel ever needs its own transparency.
//
#define BL_PANEL_ALPHA      CC_PANEL_ALPHA

#define BL_PANEL_COLOR      CC_ABGR(BL_PANEL_ALPHA, 0x10, 0x18, 0x20)
#define BL_TRACK_COLOR      CC_ABGR(0xff, 0x2a, 0x2e, 0x33)  // track: dark grey
#define BL_FILL_COLOR       CC_ABGR(0xff, 0xff, 0xff, 0xff)  // filled: white

//
// The glow: cyan, the same accent as the temperature page. The alpha differs per
// layer, so it is a macro with a parameter rather than "one RGB constant the
// caller assembles" -- the latter, written as CC_ABGR(alpha, BL_GLOW_RGB), does
// not work: the preprocessor splits the arguments before expanding BL_GLOW_RGB,
// so CC_ABGR would receive two arguments instead of four.
//
#define BL_GLOW_COLOR(a)    CC_ABGR((a), 0x22, 0xe0, 0xf0)
#define BL_SUN_ON_COLOR     CC_ABGR(0xff, 0x18, 0x18, 0x18)  // over the white fill: dark grey
#define BL_SUN_OFF_COLOR    CC_ABGR(0xff, 0xc0, 0xc8, 0xd0)  // over the track: light grey

//*****************************************************************************
//
// Home page quick page menu (slides in from the left).
//
// The frame reuses the brightness panel's size and palette, with two navigation
// entries stacked inside. That keeps all three overlays in one visual language
// without mixing the brightness slider's state into the page menu.
//
//*****************************************************************************
#define PAGE_MENU_PANEL_W       BL_PANEL_W
#define PAGE_MENU_PANEL_H       BL_PANEL_H
#define PAGE_MENU_PANEL_X       BL_PANEL_X
#define PAGE_MENU_PANEL_Y       BL_PANEL_Y
#define PAGE_MENU_PANEL_R       BL_PANEL_R

#define PAGE_MENU_PAD           (18)
#define PAGE_MENU_GAP           (14)
#define PAGE_MENU_TILE_W        (PAGE_MENU_PANEL_W - (2 * PAGE_MENU_PAD))
#define PAGE_MENU_TILE_H        ((PAGE_MENU_PANEL_H - (2 * PAGE_MENU_PAD) - \
                                  PAGE_MENU_GAP) / 2)
#define PAGE_MENU_TILE_X        (PAGE_MENU_PANEL_X + PAGE_MENU_PAD)
#define PAGE_MENU_TILE_Y(row)   (PAGE_MENU_PANEL_Y + PAGE_MENU_PAD + \
                                 ((row) * (PAGE_MENU_TILE_H + PAGE_MENU_GAP)))
#define PAGE_MENU_TILE_R        (24)
#define PAGE_MENU_ICON_CX       ((float)PAGE_MENU_TILE_X + \
                                 ((float)PAGE_MENU_TILE_W / 2.0f))
#define PAGE_MENU_ICON_CY(row)  ((float)PAGE_MENU_TILE_Y(row) + \
                                 ((float)PAGE_MENU_TILE_H / 2.0f))
#define PAGE_MENU_ICON_STROKE   (5.0f)
#define PAGE_MENU_DETAIL_STROKE (2.0f)

#define PAGE_MENU_PANEL_COLOR   BL_PANEL_COLOR
#define PAGE_MENU_TILE_COLOR    CC_TILE_NAV_COLOR
#define PAGE_MENU_ICON_COLOR    CC_ICON_OFF_COLOR
#define PAGE_MENU_ACCENT_COLOR  CC_ABGR(0xff, 0x22, 0xe0, 0xf0)

//
// The start of an upward drag is limited by BL_PULL_START_MIN_Y, derived
// together with the control centre's CC_PULL_START_MAX_Y from UI_EDGE_PULL_BAND
// (see the gesture threshold section).
//

//*****************************************************************************
//
// The sweeping light on the home page horizon.
//
// The blue horizon at the bottom of the home image
// (src/Generated_2/EdgePilot.png) and the bright spot at its centre are baked
// into the bitmap and cannot be moved on their own. There is only one way to
// make it move: blit the original as before, then add a travelling bright spot
// on the same horizon with additive blending (NEMA_BL_ADD) -- addition only
// brightens and never covers the hex grid and the gradient underneath, so it
// stays the same light rather than a block floating on top.
//
// The spot travels back and forth along a sine and fades towards both ends
// (HOME_GLOW_EDGE_FADE): the endpoints are exactly where the sine is slowest, and
// without the fade a bright dot would visibly sit at the edge "waiting to turn",
// which looks fake. With it, the light appears to rise at the centre, flow
// outward, vanish, and rise at the centre again.
//
//*****************************************************************************

//
// The centre of the spot's path, written in "coordinates inside the home image"
// rather than framebuffer coordinates: the image is blitted centred, so changing
// the panel size (FB_RESX is either 468 or 456) shifts the framebuffer
// coordinates while the spot's position in the image does not. The actual
// position is converted at draw time with the same centring expression as nema_blit().
//
// Both values were measured from EdgePilot.png: the brightest row of the horizon
// is y=385, and the centre of that row's white core is x=226.
//
#define HOME_GLOW_IMG_X     (226)
#define HOME_GLOW_IMG_Y     (385)

//
// Amplitude of the travel (px). 140 is roughly how far from the centre the blue
// light is still visible along the horizon; further and it sweeps into the
// almost-black sides and becomes a bright dot out of nowhere.
//
#define HOME_GLOW_AMP       (140.0f)

//
// Period of one round trip (ms). 3600 ms completes one sine cycle, i.e. about
// 3.6 seconds out to the right and back to the left -- slow enough not to
// distract, fast enough to be visibly moving.
//
#define HOME_GLOW_PERIOD_MS (3600U)

//
// How much to attenuate at the two extremes. 0.35 leaves about 65% brightness at
// the endpoints -- any higher and the light no longer looks like it is moving
// (the centre of the horizon is white anyway, and the spot only shows once it
// leaves the centre), any lower and it leaves one too-obvious isolated dot in
// the dark areas at the sides.
//
#define HOME_GLOW_EDGE_FADE (0.35f)

//
// Colour of the spot. A bluish white, the same hue as the horizon itself; the
// alpha is given per layer by the table below, so only the RGB is defined here.
//
#define HOME_GLOW_R         (0x38)
#define HOME_GLOW_G         (0xac)
#define HOME_GLOW_B         (0xff)

//*****************************************************************************
//
// The temperature trend page (UI_PAGE_TREND).
//
// The screen has four blocks, top to bottom: a large clock, the date, the 3D temperature curve, and a footer.
//
// The "3D" is a shear projection, not real three dimensions: a data point's
// (u, v) -- u is time, v is temperature -- is projected to the screen with y
// further reduced by u * TREND_PLOT_RISE. Gridlines of constant u therefore stay
// vertical while those of constant v slope up to the right, which reads as a
// receding ground plane. A real perspective divide (near large, far small) would
// cost a division per point, and at this size the difference is invisible.
//
//*****************************************************************************

//
// Length of the sample ring buffer. The TMP119 is set to one conversion per
// second (see sensor_task.h), so 60 entries is exactly the last minute.
//
#define TREND_SAMPLE_COUNT  (60)

//
// Marker for an invalid sample. Temperatures are stored as int16_t in hundredths
// of a degree Celsius (37.20 degrees = 3720, far from the int16 limit), so
// INT16_MIN as a sentinel can never collide with a real temperature.
//
#define TREND_SAMPLE_NONE   ((int16_t)(-32768))

//
// The plot area. Coordinates are absolute framebuffer coordinates (panel 468x468, centre 234,234).
//
// X1 at 400 and the baseline at 330 come from the chord width of the round
// frame: on row y=330 the usable range is x 21 to 447, and any further right
// the curve would hit the bezel.
//
#define TREND_PLOT_X0       (86.0f)
#define TREND_PLOT_X1       (404.0f)
#define TREND_PLOT_W        (TREND_PLOT_X1 - TREND_PLOT_X0)

//
// The y of the baseline (v = 0) and the height of the temperature axis.
//
#define TREND_PLOT_Y1       (330.0f)
#define TREND_PLOT_H        (158.0f)

//
// Rise of the projection: the whole plot lifts this many px from left to right.
//
// Currently 0, i.e. an orthogonal flat plot -- the design (Date_temp.png) uses a
// rectangular grid, with the sense of depth coming from the curve's glow and the
// gradient of the fill rather than from perspective. To tilt the ground plane
// back, 20 to 30 here restores the sloped version and the gridlines, the curve
// and the NOW vertical all follow; nothing else needs changing.
//
#define TREND_PLOT_RISE     (0.0f)

//
// Number of gridlines. 8 vertical and 4 horizontal look about right -- any denser
// and the 1 px lines blur together at the edge of the round panel.
//
#define TREND_GRID_COLS     (8)
#define TREND_GRID_ROWS     (4)

//
// Minimum span of the temperature axis (degrees Celsius).
//
// The axis range is computed dynamically from the min/max in the buffer. Without
// this floor, the +-0.02 degree jitter at room temperature would be amplified
// into a screenful of sawtooth and look broken.
//
#define TREND_SPAN_MIN_C    (1.4f)

//
// Proportion of margin left at the top and bottom. A curve touching the frame looks bad and leaves nowhere for the bright end point.
//
#define TREND_SPAN_PAD      (0.12f)

//
// Centre y and font size of each text row.
//
#define TREND_TIME_CY       (92.0f)
#define TREND_TIME_FONT     (60.0f)
#define TREND_DATE_CY       (140.0f)
#define TREND_DATE_FONT     (21.0f)
#define TREND_VALUE_FONT    (26.0f)
#define TREND_AXIS_FONT     (16.0f)
#define TREND_FOOT_CY       (378.0f)
#define TREND_FOOT_FONT     (21.0f)
#define TREND_RANGE_CY      (404.0f)
#define TREND_RANGE_FONT    (17.0f)

//
// The large temperature group (value + degree ring + "C") follows the end of the
// curve, like a label hanging beside that bright point rather than pinned in a
// corner -- that is what the design does, keeping the reading next to the point
// it represents so the eye does not have to pair them up.
//
// GAP is how far above the end point the label's centre sits, LEAD is how far the
// label's right edge is from the end point (a thin leader line fills the space).
//
#define TREND_VALUE_GAP     (22.0f)
#define TREND_VALUE_LEAD    (34.0f)

//
// Lower bound for the label's y. When the end point is high (temperature against
// the top of the axis) the label rises and would hit the date row; clamping here
// lengthens the leader instead.
//
#define TREND_VALUE_CY_MIN  (170.0f)

//
// The degree symbol. The font's glyphs cover only 0x20 to 0x7E and have no degree sign, so a small ring is used as on the temperature page.
//
#define TREND_DEG_R         (4.0f)
#define TREND_DEG_W         (2.0f)
#define TREND_DEG_RISE      (0.30f)     // how far the ring's centre sits above the digits' centre (as a fraction of the font size)

//
// The curve and its end marker.
//
#define TREND_LINE_W        (3.0f)

//
// Glow of the curve: same colour and path, but wider and fainter, drawn beneath the body.
//
// The GPU has no blur, so a real glow is impossible; a width-8 semi-transparent
// line under a width-3 solid one comes very close at this size -- the edge
// antialiasing already gives a little spread.
//
#define TREND_GLOW_W        (8.0f)
#define TREND_GLOW_ALPHA    (0x4a)

//
// The dark bands used to fade the fill downward.
//
// The fill is a single-colour polygon (one quad per segment) and cannot take a
// vertical gradient directly. Instead, after filling, a few semi-transparent
// horizontal bands in the background colour are laid over the plot area, more
// opaque further down, which subdues the lower half. Five is enough -- more and
// the steps between bands become more obvious than the gradient itself.
//
// These bands must be drawn before the gridlines, or the gridlines are darkened too.
//
#define TREND_FADE_BANDS    (5)
#define TREND_FADE_ALPHA    (0x48)

#define TREND_NOW_DOT_R     (5.0f)
#define TREND_NOW_RING_R    (8.0f)
#define TREND_NOW_RING_W    (2.0f)

//
// Outer decoration. A full ring of fine ticks plus two thick arcs left and right, so this page has a bezel like the other two.
//
#define TREND_RIM_R         (222.0f)    // outer radius of the fine ticks
#define TREND_RIM_LEN       (10.0f)     // tick length
#define TREND_RIM_W         (2.0f)
#define TREND_RIM_COUNT     (60)
#define TREND_ARC_R         (214.0f)    // centreline radius of the two thick arcs
#define TREND_ARC_W         (5.0f)
#define TREND_ARC_L_DEG0    (152.0f)    // left blue arc
#define TREND_ARC_L_DEG1    (208.0f)
#define TREND_ARC_R_DEG0    (332.0f)    // right orange arc
#define TREND_ARC_R_DEG1    (388.0f)

//
// Palette. The background is a little deeper than the temperature page, so the blue and orange of the curve separate.
//
//
// The three components of the background are kept separately: the dark bands in
// trend_draw_fill() layer the same colour at different alphas, and splitting
// them avoids two copies where one gets updated and the other does not.
//
#define TREND_BG_R          (0x04)
#define TREND_BG_G          (0x08)
#define TREND_BG_B          (0x11)

#define TREND_BG_COLOR      CC_ABGR(0xff, TREND_BG_R, TREND_BG_G, TREND_BG_B)
#define TREND_GRID_COLOR    CC_ABGR(0xff, 0x16, 0x32, 0x52)
#define TREND_GRID_BASE     CC_ABGR(0xff, 0x2a, 0x58, 0x84)
#define TREND_COLD_COLOR    CC_ABGR(0xff, 0x2d, 0xb6, 0xff)
#define TREND_WARM_COLOR    CC_ABGR(0xff, 0xff, 0x9d, 0x2e)
#define TREND_FILL_COLOR    CC_ABGR(0x88, 0x12, 0x68, 0xc0)
#define TREND_TEXT_COLOR    CC_ABGR(0xff, 0xff, 0xff, 0xff)
#define TREND_DIM_COLOR     CC_ABGR(0xff, 0x8f, 0xa6, 0xbd)
#define TREND_RIM_COLOR     CC_ABGR(0xff, 0x30, 0x48, 0x60)

//
// Where the polyline's gradient starts to warm up.
//
// From 0 to this position it stays pure blue and only then interpolates to
// orange. It does not interpolate from the start because the midpoint of blue
// and orange in RGB is a grey-green -- linear interpolation would wash the middle
// of the curve into a dirty grey line rather than "cool turning warm". Confining
// the transition to the tail keeps both ends saturated and matches the design.
//
#define TREND_WARM_START    (0.55f)

//
// The date's starting point.
//
// The board has no RTC, so the time, like the watch face page, is "time since
// boot" plus an offset (see WATCH_START_HOUR). The date works the same way:
// counting from this day, advancing one day every 86400 seconds. Once an RTC is
// attached this set of constants and WATCH_START_HOUR go together.
//
#define TREND_START_WDAY    (5)         // 0=SUN … 5=FRI
#define TREND_START_DAY     (18)
#define TREND_START_MONTH   (9)         // 1=JAN … 9=SEP



//*****************************************************************************
//
// Render task handle.
//
//*****************************************************************************
TaskHandle_t RenderTaskHandle;

//*****************************************************************************
//
// UI pages.
//
//*****************************************************************************
typedef enum
{
    UI_PAGE_HOME = 0,   // The EdgePilot home page, shown by default at boot.
    UI_PAGE_WATCH,      // The analogue watch face.
    UI_PAGE_TEMP,       // TMP119 temperature display.
    UI_PAGE_TREND,      // Time / date and the 3D temperature trend of the TMP119.
    UI_PAGE_MAP,        // Map navigation from Taipei 101 to Taipei Main Station.
    UI_PAGE_COUNT,      // The number of browsable pages; must come after every switchable page.

    //
    // The UI Designer generated page (for testing) is currently out of the
    // carousel: it sits after UI_PAGE_COUNT, so the range check in ui_page_set()
    // rejects it and swipes cannot reach it. To re-enable it, move it back before
    // UI_PAGE_COUNT and add its name to pcPageNames[].
    //
    UI_PAGE_DESIGN
} ui_page_e;

//
// FreeRTOS ticks converted to milliseconds, for animations expressed in
// milliseconds (the UI Designer generated page and the home page's sweeping light).
//
// configTICK_RATE_HZ happens to be 1000 right now, so the two are numerically
// equal; it is written as a conversion because the generated side's interface is
// in milliseconds, and changing the tick rate should not change animation speed.
//
#define TICKS_TO_MS(t)      (((uint32_t)(t) * 1000U) / configTICK_RATE_HZ)

static ui_page_e g_ePage = UI_PAGE_HOME;

//*****************************************************************************
//
// Button1 (SW3) requests a return to the home page.
//
// Written by LedTask's button interrupt and read and cleared by RenderTask every
// frame, hence volatile -- without it the compiler may hoist the read out of the
// loop and do it once, and the button would never respond again.
//
// A bool load or store is atomic on Cortex-M, and there are only two actions
// here, "set" and "clear", so no critical section is needed: the worst case of a
// simultaneous access is that this press takes effect one frame later.
//
//*****************************************************************************
static volatile bool g_bGoHomeReq;

//*****************************************************************************
//
// Request a return to the home page.
//
//*****************************************************************************
void
RenderTaskGoHome(void)
{
    g_bGoHomeReq = true;
}

//*****************************************************************************
//
// Button0 (SW1) requests the page overview to be toggled.
//
// volatile for the same reason as g_bGoHomeReq, and unlocked for the same reason:
// the ISR only raises the flag, and RenderTask does the actual open or close on
// the next frame (see psel_toggle()).
//
//*****************************************************************************
static volatile bool g_bPageOverviewReq;

//*****************************************************************************
//
// Request the page overview to be toggled (shrink / zoom back).
//
//*****************************************************************************
void
RenderTaskTogglePageOverview(void)
{
    g_bPageOverviewReq = true;
}

//*****************************************************************************
//
// Sample buffer of the temperature trend page.
//
// Maintained by trend_sample_update() on every RenderTask frame, whichever page
// is showing -- the curve is about "the last minute", not "the last minute spent
// on this page". Collecting only after switching here would show the user an
// empty line slowly growing in.
//
// Only RenderTask touches these variables (written in trend_sample_update(),
// read in draw_trend()), and within one task no lock is needed.
//
//*****************************************************************************
static int16_t  g_ai16TrendC100[TREND_SAMPLE_COUNT];

//
// Number of entries written, capped at TREND_SAMPLE_COUNT; it stays at the cap once the buffer is full.
//
static uint32_t g_ui32TrendCount;

//
// Where the next entry goes (ring). Once the buffer is full this is also the oldest entry.
//
static uint32_t g_ui32TrendHead;

//
// Tick of the last sample.
//
static uint32_t g_ui32TrendLastTick;


//*****************************************************************************
//
// Image assets.
//
//*****************************************************************************
//
// Every size comes from asset_store.h. With the assets on eMMC that is the only
// source -- the files hold raw pixel data with no header, so the reader and the
// PC-side generator must agree on one set of numbers.
//
static img_obj_t g_sHomeImage =
{
    .bo = {0}, .w = ASSET_HOME_W, .h = ASSET_HOME_H, .stride = -1, .color = 0,
    .format = NEMA_RGB565, .sampling_mode = NEMA_FILTER_PS,
};

static img_obj_t g_sWatchface = {{0}, ASSET_WATCH_BG_W, ASSET_WATCH_BG_H, -1, 0, NEMA_RGB565, NEMA_FILTER_BL};
static img_obj_t g_sHourhand = {{0}, ASSET_HAND_HOUR_W, ASSET_HAND_HOUR_H, -1, 0, NEMA_RGBA8888, NEMA_FILTER_BL};
static img_obj_t g_sMinuteHand = {{0}, ASSET_HAND_MIN_W, ASSET_HAND_MIN_H, -1, 0, NEMA_RGBA8888, NEMA_FILTER_BL};
static img_obj_t g_sSecondHand = {{0}, ASSET_HAND_SEC_W, ASSET_HAND_SEC_H, -1, 0, NEMA_RGBA8888, NEMA_FILTER_BL};

//
// Assets of the second watch face. The 360x360 dial is far smaller than the panel
// and must be scaled to fill, so the filter is NEMA_FILTER_BL -- a non-integer
// 1.3x scale with point sampling would leave steps on the outer ticks.
//
//
// The aurora assets are no longer built into the firmware (their slots went to
// the Ambiq face, see g_sFaces[]). The four img_obj_t and their load entries were
// removed together -- keeping them would still read 346 KB from eMMC with nothing
// drawing it, so the swap would save no memory at all, which was its only point.
//

//
// The third watch face. The hands share their artwork with the second face but
// have already been scaled to the 454 basis on the PC and are separate assets
// (see tools/face3_prep.py).
//
// 454x454 is within 3% of the panel's 468, so it blits 1:1 and centred
// (bFit = false) and the filter is NEMA_FILTER_PS -- with no resampling, point
// sampling is sharpest and the edges of those 3D numerals stay crisp.
//
static img_obj_t g_sFace3Dial = {{0}, ASSET_FACE3_BG_W, ASSET_FACE3_BG_H, -1, 0, NEMA_RGB565, NEMA_FILTER_PS};
static img_obj_t g_sFace3Hour = {{0}, ASSET_FACE3_HOUR_W, ASSET_FACE3_HOUR_H, -1, 0, NEMA_RGBA8888, NEMA_FILTER_BL};
static img_obj_t g_sFace3Min  = {{0}, ASSET_FACE3_MIN_W, ASSET_FACE3_MIN_H, -1, 0, NEMA_RGBA8888, NEMA_FILTER_BL};
static img_obj_t g_sFace3Sec  = {{0}, ASSET_FACE3_SEC_W, ASSET_FACE3_SEC_H, -1, 0, NEMA_RGBA8888, NEMA_FILTER_BL};

//
// The Ambiq Apollo510B face. It has one extra asset, the centre cap; see psCap in g_sFaces[].
//
static img_obj_t g_sFace4Dial = {{0}, ASSET_FACE4_BG_W, ASSET_FACE4_BG_H, -1, 0, NEMA_RGB565, NEMA_FILTER_PS};
static img_obj_t g_sFace4Hour = {{0}, ASSET_FACE4_HOUR_W, ASSET_FACE4_HOUR_H, -1, 0, NEMA_RGBA8888, NEMA_FILTER_BL};
static img_obj_t g_sFace4Min  = {{0}, ASSET_FACE4_MIN_W, ASSET_FACE4_MIN_H, -1, 0, NEMA_RGBA8888, NEMA_FILTER_BL};
static img_obj_t g_sFace4Sec  = {{0}, ASSET_FACE4_SEC_W, ASSET_FACE4_SEC_H, -1, 0, NEMA_RGBA8888, NEMA_FILTER_BL};
static img_obj_t g_sFace4Cap  = {{0}, ASSET_FACE4_CAP_W, ASSET_FACE4_CAP_H, -1, 0, NEMA_RGBA8888, NEMA_FILTER_BL};

const unsigned char* icon_to_draw = blowing;
const unsigned char* icon_array[3] = {blowing, climate, day};

//*****************************************************************************
//
// Path data and NemaVG objects of the Bluetooth icon.
//
// All LINEs in absolute coordinates rather than the SVG's original relative form
// -- the data is fixed, and absolute coordinates are easier to check.
//
//*****************************************************************************
static const uint8_t g_pui8BtIconSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // (7,7)
    NEMA_VG_PRIM_LINE,      // (17,17)
    NEMA_VG_PRIM_LINE,      // (12,22)
    NEMA_VG_PRIM_LINE,      // (12,2)
    NEMA_VG_PRIM_LINE,      // (17,7)
    NEMA_VG_PRIM_LINE       // (7,17)
};

static const nema_vg_float_t g_pfBtIconData[] =
{
    BT_ICON_S( 7.0f), BT_ICON_S( 7.0f),
    BT_ICON_S(17.0f), BT_ICON_S(17.0f),
    BT_ICON_S(12.0f), BT_ICON_S(22.0f),
    BT_ICON_S(12.0f), BT_ICON_S( 2.0f),
    BT_ICON_S(17.0f), BT_ICON_S( 7.0f),
    BT_ICON_S( 7.0f), BT_ICON_S(17.0f)
};

//
// Created once at the start of RenderTask and reused. If creation fails (NULL is
// returned) the icon is simply not drawn, so the temperature page does not break with it.
//
static NEMA_VG_PATH_HANDLE  g_hBtIconPath;
static NEMA_VG_PAINT_HANDLE g_hBtIconPaint;

//*****************************************************************************
//
// Paths of the pairing result badge.
//
// Success is a tick, failure an exclamation mark, both drawn inside the same
// TEMP_BADGE_R ring (the circle is added with nema_vg_draw_circle(), for the same
// reason as the control centre icons).
//
// The coordinate system matches the other icons: a 24x24 viewBox converted to
// pixels at compile time. The edge length here is the badge circle's diameter,
// i.e. the square circumscribing the content -- the strokes of the tick and the
// exclamation mark occupy only the middle of the viewBox and never reach the ring.
//
//*****************************************************************************
#define BADGE_VIEWBOX       (24.0f)
#define BADGE_S(v)          ((v) * ((TEMP_BADGE_R * 2.0f) / BADGE_VIEWBOX))
#define BADGE_STROKE_W      BADGE_S(2.6f)

//
// The tick: a three-point polyline, with round caps and joins so both ends and the corner are rounded.
//
//      6,12.5
//         \        18,7.5
//          \      /
//        10.5,17 -
//
static const uint8_t g_pui8BadgeOkSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // ( 6.0,12.5)
    NEMA_VG_PRIM_LINE,      // (10.5,17.0)
    NEMA_VG_PRIM_LINE       // (18.0, 7.5)
};

static const nema_vg_float_t g_pfBadgeOkData[] =
{
    BADGE_S( 6.0f), BADGE_S(12.5f),
    BADGE_S(10.5f), BADGE_S(17.0f),
    BADGE_S(18.0f), BADGE_S( 7.5f)
};

//
// The exclamation mark: a vertical bar with a dot below it.
//
// The dot is drawn as an extremely short line with a round cap (0.4 viewBox units
// long), which comes out as a circle the diameter of the line width. draw_circle
// is avoided so the bar and the dot belong to one path, drawn in a single
// draw_path with no need to switch the fill rule again.
//
static const uint8_t g_pui8BadgeErrSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // (12, 6.0)  top of the bar
    NEMA_VG_PRIM_LINE,      // (12,14.0)  bottom of the bar
    NEMA_VG_PRIM_MOVE,      // (12,17.6)  the dot
    NEMA_VG_PRIM_LINE       // (12,18.0)
};

static const nema_vg_float_t g_pfBadgeErrData[] =
{
    BADGE_S(12.0f), BADGE_S( 6.0f),
    BADGE_S(12.0f), BADGE_S(14.0f),
    BADGE_S(12.0f), BADGE_S(17.6f),
    BADGE_S(12.0f), BADGE_S(18.0f)
};

static NEMA_VG_PATH_HANDLE  g_hBadgeOkPath;
static NEMA_VG_PATH_HANDLE  g_hBadgeErrPath;

//*****************************************************************************
//
// Paths of the control centre icons.
//
// All stroked outlines, drawn the same way as the Bluetooth icon: MOVE/LINE in
// absolute coordinates with round caps and joins. The curved parts (the watch
// dial, the thermometer bulb) are left out of the path and drawn separately with
// nema_vg_draw_circle() -- approximating a circle with LINEs is visibly polygonal
// at 56 px, and NemaVG's circle is hardware-accelerated, costing less than a few
// dozen extra vertices.
//
//*****************************************************************************

//
// Torch: the trapezoid of the head plus the body, with a divider in between. Two
// sub-paths, the second starting at the second MOVE.
//
//      6,2 ______ 18,2
//         \      /
//     9,8  ------  15,8      <- the divider
//          |    |
//     9,21 |____| 15,21
//
static const uint8_t g_pui8FlashIconSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // ( 6, 2)  head, top left
    NEMA_VG_PRIM_LINE,      // (18, 2)  head, top right
    NEMA_VG_PRIM_LINE,      // (15, 8)  narrowing to the body
    NEMA_VG_PRIM_LINE,      // (15,21)  body, bottom right
    NEMA_VG_PRIM_LINE,      // ( 9,21)  body, bottom left
    NEMA_VG_PRIM_LINE,      // ( 9, 8)  back to the narrowing
    NEMA_VG_PRIM_CLOSE,     //          close back to the head's top left
    NEMA_VG_PRIM_MOVE,      // ( 9, 8)  start of the divider
    NEMA_VG_PRIM_LINE       // (15, 8)  end of the divider
};

static const nema_vg_float_t g_pfFlashIconData[] =
{
    CC_S( 6.0f), CC_S( 2.0f),
    CC_S(18.0f), CC_S( 2.0f),
    CC_S(15.0f), CC_S( 8.0f),
    CC_S(15.0f), CC_S(21.0f),
    CC_S( 9.0f), CC_S(21.0f),
    CC_S( 9.0f), CC_S( 8.0f),
    CC_S( 9.0f), CC_S( 8.0f),
    CC_S(15.0f), CC_S( 8.0f)
};

//
// Watch: the two strap sections plus the hour and minute hands; the dial circle is drawn separately.
//
// The strap meets the dial at y = 6.8 / 17.2, the intersection of an r = 6 circle
// with the column x = 9 (12 +- sqrt(6^2 - 3^2) = 12 +- 5.196), so the strap ends
// sit exactly against the dial's outer edge, neither cutting into it nor floating.
//
static const uint8_t g_pui8WatchIconSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // ( 9,6.8) upper strap: where it meets the dial on the right
    NEMA_VG_PRIM_LINE,      // ( 9, 2)
    NEMA_VG_PRIM_LINE,      // (15, 2)
    NEMA_VG_PRIM_LINE,      // (15,6.8)
    NEMA_VG_PRIM_MOVE,      // ( 9,17.2) lower strap
    NEMA_VG_PRIM_LINE,      // ( 9,22)
    NEMA_VG_PRIM_LINE,      // (15,22)
    NEMA_VG_PRIM_LINE,      // (15,17.2)
    NEMA_VG_PRIM_MOVE,      // (12, 9)  minute hand (pointing up)
    NEMA_VG_PRIM_LINE,      // (12,12)  pivot
    NEMA_VG_PRIM_LINE       // (14.5,12) hour hand (pointing right)
};

static const nema_vg_float_t g_pfWatchIconData[] =
{
    CC_S( 9.0f), CC_S( 6.8f),
    CC_S( 9.0f), CC_S( 2.0f),
    CC_S(15.0f), CC_S( 2.0f),
    CC_S(15.0f), CC_S( 6.8f),
    CC_S( 9.0f), CC_S(17.2f),
    CC_S( 9.0f), CC_S(22.0f),
    CC_S(15.0f), CC_S(22.0f),
    CC_S(15.0f), CC_S(17.2f),
    CC_S(12.0f), CC_S( 9.0f),
    CC_S(12.0f), CC_S(12.0f),
    CC_S(14.5f), CC_S(12.0f)
};

//
// Thermometer: the tube plus three ticks on the right; the bulb circle is drawn separately.
//
// The tube ends at y = 15.5 while the bulb is centred at (12, 18.5) with radius 3,
// so the bulb covers the last 0.5 px of the tube and the seam does not show.
//
static const uint8_t g_pui8TempIconSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // (12, 5)    top of the tube
    NEMA_VG_PRIM_LINE,      // (12,15.5)  bottom of the tube (into the bulb)
    NEMA_VG_PRIM_MOVE,      // (14.5, 7)  tick 1
    NEMA_VG_PRIM_LINE,      // (16.5, 7)
    NEMA_VG_PRIM_MOVE,      // (14.5,10)  tick 2
    NEMA_VG_PRIM_LINE,      // (16.5,10)
    NEMA_VG_PRIM_MOVE,      // (14.5,13)  tick 3
    NEMA_VG_PRIM_LINE       // (16.5,13)
};

static const nema_vg_float_t g_pfTempIconData[] =
{
    CC_S(12.0f), CC_S( 5.0f),
    CC_S(12.0f), CC_S(15.5f),
    CC_S(14.5f), CC_S( 7.0f),
    CC_S(16.5f), CC_S( 7.0f),
    CC_S(14.5f), CC_S(10.0f),
    CC_S(16.5f), CC_S(10.0f),
    CC_S(14.5f), CC_S(13.0f),
    CC_S(16.5f), CC_S(13.0f)
};

//
// The path objects and a shared paint. There is only one paint, with the colour
// reset before each draw -- the four icons change colour with their on/off state,
// and one paint each would only use more memory.
//
// If any of them fails to create (returns NULL) that icon is not drawn, and the rest of the menu works as usual.
//
static NEMA_VG_PATH_HANDLE  g_hFlashIconPath;
static NEMA_VG_PATH_HANDLE  g_hWatchIconPath;
static NEMA_VG_PATH_HANDLE  g_hTempIconPath;
static NEMA_VG_PAINT_HANDLE g_hCcIconPaint;

//*****************************************************************************
//
// The sun icon of the brightness panel.
//
// Eight rays, four axial and four diagonal. The solid circle at the centre is not
// part of the path: approximating a circle with line segments is visibly
// polygonal at this size, and nema_vg_draw_circle() is hardware-accelerated (the
// same reason as the watch dial in the control centre).
//
// The four diagonals use +-3.4, the 45-degree component of 4.8 (4.8 * 0.707 ~= 3.4),
// so the inner and outer endpoints of all eight rays land on the same two
// concentric circles and the lengths match.
//
//   inner radius 6.8 (12 +- 4.8), outer radius 10 (12 +- 10 for the axial endpoints)
//
//*****************************************************************************
static const uint8_t g_pui8SunIconSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // (12, 2.0) up
    NEMA_VG_PRIM_LINE,      // (12, 5.2)
    NEMA_VG_PRIM_MOVE,      // (12,18.8) down
    NEMA_VG_PRIM_LINE,      // (12,22.0)
    NEMA_VG_PRIM_MOVE,      // ( 2.0,12) left
    NEMA_VG_PRIM_LINE,      // ( 5.2,12)
    NEMA_VG_PRIM_MOVE,      // (18.8,12) right
    NEMA_VG_PRIM_LINE,      // (22.0,12)
    NEMA_VG_PRIM_MOVE,      // ( 4.9, 4.9) upper left
    NEMA_VG_PRIM_LINE,      // ( 8.6, 8.6)
    NEMA_VG_PRIM_MOVE,      // (15.4,15.4) lower right
    NEMA_VG_PRIM_LINE,      // (19.1,19.1)
    NEMA_VG_PRIM_MOVE,      // (19.1, 4.9) upper right
    NEMA_VG_PRIM_LINE,      // (15.4, 8.6)
    NEMA_VG_PRIM_MOVE,      // ( 8.6,15.4) lower left
    NEMA_VG_PRIM_LINE       // ( 4.9,19.1)
};

static const nema_vg_float_t g_pfSunIconData[] =
{
    BL_SUN_S(12.0f), BL_SUN_S( 2.0f),
    BL_SUN_S(12.0f), BL_SUN_S( 5.2f),
    BL_SUN_S(12.0f), BL_SUN_S(18.8f),
    BL_SUN_S(12.0f), BL_SUN_S(22.0f),
    BL_SUN_S( 2.0f), BL_SUN_S(12.0f),
    BL_SUN_S( 5.2f), BL_SUN_S(12.0f),
    BL_SUN_S(18.8f), BL_SUN_S(12.0f),
    BL_SUN_S(22.0f), BL_SUN_S(12.0f),
    BL_SUN_S( 4.9f), BL_SUN_S( 4.9f),
    BL_SUN_S( 8.6f), BL_SUN_S( 8.6f),
    BL_SUN_S(15.4f), BL_SUN_S(15.4f),
    BL_SUN_S(19.1f), BL_SUN_S(19.1f),
    BL_SUN_S(19.1f), BL_SUN_S( 4.9f),
    BL_SUN_S(15.4f), BL_SUN_S( 8.6f),
    BL_SUN_S( 8.6f), BL_SUN_S(15.4f),
    BL_SUN_S( 4.9f), BL_SUN_S(19.1f)
};

static NEMA_VG_PATH_HANDLE  g_hSunIconPath;

//*****************************************************************************
//
// State and cell definitions of the control centre.
//
//*****************************************************************************

//
// Whether the menu is open. Only meaningful on the home page, and always closed on a page change.
//
static bool g_bMenuOpen = false;

//
// The quick page menu summoned by a right swipe on the home page. It exists only there and is mutually exclusive with the control centre and the brightness panel.
//
static bool g_bPageMenuOpen = false;

//*****************************************************************************
//
// State of the brightness panel.
//
// g_bBlOpen   whether the panel is open. Valid on every page, mutually exclusive with the two home page menus.
// g_bBlDrag   whether this press has already been classified as "dragging the slider".
//
// Why g_bBlDrag is needed: a gesture is classified when the finger lifts, and
// dragging the slider usually covers a large distance, so the release is bound to
// come out as SWIPE_UP or SWIPE_DOWN. Without remembering that "this press was a
// slider drag", that SWIPE_DOWN would immediately read as "close the panel" --
// adjusting the brightness would close the panel by itself.
//
//*****************************************************************************
static bool g_bBlOpen = false;

#if WATCHFACE_ENABLE_TOUCH
//
// Only touch interaction uses these, so they are compiled in with
// WATCHFACE_ENABLE_TOUCH. g_bBlOpen cannot be wrapped that way -- bl_draw() reads
// it on every page, and that code is unaffected by the switch.
//
static bool g_bBlDrag = false;
#endif

//
// The action behind each of the four cells.
//
typedef enum
{
    CC_ACT_LED2 = 0,    // toggle GP11
    CC_ACT_LED4,        // toggle GP83
    CC_ACT_GO_WATCH,    // jump to page 2 (watch face)
    CC_ACT_GO_TEMP,     // jump to page 3 (temperature)
    CC_ACT_COUNT
} cc_action_e;

//
// The two entries of the home page quick page menu.
//
typedef enum
{
    PAGE_MENU_ACT_TREND = 0,
    PAGE_MENU_ACT_MAP,
    PAGE_MENU_ACT_COUNT
} page_menu_action_e;

//*****************************************************************************
//
// Frame buffer.
//
//*****************************************************************************
img_obj_t g_sFrameBuffer =
{
    .bo = {0}, .w = FB_RESX, .h = FB_RESY, .stride = -1, .color = 0, .format = FB_COLOR_FORMAT, .sampling_mode = 0,
};

//*****************************************************************************
//
// Thumbnail buffer of the page overview.
//
// The page overview shrinks a whole page by "draw then capture": the page is
// drawn into g_sFrameBuffer as usual, that is used as a source texture to scale
// into this buffer, and finally the screen is cleared and this buffer blitted
// back. The full reasoning is in the "page overview" section below.
//
// The size, PSEL_THUMB_PX, is covered there. The format follows the framebuffer
// -- the capture is a same-format scaling blit, and a different format would only
// add a colour conversion.
//
// Allocated when RenderTask starts (from the same pool as the framebuffer). On an
// allocation failure base_virt is NULL and psel_open() simply refuses to open;
// nothing else is affected.
//
//*****************************************************************************
//
// 208 rather than the original 264: 51.6 KB handed back to the assets.
//
// 264 came from the "size once shrunk" (468 x 0.56 = 262), which blits almost 1:1 and is sharpest.
// But once the Ambiq face (454 dial) replaced aurora (390 dial) the pool was
// 122 KB tighter, the third face's assets stopped loading, and the thumbnail was
// the only thing here that could shrink painlessly:
//
//   264 x 264 x 2 = 139,392 B
//   208 x 208 x 2 =  86,528 B   -> saves 52,864 B
//
// The cost is a 1.26x upscale when blitting back (208 -> 262), slightly softer
// than 1:1. The overview is a "step back and see everything" mode and was never
// about detail; in practice it is acceptable.
//
// The transition animation is softer still at the start (scaling to nearly 468,
// 2.25x), but that lasts only 240 ms while the screen is moving -- barely
// distinguishable from the 264 version.
//
// Going lower has diminishing returns: 176 saves only another 24 KB while the
// upscale reaches 1.5x. If the assets still do not fit, the thing to change is
// VMEM_SIZE in gcc/Makefile (SHARED_SRAM still has about 519 KB unused), which
// genuinely adds memory instead of trading away image quality.
//
#define PSEL_THUMB_PX       (208)

img_obj_t g_sPselThumb =
{
    .bo = {0}, .w = PSEL_THUMB_PX, .h = PSEL_THUMB_PX, .stride = -1, .color = 0, .format = FB_COLOR_FORMAT, .sampling_mode = 0,
};

//*****************************************************************************
//
//! @brief Create a frame buffer in GPU memory
//!
//! @details Allocates a frame buffer for drawing from the GPU memory pool.
//! The buffer is created in SSRAM for best performance, and its size follows the
//! image dimensions and format passed in.
//!
//! @param image Pointer to the image object structure to initialize
//!
//! @return int 0 on success, -1 if the allocation fails
//!
//! @note The memory comes from the NEMA_MEM_POOL_FB pool
//!
//*****************************************************************************
int
framebuffer_create(img_obj_t* image)
{
    uint32_t size = nema_texture_size(image->format, 0, image->w, image->h);

    // Create the buffer in SSRAM
    image->bo = nema_buffer_create_pool(NEMA_MEM_POOL_FB, size);
    if ( image->bo.base_virt == NULL )
    {
        return -1;
    }
    else
    {
        return 0;
    }
}

//*****************************************************************************
//
//! @brief Load texture data from MRAM into SSRAM
//!
//! @details Copies a texture or image from MRAM (main memory) into SSRAM (GPU
//! memory) for best drawing performance. The function allocates from the assets
//! pool and performs cache-safe memory operations.
//!
//! @param image Pointer to the image object structure to fill in
//! @param ptr Pointer to the source texture data in MRAM
//!
//! @return int 0 on success, -1 if the allocation fails
//!
//! @note The memory comes from the NEMA_MEM_POOL_ASSETS pool, with a cache flush to keep the data coherent
//!
//*****************************************************************************
int
texture_load(img_obj_t* image, const unsigned char *ptr)
{
    uint32_t size = nema_texture_size(image->format, 0, image->w, image->h);

    // Create the buffer in SSRAM
    image->bo = nema_buffer_create_pool(NEMA_MEM_POOL_ASSETS, size);
    if ( image->bo.base_virt == NULL )
    {
        return -1;
    }

    // Do the memcpy and flush the cache.
    nema_memcpy(image->bo.base_virt, (void *)ptr, size);
    return 0;
}

//*****************************************************************************
//
//! @brief Load one texture from eMMC into SSRAM.
//!
//! @details The only difference from texture_load() is the source: that one
//! copies from a const array in MRAM, this one reads a file on eMMC. The
//! destination is the same SSRAM assets pool -- the GPU can sample neither MRAM
//! nor eMMC, so the copy in between is unavoidable.
//!
//! The cache must be flushed after the read, for the same reason as the tile copy
//! in map_task.c: FatFs may have moved the data with the CPU, leaving those bytes
//! in the D-cache while the GPU reads memory itself.
//!
//! @param image  The image object to fill in (w/h/format must be set beforehand).
//! @param pcName Asset file name (one of the ASSET_* macros).
//!
//! @return 0 on success, -1 on allocation failure, -2 on read failure.
//
//*****************************************************************************
static int
texture_load_from_store(img_obj_t *image, const char *pcName)
{
    uint32_t size = nema_texture_size(image->format, 0, image->w, image->h);
    am_hal_cachectrl_range_t sRange;

    image->bo = nema_buffer_create_pool(NEMA_MEM_POOL_ASSETS, size);
    if ( image->bo.base_virt == NULL )
    {
        return -1;
    }

    if ( !AssetStoreRead(pcName, image->bo.base_virt, size) )
    {
        //
        // A failed read must return the buffer.
        //
        // This used to return -2 directly, leaving the allocation unclaimed in the
        // pool. And this path is not a rare error case but the entirely normal
        // state of "the asset has not been uploaded to eMMC yet".
        //
        // It was hit once, and the cost was larger than expected: while trying a
        // fourth watch face those five .BIN files were not yet on eMMC, five
        // failures leaked about 460 KB, the pool ran dry, the framebuffer
        // allocation then failed, nobody was checking that, the GPU was bound to a
        // NULL address -> fault -> the whole system (BLE advertising included)
        // stopped. One missing free was enough to make the board look bricked.
        //
        nema_buffer_destroy(&image->bo);

        //
        // Explicitly cleared to NULL: the drawing side decides whether an asset
        // exists from base_virt (see face_render()), and it cannot be left to
        // chance whether nema_buffer_destroy() clears it for us.
        //
        image->bo.base_virt = NULL;
        image->bo.base_phys = 0;

        return -2;
    }

    sRange.ui32StartAddr = image->bo.base_phys;
    sRange.ui32Size      = size;
    am_hal_cachectrl_dcache_clean(&sRange);

    return 0;
}

//*****************************************************************************
//
//! @brief Load every texture the GPU has to sample
//!
//! @details The home image and the watch face assets are read from eMMC into the
//! SSRAM graphics pool. Any failure is logged and leaves that page without its
//! image, but nothing stops -- with eMMC unmountable, the watch is still a watch
//! that tells the time (the hands are vector-drawn and need no assets).
//!
//! @return None
//
//*****************************************************************************
static void
render_assets_load(void)
{
    static const struct
    {
        img_obj_t  *psImage;
        const char *pcName;
        const char *pcLabel;
    } sList[] =
    {
        { &g_sHomeImage,  ASSET_HOME,      "home"        },
        { &g_sWatchface,  ASSET_WATCH_BG,  "watchface"   },
        { &g_sHourhand,   ASSET_HAND_HOUR, "hour hand"   },
        { &g_sMinuteHand, ASSET_HAND_MIN,  "minute hand" },
        { &g_sSecondHand, ASSET_HAND_SEC,  "second hand" },

        //
        // The Ambiq face (which took aurora's slots). Missing files are handled
        // like any other asset: one log line, that face draws black, while the
        // selector and the other two faces work as usual -- someone who has not
        // uploaded WF4*.BIN yet does not lose the watch as well.
        //
        // A failed read now returns the buffer (see texture_load_from_store()).
        // Without that free, these five files missing from eMMC leaked about
        // 460 KB, the framebuffer allocation then failed and the whole system
        // faulted -- which did happen.
        //
        { &g_sFace4Dial,  ASSET_FACE4_BG,   "face4 dial"   },
        { &g_sFace4Hour,  ASSET_FACE4_HOUR, "face4 hour"   },
        { &g_sFace4Min,   ASSET_FACE4_MIN,  "face4 minute" },
        { &g_sFace4Sec,   ASSET_FACE4_SEC,  "face4 second" },
        { &g_sFace4Cap,   ASSET_FACE4_CAP,  "face4 cap"    },

        { &g_sFace3Dial,  ASSET_FACE3_BG,   "face3 dial"   },
        { &g_sFace3Hour,  ASSET_FACE3_HOUR, "face3 hour"   },
        { &g_sFace3Min,   ASSET_FACE3_MIN,  "face3 minute" },
        { &g_sFace3Sec,   ASSET_FACE3_SEC,  "face3 second" }
    };

    uint32_t ui32Idx;
    uint32_t ui32Bytes = 0;
    uint32_t ui32Fail  = 0;

    for ( ui32Idx = 0; ui32Idx < (sizeof(sList) / sizeof(sList[0])); ui32Idx++ )
    {
        img_obj_t *psImg = sList[ui32Idx].psImage;
        uint32_t   ui32Sz;
        int        i32Res;

        ui32Sz = nema_texture_size(psImg->format, 0, psImg->w, psImg->h);
        i32Res = texture_load_from_store(psImg, sList[ui32Idx].pcName);

        if ( i32Res == 0 )
        {
            ui32Bytes += ui32Sz;
        }
        else
        {
            ui32Fail++;

            //
            // Print the size on failure too. Judging whether the pool is large
            // enough needs "how big is this one", and nema_texture_size() is not
            // necessarily w * h * bpp -- that assumption made me miscompute the
            // capacity of the fourth watch face once (see the VMEM_SIZE note in
            // gcc/Makefile).
            //
            am_util_debug_printf("[render] %s texture %s (%u B)\n",
                                 sList[ui32Idx].pcLabel,
                                 (i32Res == -1) ? "alloc failed"
                                                : "read failed",
                                 (unsigned)ui32Sz);
        }
    }

    //
    // Total consumed by the assets. This line turns "how much pool is left" into
    // something measured rather than estimated: check this number before adding a
    // new asset, and confirm it grew afterwards.
    //
    am_util_debug_printf("[render] textures: %u loaded (%u B), %u failed\n",
                         (unsigned)((sizeof(sList) / sizeof(sList[0])) - ui32Fail),
                         (unsigned)ui32Bytes,
                         (unsigned)ui32Fail);

    //
    // The font no longer has to be moved into SSRAM. The old bitmap font was a
    // texture the GPU sampled directly and so had to be copied (358 KB) into the
    // assets pool like an image; with a vector font the glyphs are expanded into
    // paths by the CPU from const data in MRAM and handed to the GPU, with no
    // texture to move.
    //
}

//*****************************************************************************
//
//! @brief Create the NemaVG paths and shared paint of the three control centre icons.
//!
//! @details Called once by RenderTask after nema_vg_init(), next to bt_icon_init().
//! The vertex data is const and set once; after that each frame only changes the
//! matrix and the colour.
//
//*****************************************************************************
static void
cc_icons_init(void)
{
    g_hCcIconPaint   = nema_vg_paint_create();
    g_hFlashIconPath = nema_vg_path_create();
    g_hWatchIconPath = nema_vg_path_create();
    g_hTempIconPath  = nema_vg_path_create();

    //
    // The brightness panel's sun icon is created here too: it is the same kind of
    // thing as the three control centre icons (const vertex data, the shared
    // g_hCcIconPaint, only the matrix and colour change), so there is no reason
    // for a separate init function.
    //
    g_hSunIconPath   = nema_vg_path_create();

    if ( (g_hCcIconPaint == NULL) || (g_hFlashIconPath == NULL) ||
         (g_hWatchIconPath == NULL) || (g_hTempIconPath == NULL) ||
         (g_hSunIconPath == NULL) )
    {
        am_util_debug_printf("[render] cc icons: vg object create failed\n");
        return;
    }

    nema_vg_path_set_shape(g_hSunIconPath,
                           sizeof(g_pui8SunIconSegs) / sizeof(g_pui8SunIconSegs[0]),
                           g_pui8SunIconSegs,
                           sizeof(g_pfSunIconData) / sizeof(g_pfSunIconData[0]),
                           g_pfSunIconData);

    nema_vg_path_set_shape(g_hFlashIconPath,
                           sizeof(g_pui8FlashIconSegs) / sizeof(g_pui8FlashIconSegs[0]),
                           g_pui8FlashIconSegs,
                           sizeof(g_pfFlashIconData) / sizeof(g_pfFlashIconData[0]),
                           g_pfFlashIconData);

    nema_vg_path_set_shape(g_hWatchIconPath,
                           sizeof(g_pui8WatchIconSegs) / sizeof(g_pui8WatchIconSegs[0]),
                           g_pui8WatchIconSegs,
                           sizeof(g_pfWatchIconData) / sizeof(g_pfWatchIconData[0]),
                           g_pfWatchIconData);

    nema_vg_path_set_shape(g_hTempIconPath,
                           sizeof(g_pui8TempIconSegs) / sizeof(g_pui8TempIconSegs[0]),
                           g_pui8TempIconSegs,
                           sizeof(g_pfTempIconData) / sizeof(g_pfTempIconData[0]),
                           g_pfTempIconData);

    nema_vg_paint_set_type(g_hCcIconPaint, NEMA_VG_PAINT_COLOR);
}

//*****************************************************************************
//
//! @brief Draw one icon path at the given position.
//!
//! @details The path itself sits at the origin and the position comes only from a
//! matrix translation, as in draw_bt_icon(). The stroke style is set centrally by
//! the caller (see cc_draw()); this only changes position and colour.
//!
//! @param hPath     The path to draw; skipped entirely when NULL
//! @param i32X      Top-left x
//! @param i32Y      Top-left y
//! @param ui32Color Line colour (ABGR)
//
//*****************************************************************************
static void
cc_draw_icon_path(NEMA_VG_PATH_HANDLE hPath, int32_t i32X, int32_t i32Y,
                  uint32_t ui32Color)
{
    nema_matrix3x3_t matrix;

    if ( (hPath == NULL) || (g_hCcIconPaint == NULL) )
    {
        return;
    }

    nema_mat3x3_load_identity(matrix);
    nema_mat3x3_translate(matrix, (float)i32X, (float)i32Y);
    nema_vg_path_set_matrix(hPath, matrix);

    nema_vg_paint_set_paint_color(g_hCcIconPaint, ui32Color);
    nema_vg_draw_path(hPath, g_hCcIconPaint);
}

//*****************************************************************************
//
//! @brief Draw the extra circle an icon needs (the watch dial, the thermometer bulb).
//!
//! @param fCx       Centre x (absolute coordinates)
//! @param fCy       Centre y (absolute coordinates)
//! @param fR        Radius
//! @param ui32Color Line colour (ABGR)
//
//*****************************************************************************
static void
cc_draw_icon_circle(float fCx, float fCy, float fR, uint32_t ui32Color)
{
    nema_matrix3x3_t matrix;

    if ( g_hCcIconPaint == NULL )
    {
        return;
    }

    nema_mat3x3_load_identity(matrix);

    nema_vg_paint_set_paint_color(g_hCcIconPaint, ui32Color);
    nema_vg_draw_circle(fCx, fCy, fR, matrix, g_hCcIconPaint);
}

//*****************************************************************************
//
//! @brief Draw the home page's pull-down control centre.
//!
//! @details The caller must already have bound the frame buffer and command list,
//! and the background must already be drawn -- the panel and the cells are
//! semi-transparent and composite over the layer below with NEMA_BL_SIMPLE.
//!
//! Drawing order: panel -> four cells -> four icons. The first two use NemaGFX
//! fills and the icons use NemaVG strokes; mixing the two APIs in one command
//! list is fine, but the VG stroke style has to be reset after switching over and
//! cannot inherit leftovers from the previous frame (same reason as
//! draw_bt_icon(): other drawing in the same CL rewrites the VG context).
//
//*****************************************************************************
static void
cc_draw(void)
{
    static const struct
    {
        uint8_t ui8Col;
        uint8_t ui8Row;
    } sTileGrid[CC_ACT_COUNT] =
    {
        { 0, 0 },   // CC_ACT_LED2
        { 1, 0 },   // CC_ACT_LED4
        { 0, 1 },   // CC_ACT_GO_WATCH
        { 1, 1 }    // CC_ACT_GO_TEMP
    };

    uint32_t ui32Idx;

    //
    // The semi-transparent panel background. NEMA_BL_SIMPLE blends by alpha with
    // the layer below, so the home image shows through a little rather than being
    // covered entirely.
    //
    nema_set_blend_fill(NEMA_BL_SIMPLE);
    nema_fill_rounded_rect(CC_PANEL_X, CC_PANEL_Y,
                           CC_PANEL_W, CC_PANEL_H,
                           CC_PANEL_R, CC_PANEL_COLOR);

    //
    // The backgrounds of the four cells. Toggle cells change colour with the LED's current state; navigation cells keep one colour.
    //
    for ( ui32Idx = 0; ui32Idx < CC_ACT_COUNT; ui32Idx++ )
    {
        uint32_t ui32TileColor;

        switch ( ui32Idx )
        {
            case CC_ACT_LED2:
                ui32TileColor = LedTaskGetState(LED_ID_LED2) ? CC_TILE_ON_COLOR
                                                             : CC_TILE_OFF_COLOR;
                break;

            case CC_ACT_LED4:
                ui32TileColor = LedTaskGetState(LED_ID_LED4) ? CC_TILE_ON_COLOR
                                                             : CC_TILE_OFF_COLOR;
                break;

            default:
                ui32TileColor = CC_TILE_NAV_COLOR;
                break;
        }

        nema_fill_rounded_rect(CC_TILE_X(sTileGrid[ui32Idx].ui8Col),
                               CC_TILE_Y(sTileGrid[ui32Idx].ui8Row),
                               CC_TILE_W, CC_TILE_H,
                               CC_TILE_R, ui32TileColor);
    }

    //
    // Switch to NemaVG for the icons. The stroke style is the same for all four, so it is set once.
    //
    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_STROKE);
    nema_vg_stroke_set_width(CC_ICON_STROKE_W);
    nema_vg_stroke_set_cap_style(NEMA_VG_CAP_ROUND, NEMA_VG_CAP_ROUND);
    nema_vg_stroke_set_join_style(NEMA_VG_JOIN_ROUND);

    for ( ui32Idx = 0; ui32Idx < CC_ACT_COUNT; ui32Idx++ )
    {
        int32_t  i32X = CC_TILE_X(sTileGrid[ui32Idx].ui8Col) + CC_ICON_DX;
        int32_t  i32Y = CC_TILE_Y(sTileGrid[ui32Idx].ui8Row) + CC_ICON_DY;
        uint32_t ui32Color;

        switch ( ui32Idx )
        {
            case CC_ACT_LED2:
                ui32Color = LedTaskGetState(LED_ID_LED2) ? CC_ICON_ON_COLOR
                                                         : CC_ICON_OFF_COLOR;
                cc_draw_icon_path(g_hFlashIconPath, i32X, i32Y, ui32Color);
                break;

            case CC_ACT_LED4:
                ui32Color = LedTaskGetState(LED_ID_LED4) ? CC_ICON_ON_COLOR
                                                         : CC_ICON_OFF_COLOR;
                cc_draw_icon_path(g_hFlashIconPath, i32X, i32Y, ui32Color);
                break;

            case CC_ACT_GO_WATCH:
                cc_draw_icon_path(g_hWatchIconPath, i32X, i32Y, CC_ICON_OFF_COLOR);

                //
                // The dial: (12,12) in the viewBox, radius 6.
                //
                cc_draw_icon_circle((float)i32X + CC_S(12.0f),
                                    (float)i32Y + CC_S(12.0f),
                                    CC_S(6.0f),
                                    CC_ICON_OFF_COLOR);
                break;

            default:
                cc_draw_icon_path(g_hTempIconPath, i32X, i32Y, CC_ICON_OFF_COLOR);

                //
                // The bulb: (12,18.5) in the viewBox, radius 3.
                //
                cc_draw_icon_circle((float)i32X + CC_S(12.0f),
                                    (float)i32Y + CC_S(18.5f),
                                    CC_S(3.0f),
                                    CC_ICON_OFF_COLOR);
                break;
        }
    }
}

//*****************************************************************************
//
//! @brief Draw the quick page menu summoned by a right swipe on the home page.
//!
//! @details The frame reuses the brightness panel, holding a trend polyline and a
//! GPS satellite as two vertically stacked buttons. The caller must already have
//! drawn the home page and bound the frame buffer and command list.
//
//*****************************************************************************
static void
page_menu_draw_trend_icon(float fCx, float fCy)
{
    static const float pfPointDx[] = { -32.0f, -10.0f, 8.0f, 36.0f };
    static const float pfPointDy[] = {  18.0f,  -6.0f, 7.0f, -24.0f };
    nema_matrix3x3_t matrix;
    uint32_t ui32Idx;

    nema_mat3x3_load_identity(matrix);

    // The axes.
    nema_vg_set_fill_rule(NEMA_VG_STROKE);
    nema_vg_paint_set_paint_color(g_hCcIconPaint, PAGE_MENU_ICON_COLOR);
    nema_vg_draw_line(fCx - 42.0f, fCy - 30.0f,
                      fCx - 42.0f, fCy + 30.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx - 42.0f, fCy + 30.0f,
                      fCx + 42.0f, fCy + 30.0f,
                      matrix, g_hCcIconPaint);

    // The trend polyline and its four data points.
    nema_vg_paint_set_paint_color(g_hCcIconPaint, PAGE_MENU_ACCENT_COLOR);
    for ( ui32Idx = 1; ui32Idx < 4U; ui32Idx++ )
    {
        nema_vg_draw_line(fCx + pfPointDx[ui32Idx - 1U],
                          fCy + pfPointDy[ui32Idx - 1U],
                          fCx + pfPointDx[ui32Idx],
                          fCy + pfPointDy[ui32Idx],
                          matrix, g_hCcIconPaint);
    }

    nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);
    for ( ui32Idx = 0; ui32Idx < 4U; ui32Idx++ )
    {
        nema_vg_draw_circle(fCx + pfPointDx[ui32Idx],
                            fCy + pfPointDy[ui32Idx],
                            4.0f, matrix, g_hCcIconPaint);
    }
}

static void
page_menu_draw_gps_icon(float fCx, float fCy)
{
    nema_matrix3x3_t matrix;

    nema_mat3x3_load_identity(matrix);

    // The satellite body, the solar panels on each side and the booms.
    nema_vg_set_fill_rule(NEMA_VG_STROKE);
    nema_vg_paint_set_paint_color(g_hCcIconPaint, PAGE_MENU_ICON_COLOR);

    nema_vg_draw_line(fCx - 13.0f, fCy - 18.0f,
                      fCx + 13.0f, fCy - 18.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx + 13.0f, fCy - 18.0f,
                      fCx + 13.0f, fCy + 18.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx + 13.0f, fCy + 18.0f,
                      fCx - 13.0f, fCy + 18.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx - 13.0f, fCy + 18.0f,
                      fCx - 13.0f, fCy - 18.0f,
                      matrix, g_hCcIconPaint);

    nema_vg_draw_line(fCx - 45.0f, fCy - 13.0f,
                      fCx - 17.0f, fCy - 13.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx - 17.0f, fCy - 13.0f,
                      fCx - 17.0f, fCy + 13.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx - 17.0f, fCy + 13.0f,
                      fCx - 45.0f, fCy + 13.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx - 45.0f, fCy + 13.0f,
                      fCx - 45.0f, fCy - 13.0f,
                      matrix, g_hCcIconPaint);

    nema_vg_draw_line(fCx + 17.0f, fCy - 13.0f,
                      fCx + 45.0f, fCy - 13.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx + 45.0f, fCy - 13.0f,
                      fCx + 45.0f, fCy + 13.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx + 45.0f, fCy + 13.0f,
                      fCx + 17.0f, fCy + 13.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx + 17.0f, fCy + 13.0f,
                      fCx + 17.0f, fCy - 13.0f,
                      matrix, g_hCcIconPaint);

    nema_vg_draw_line(fCx - 17.0f, fCy,
                      fCx - 13.0f, fCy,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx + 13.0f, fCy,
                      fCx + 17.0f, fCy,
                      matrix, g_hCcIconPaint);

    // The GPS antenna and its three signal beams.
    nema_vg_draw_line(fCx, fCy - 18.0f,
                      fCx, fCy - 29.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx, fCy - 29.0f,
                      fCx - 11.0f, fCy - 40.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx, fCy - 29.0f,
                      fCx, fCy - 44.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx, fCy - 29.0f,
                      fCx + 11.0f, fCy - 40.0f,
                      matrix, g_hCcIconPaint);

    // Cell divisions on the solar panels, in thinner lines so the small icon does not blur together.
    nema_vg_stroke_set_width(PAGE_MENU_DETAIL_STROKE);
    nema_vg_draw_line(fCx - 31.0f, fCy - 13.0f,
                      fCx - 31.0f, fCy + 13.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx - 45.0f, fCy,
                      fCx - 17.0f, fCy,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx + 31.0f, fCy - 13.0f,
                      fCx + 31.0f, fCy + 13.0f,
                      matrix, g_hCcIconPaint);
    nema_vg_draw_line(fCx + 17.0f, fCy,
                      fCx + 45.0f, fCy,
                      matrix, g_hCcIconPaint);

    // The satellite's GPS receiver is accented in the same cyan as the brightness panel.
    nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);
    nema_vg_paint_set_paint_color(g_hCcIconPaint, PAGE_MENU_ACCENT_COLOR);
    nema_vg_draw_circle(fCx, fCy, 6.0f,
                        matrix, g_hCcIconPaint);
}

static void
page_menu_draw(void)
{
    uint32_t ui32Idx;

    nema_set_blend_fill(NEMA_BL_SIMPLE);
    nema_fill_rounded_rect(PAGE_MENU_PANEL_X, PAGE_MENU_PANEL_Y,
                           PAGE_MENU_PANEL_W, PAGE_MENU_PANEL_H,
                           PAGE_MENU_PANEL_R, PAGE_MENU_PANEL_COLOR);

    for ( ui32Idx = 0; ui32Idx < PAGE_MENU_ACT_COUNT; ui32Idx++ )
    {
        nema_fill_rounded_rect(PAGE_MENU_TILE_X, PAGE_MENU_TILE_Y(ui32Idx),
                               PAGE_MENU_TILE_W, PAGE_MENU_TILE_H,
                               PAGE_MENU_TILE_R, PAGE_MENU_TILE_COLOR);
    }

    if ( g_hCcIconPaint == NULL )
    {
        return;
    }

    //
    // The VG context may have been changed by another page, so the stroke style is reset in full here every frame.
    //
    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_stroke_set_width(PAGE_MENU_ICON_STROKE);
    nema_vg_stroke_set_cap_style(NEMA_VG_CAP_ROUND, NEMA_VG_CAP_ROUND);
    nema_vg_stroke_set_join_style(NEMA_VG_JOIN_ROUND);

    page_menu_draw_trend_icon(PAGE_MENU_ICON_CX,
                              PAGE_MENU_ICON_CY(PAGE_MENU_ACT_TREND));
    page_menu_draw_gps_icon(PAGE_MENU_ICON_CX,
                            PAGE_MENU_ICON_CY(PAGE_MENU_ACT_MAP));
}

//*****************************************************************************
//
//! @brief Convert a brightness value into the filled height of the slider.
//!
//! @details The settable brightness range is BACKLIGHT_LEVEL_MIN to MAX (the
//! lower bound is not 0, see backlight.h), so the denominator is that interval
//! and not 0 to 255. Using 0 to 255 would leave about 9% of white in the slider
//! at the darkest setting, which looks like it "cannot go all the way down".
//!
//! @return The filled height in px, 0 to BL_SLIDER_H
//
//*****************************************************************************
static int32_t
bl_level_to_fill_h(uint8_t ui8Level)
{
    int32_t i32Span = (int32_t)BACKLIGHT_LEVEL_MAX - (int32_t)BACKLIGHT_LEVEL_MIN;
    int32_t i32Rel  = (int32_t)ui8Level - (int32_t)BACKLIGHT_LEVEL_MIN;

    if ( i32Rel < 0 )
    {
        i32Rel = 0;
    }
    else if ( i32Rel > i32Span )
    {
        i32Rel = i32Span;
    }

    return (i32Rel * BL_SLIDER_H) / i32Span;
}

//*****************************************************************************
//
//! @brief Draw the brightness panel.
//!
//! @details The caller must already have bound the frame buffer and command list,
//! and the background must already be drawn -- the panel is semi-transparent and
//! composites with NEMA_BL_SIMPLE. All three pages call it.
//!
//! Drawing order: panel -> glow -> track -> fill -> sun icon, i.e. bottom to top.
//!
//! The filled part is drawn as "clip + the whole capsule" rather than as a
//! separate shorter rounded rectangle: that way the bottom corner radius would
//! shrink with the height, and at low brightness the bottom of the capsule would
//! turn from an arc into a square corner.
//! Clipping to the lower section first and then drawing a full white capsule
//! leaves exactly "a rounded bottom with a horizontal cut at the top" -- as in the design.
//
//*****************************************************************************
static void
bl_draw(void)
{
    uint8_t ui8Level  = backlight_get_level();
    int32_t i32FillH  = bl_level_to_fill_h(ui8Level);
    int32_t i32FillTop = BL_SLIDER_BOTTOM - i32FillH;
    int32_t i32Idx;

    //
    // The semi-transparent panel background.
    //
    nema_set_blend_fill(NEMA_BL_SIMPLE);
    nema_fill_rounded_rect(BL_PANEL_X, BL_PANEL_Y,
                           BL_PANEL_W, BL_PANEL_H,
                           BL_PANEL_R, BL_PANEL_COLOR);

    //
    // The cyan glow around the slider. Drawn outside in, each layer one step
    // smaller with double the alpha, which stacks into a gradient spreading outward.
    //
    for ( i32Idx = BL_GLOW_LAYERS; i32Idx >= 1; i32Idx-- )
    {
        int32_t i32Inset = i32Idx * BL_GLOW_STEP;
        uint8_t ui8Alpha = (uint8_t)(BL_GLOW_ALPHA << (BL_GLOW_LAYERS - i32Idx));

        nema_fill_rounded_rect(BL_SLIDER_X - i32Inset,
                               BL_SLIDER_Y - i32Inset,
                               BL_SLIDER_W + (2 * i32Inset),
                               BL_SLIDER_H + (2 * i32Inset),
                               BL_SLIDER_R + i32Inset,
                               BL_GLOW_COLOR(ui8Alpha));
    }

    //
    // The track (the unfilled part). The whole capsule is drawn and the lower section is covered in white shortly after.
    //
    // The blend stays NEMA_BL_SIMPLE (set on the panel line) rather than switching
    // to NEMA_BL_SRC: the track and the fill are both opaque and the interiors come
    // out identical either way, but the antialiased pixels along the rounded edges
    // need to blend with what is underneath to look smooth; SRC would write those
    // semi-transparent edge pixels straight over and leave the capsule ends jagged.
    //
    nema_fill_rounded_rect(BL_SLIDER_X, BL_SLIDER_Y,
                           BL_SLIDER_W, BL_SLIDER_H,
                           BL_SLIDER_R, BL_TRACK_COLOR);

    //
    // The filled part.
    //
    if ( i32FillH > 0 )
    {
        nema_set_clip(BL_SLIDER_X, i32FillTop, BL_SLIDER_W, i32FillH);

        nema_fill_rounded_rect(BL_SLIDER_X, BL_SLIDER_Y,
                               BL_SLIDER_W, BL_SLIDER_H,
                               BL_SLIDER_R, BL_FILL_COLOR);

        //
        // Restore the clip. Icons are still to be drawn and the caller submits this
        // CL afterwards -- leaving the clip on that small slider area would make
        // everything drawn later in the same frame disappear.
        //
        nema_set_clip(0, 0, FB_RESX, FB_RESY);
    }

    //
    // The sun icon.
    //
    // Its colour depends on whether the white fill has reached it: the position is
    // fixed, and at very low brightness the fill has not risen this far, where a
    // dark grey icon over the dark grey track would be invisible.
    //
    if ( g_hSunIconPath != NULL )
    {
        bool     bOnFill  = (i32FillTop <= (BL_SUN_CY - (int32_t)(BL_SUN_SIZE / 2.0f)));
        uint32_t ui32Color = bOnFill ? BL_SUN_ON_COLOR : BL_SUN_OFF_COLOR;

        float fX = (float)BL_SLIDER_X + ((BL_SLIDER_W - BL_SUN_SIZE) / 2.0f);
        float fY = (float)BL_SUN_CY - (BL_SUN_SIZE / 2.0f);

        nema_vg_set_blend(NEMA_BL_SIMPLE);
        nema_vg_set_fill_rule(NEMA_VG_STROKE);
        nema_vg_stroke_set_width(BL_SUN_STROKE_W);
        nema_vg_stroke_set_cap_style(NEMA_VG_CAP_ROUND, NEMA_VG_CAP_ROUND);
        nema_vg_stroke_set_join_style(NEMA_VG_JOIN_ROUND);

        cc_draw_icon_path(g_hSunIconPath, (int32_t)fX, (int32_t)fY, ui32Color);

        //
        // The solid circle at the centre. cc_draw_icon_circle() draws a stroked
        // ring (the fill rule was set to NEMA_VG_STROKE above), and a solid one is
        // wanted here, so the fill rule is switched once locally.
        //
        if ( g_hCcIconPaint != NULL )
        {
            nema_matrix3x3_t matrix;

            nema_mat3x3_load_identity(matrix);
            nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);
            nema_vg_paint_set_paint_color(g_hCcIconPaint, ui32Color);
            nema_vg_draw_circle(fX + BL_SUN_S(12.0f),
                                fY + BL_SUN_S(12.0f),
                                BL_SUN_DOT_R,
                                matrix, g_hCcIconPaint);
        }
    }
}

//*****************************************************************************
//
//! @brief Draw a travelling bright spot on the home page horizon.
//!
//! @details The spot is several concentric capsules stacked up: wide and faint on
//! the outside, narrow and bright inside, added together into a glow that is
//! bright in the middle and falls off softly to the sides. One rectangle cannot
//! do this -- the GPU's fill has no gradient, and a hard-edged block over a
//! gradient background is obvious at a glance.
//!
//! This page already re-blits the whole 454x454 image every frame, which is where
//! the cost lies; ten thin rectangle fills are negligible next to it, so the layer
//! count is chosen for how soft it looks, not for saving GPU time.
//!
//! @param fb The target frame buffer, used to work out the home image's centring offset
//!
//! @return None
//
//*****************************************************************************
static void
home_glow_draw(img_obj_t* fb)
{
    //
    // Half-width, half-height and intensity of each layer. Outside in, the size
    // shrinks faster than the intensity, so the stack ends up as a bright core
    // with a glow drawn out to the sides rather than an even bright bar.
    //
    // The proportions are deliberately flattened (240x16 for the widest layer):
    // the core of the horizon itself is only a few pixels thick, and a spot drawn
    // anywhere near circular would read as fog floating above the horizon rather
    // than the same light.
    //
    // Ten layers was also tuned: at five the boundaries between layers are visible
    // as steps, and beyond ten the improvement in softness is no longer noticeable.
    //
    static const struct
    {
        int32_t i32HalfW;
        int32_t i32HalfH;
        uint8_t ui8Alpha;
    }
    sLayers[] =
    {
        { 120, 8, 0x08 },
        { 100, 7, 0x0a },
        {  84, 6, 0x0d },
        {  68, 5, 0x11 },
        {  54, 5, 0x16 },
        {  42, 4, 0x1e },
        {  32, 4, 0x28 },
        {  22, 3, 0x36 },
        {  14, 3, 0x48 },
        {   7, 2, 0x5c }
    };

    float fDeg;
    float fSin;
    float fAbsSin;
    float fFade;
    int32_t i32Cx;
    int32_t i32Cy;
    uint32_t ui32Idx;

    //
    // The phase is derived straight from the tick with no stored state: leaving the
    // home page and returning puts the spot where it should be "now" rather than
    // jumping back to where it was left.
    //
    // The modulo comes before the multiply, so the angle always falls in 0 to 360,
    // matching nema_sin()'s convention (see the same note in draw_temp()).
    //
    fDeg = (float)(TICKS_TO_MS(xTaskGetTickCount()) % HOME_GLOW_PERIOD_MS) *
           (360.0f / (float)HOME_GLOW_PERIOD_MS);

    fSin    = nema_sin(fDeg);
    fAbsSin = (fSin < 0.0f) ? -fSin : fSin;

    //
    // The same centring expression as the nema_blit() in draw_home(), so the two line up.
    //
    i32Cx = ((fb->w - g_sHomeImage.w) / 2) + HOME_GLOW_IMG_X +
            (int32_t)(HOME_GLOW_AMP * fSin);
    i32Cy = ((fb->h - g_sHomeImage.h) / 2) + HOME_GLOW_IMG_Y;

    fFade = 1.0f - (HOME_GLOW_EDGE_FADE * fAbsSin);

    //
    // NEMA_BL_ADD is src * 1 + dst * 1: the source alpha takes no part in the
    // colour arithmetic, and the alpha field of a fill is effectively ignored in
    // this mode. So each layer's intensity is multiplied into the RGB on the CPU
    // side first (equivalent to premultiplied alpha) before the GPU adds it.
    //
    // NEMA_BLOP_SRC_PREMULT is not used to have the hardware do it, because that
    // flag's documentation only discusses texture sources and says nothing about
    // whether it applies to a constant fill colour; if it did not, all ten layers
    // would add at full intensity and the whole horizon would blow out to white.
    // One float multiply is a cheap price for removing that uncertainty.
    //
    nema_set_blend_fill(NEMA_BL_ADD);

    for ( ui32Idx = 0; ui32Idx < (sizeof(sLayers) / sizeof(sLayers[0])); ui32Idx++ )
    {
        uint32_t ui32Scale = (uint32_t)((float)sLayers[ui32Idx].ui8Alpha * fFade);

        if ( ui32Scale == 0 )
        {
            continue;
        }

        //
        // The corner radius is half the height, so both ends are semicircles -- light should not have corners.
        //
        nema_fill_rounded_rect(i32Cx - sLayers[ui32Idx].i32HalfW,
                               i32Cy - sLayers[ui32Idx].i32HalfH,
                               sLayers[ui32Idx].i32HalfW * 2,
                               sLayers[ui32Idx].i32HalfH * 2,
                               sLayers[ui32Idx].i32HalfH,
                               CC_ABGR(0xff,
                                       (HOME_GLOW_R * ui32Scale) / 255U,
                                       (HOME_GLOW_G * ui32Scale) / 255U,
                                       (HOME_GLOW_B * ui32Scale) / 255U));
    }
}

//*****************************************************************************
//
//! @brief Draw the home page -- a single full-screen image.
//!
//! @details The image is 454x454 and the panel 468x468, so the frame buffer is
//! cleared to black first and the image blitted centred. The source is RGB565
//! without alpha, so a plain source copy is enough. Afterwards home_glow_draw()
//! adds a travelling bright spot on the horizon at the bottom, so this page is no
//! longer a still image.
//!
//! @param fb The target frame buffer
//! @param cl The command list that records the drawing commands
//!
//! @return None
//
//*****************************************************************************
static void
draw_home(img_obj_t* fb, nema_cmdlist_t* cl)
{
    //
    // Reset and bind the command list. The CL is created once in RenderTask() and
    // only rewound here, so there is no per-frame memory leak.
    //
    nema_cl_rewind(cl);
    nema_cl_bind_circular(cl);

    //
    // Bind the target frame buffer.
    //
    nema_bind_dst_tex(fb->bo.base_phys,
                      fb->w,
                      fb->h,
                      fb->format,
                      fb->stride);

    nema_set_clip(0, 0, fb->w, fb->h);

    //
    // Clear to opaque black, so the border around the image is unambiguously the background colour.
    //
    nema_clear(0xff000000);

    //
    // Bind the source image.
    //
    nema_bind_src_tex(g_sHomeImage.bo.base_phys,
                      g_sHomeImage.w,
                      g_sHomeImage.h,
                      g_sHomeImage.format,
                      g_sHomeImage.stride,
                      NEMA_FILTER_PS);

    //
    // Blit it centred.
    //
    nema_set_blend_blit(NEMA_BL_SRC);
    nema_blit((fb->w - g_sHomeImage.w) / 2, (fb->h - g_sHomeImage.h) / 2);

    //
    // The sweeping light on the horizon. Above the image and below the overlays:
    // with the control centre or the brightness panel open, the light still flows
    // beneath them rather than over the panel.
    //
    home_glow_draw(fb);

    //
    // The control centre sits above the home image. Nothing is drawn when closed, so this page costs the same as before the revision.
    //
    if ( g_bMenuOpen )
    {
        cc_draw();
    }

    //
    // The home page right-swipe quick menu. ui_input_update() guarantees the three overlays are mutually exclusive.
    //
    if ( g_bPageMenuOpen )
    {
        page_menu_draw();
    }

    //
    // The brightness panel. Mutually exclusive with the two home page menus, so they are never drawn together.
    //
    if ( g_bBlOpen )
    {
        bl_draw();
    }

    //
    // Submit to the GPU; the caller is responsible for nema_cl_wait().
    //
    nema_cl_submit(cl);
}

//*****************************************************************************
//
//! @brief Draw a rotated hand at the given position
//!
//! @details Uses NemaGFX matrix arithmetic to rotate and translate the hand
//! image. The function builds the rotation matrix, transforms the four corners of
//! the hand, then draws the rotated hand with a texture binding and a quad blit.
//!
//! @param img Pointer to the image object of the hand
//! @param x0 Starting x of the hand (asset coordinate system, unscaled)
//! @param y0 Starting y of the hand (asset coordinate system, unscaled)
//! @param angle Rotation angle in radians (negative is clockwise)
//! @param cx x of the rotation pivot
//! @param cy y of the rotation pivot
//! @param scale Scale factor, 1.0f being the original size. Coordinates and sizes
//!              scale together, so the caller always passes x0/y0 in asset
//!              coordinates and never converts by hand
//!
//! @return None
//!
//! @note Uses NemaGFX matrix arithmetic for better rotation performance
//!
//*****************************************************************************
static void
draw_hand( img_obj_t *img, float x0, float y0, float angle, int cx, int cy,
           float scale )
{
    //
    // The offset and the size scale together, so the four corners keep their
    // original proportions and the pivot still falls on the same point of the asset.
    //
    float fW = (float)img->w * scale;
    float fH = (float)img->h * scale;

    x0 *= scale;
    y0 *= scale;

    float x1 = x0 + fW,  y1 = y0;
    float x2 = x0 + fW,  y2 = y0 + fH;
    float x3 = x0     ,  y3 = y0 + fH;

    //
    // Compute the rotation matrix
    //
    nema_matrix3x3_t m;
    nema_mat3x3_load_identity(m);
    nema_mat3x3_rotate(m, -angle);
    nema_mat3x3_translate(m, cx, cy);

    //
    // Rotate the four corners
    //
    nema_mat3x3_mul_vec(m, &x0, &y0);
    nema_mat3x3_mul_vec(m, &x1, &y1);
    nema_mat3x3_mul_vec(m, &x2, &y2);
    nema_mat3x3_mul_vec(m, &x3, &y3);

    //
    // Draw the hand
    //
    nema_bind_src_tex( img->bo.base_phys, img->w, img->h, img->format, img->stride, NEMA_FILTER_BL);
    nema_blit_quad_fit(x0, y0,
                       x1, y1,
                       x2, y2,
                       x3, y3);
}

//*****************************************************************************
//
//! @brief Draw the weather icon.
//
//*****************************************************************************
#if WATCHFACE_SHOW_WEATHER_ICON
static void draw_vg_icon(const uint8_t * icon)
{
    //
    // Set the global matrix
    //
    uint32_t svg_width;
    uint32_t svg_height;
    nema_vg_get_tsvg_resolution(icon, &svg_width, &svg_height);

    float scale_x = ICON_WIDTH  / (float)svg_width;
    float scale_y = ICON_HEIGHT / (float)svg_height;

    nema_matrix3x3_t matrix;
    nema_mat3x3_load_identity(matrix);
    nema_mat3x3_scale(matrix, scale_x, scale_y);
    nema_mat3x3_translate(matrix, ICON_POS_X, ICON_POS_Y);
    nema_vg_set_global_matrix(matrix);

    nema_vg_set_blend(NEMA_BL_SIMPLE);

    //
    // Draw the icon
    //
    nema_vg_draw_tsvg(icon);

    //
    // Restore the global matrix
    //
    nema_vg_reset_global_matrix();

}
#endif // WATCHFACE_SHOW_WEATHER_ICON

//*****************************************************************************
//
//! @brief Create the NemaVG paths of the Bluetooth icon and the pairing badge, and the paint shared by the temperature page.
//!
//! @details Called once by RenderTask after nema_vg_init(). The paths' vertex
//! data is const and set once; after that each frame only changes the matrix, the
//! colour, and draws.
//!
//! Only one paint is created and shared by the whole page (icon, gauge, badge and
//! text all use it), with the colour reset before each draw. One paint each would
//! only use more memory: every draw on this page is issued in order within the
//! same command list, so two colours are never needed at once.
//
//*****************************************************************************
static void
bt_icon_init(void)
{
    g_hBtIconPath   = nema_vg_path_create();
    g_hBtIconPaint  = nema_vg_paint_create();
    g_hBadgeOkPath  = nema_vg_path_create();
    g_hBadgeErrPath = nema_vg_path_create();

    if ( (g_hBtIconPath == NULL) || (g_hBtIconPaint == NULL) ||
         (g_hBadgeOkPath == NULL) || (g_hBadgeErrPath == NULL) )
    {
        am_util_debug_printf("[render] bt icon: vg object create failed\n");
        return;
    }

    nema_vg_path_set_shape(g_hBtIconPath,
                           sizeof(g_pui8BtIconSegs) / sizeof(g_pui8BtIconSegs[0]),
                           g_pui8BtIconSegs,
                           sizeof(g_pfBtIconData) / sizeof(g_pfBtIconData[0]),
                           g_pfBtIconData);

    nema_vg_path_set_shape(g_hBadgeOkPath,
                           sizeof(g_pui8BadgeOkSegs) / sizeof(g_pui8BadgeOkSegs[0]),
                           g_pui8BadgeOkSegs,
                           sizeof(g_pfBadgeOkData) / sizeof(g_pfBadgeOkData[0]),
                           g_pfBadgeOkData);

    nema_vg_path_set_shape(g_hBadgeErrPath,
                           sizeof(g_pui8BadgeErrSegs) / sizeof(g_pui8BadgeErrSegs[0]),
                           g_pui8BadgeErrSegs,
                           sizeof(g_pfBadgeErrData) / sizeof(g_pfBadgeErrData[0]),
                           g_pfBadgeErrData);

    nema_vg_paint_set_type(g_hBtIconPaint, NEMA_VG_PAINT_COLOR);

    //
    // The vector font is bound once and stays; the size is what changes per draw.
    // The font data is const, expanded into paths by the CPU and handed to the
    // GPU, so unlike a bitmap font it need not be moved into SSRAM first.
    //
    nema_vg_bind_font(&DejaVuSans_ttf_kern);
}

//*****************************************************************************
//
//! @brief Draw one icon path of the temperature page (the Bluetooth polyline, the pairing badge).
//!
//! @details The path itself sits at the origin and the position comes only from a
//! matrix translation; the stroke style and the colour are both reset here rather
//! than assuming leftovers in the VG context.
//!
//! Why reset every time: earlier in the same command list a TSVG weather icon or
//! the gauge arc may just have been drawn, and nema_vg_draw_tsvg() changes the
//! fill rule, stroke width and cap / join according to the attributes inside the
//! TSVG, while nema_vg_draw_ring() also uses the stroke width and cap -- that is,
//! the context is shared and gets rewritten. Conversely, what is set here does
//! not affect anyone else, because those places set their own too.
//!
//! @param hPath     The path to draw; skipped entirely when NULL
//! @param fX        Top-left x
//! @param fY        Top-left y
//! @param fStrokeW  Line width
//! @param ui32Color Line colour (ABGR)
//
//*****************************************************************************
static void
draw_temp_icon_path(NEMA_VG_PATH_HANDLE hPath, float fX, float fY,
                    float fStrokeW, uint32_t ui32Color)
{
    nema_matrix3x3_t matrix;

    if ( (hPath == NULL) || (g_hBtIconPaint == NULL) )
    {
        return;
    }

    nema_mat3x3_load_identity(matrix);
    nema_mat3x3_translate(matrix, fX, fY);
    nema_vg_path_set_matrix(hPath, matrix);

    //
    // NEMA_BL_SIMPLE rather than NEMA_BL_SRC_OVER: what the VG rasterizer produces
    // is a non-premultiplied colour with an alpha of coverage x paint alpha, and
    // SIMPLE (Sa*Sa + Da*(1-Sa)) is the matching composition. SRC_OVER multiplies
    // by alpha one time fewer and comes out too bright where it is semi-transparent
    // -- the two agree when opaque, so only the two halo rings show the difference.
    //
    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_STROKE);
    nema_vg_stroke_set_width(fStrokeW);

    //
    // Corresponds to the SVG stroke-linecap / stroke-linejoin="round". Both ends
    // and the corners of the polyline have to be round for the icon to look the
    // way it does on a phone.
    //
    nema_vg_stroke_set_cap_style(NEMA_VG_CAP_ROUND, NEMA_VG_CAP_ROUND);
    nema_vg_stroke_set_join_style(NEMA_VG_JOIN_ROUND);

    nema_vg_paint_set_paint_color(g_hBtIconPaint, ui32Color);
    nema_vg_draw_path(hPath, g_hBtIconPaint);
}

//*****************************************************************************
//
//! @brief Draw a circle: either a stroked ring or a solid dot.
//!
//! @param fCx       Centre x
//! @param fCy       Centre y
//! @param fR        Radius
//! @param fStrokeW  Stroke width; ignored when bFilled is true
//! @param bFilled   true draws a solid circle, false draws only the outline
//! @param ui32Color Colour (ABGR)
//
//*****************************************************************************
static void
draw_temp_circle(float fCx, float fCy, float fR, float fStrokeW,
                 bool bFilled, uint32_t ui32Color)
{
    nema_matrix3x3_t matrix;

    if ( g_hBtIconPaint == NULL )
    {
        return;
    }

    nema_mat3x3_load_identity(matrix);

    nema_vg_set_blend(NEMA_BL_SIMPLE);

    //
    // Whether nema_vg_draw_circle() produces a ring or a solid disc depends on the
    // current fill rule: NEMA_VG_STROKE outlines only, NEMA_VG_FILL_NON_ZERO fills the interior.
    //
    if ( bFilled )
    {
        nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);
    }
    else
    {
        nema_vg_set_fill_rule(NEMA_VG_STROKE);
        nema_vg_stroke_set_width(fStrokeW);
        nema_vg_stroke_set_cap_style(NEMA_VG_CAP_BUTT, NEMA_VG_CAP_BUTT);
    }

    nema_vg_paint_set_paint_color(g_hBtIconPaint, ui32Color);
    nema_vg_draw_circle(fCx, fCy, fR, matrix, g_hBtIconPaint);
}

//*****************************************************************************
//
//! @brief Draw the Bluetooth icon, with a halo or a connection dot depending on state.
//!
//! @details The caller must already have bound the frame buffer and command list.
//!
//! How the icon differs across the five states (following the design):
//!
//!   not connected    grey-blue icon, no decoration
//!   pairing          cyan icon with two semi-transparent haloes around it
//!   connected        cyan icon with a small solid dot at the top right
//!   pairing passed   cyan icon, no decoration (the result is carried by the green badge below)
//!   pairing failed   grey-blue icon (the connection is no longer usable); the red badge carries the result
//!
//! @param ui32Color Icon colour (ABGR)
//! @param bGlow     true draws the two haloes (pairing)
//! @param bDot      true draws the connection dot (connected)
//
//*****************************************************************************
static void
draw_bt_icon(uint32_t ui32Color, bool bGlow, bool bDot)
{
    //
    // The haloes are drawn first, so the icon sits on top of them.
    //
    if ( bGlow )
    {
        draw_temp_circle(BT_ICON_CX, BT_ICON_CY, BT_GLOW_IN_R, BT_GLOW_IN_W,
                         false,
                         TEMP_WITH_ALPHA(TEMP_ACCENT_COLOR, BT_GLOW_IN_ALPHA));
        draw_temp_circle(BT_ICON_CX, BT_ICON_CY, BT_GLOW_OUT_R, BT_GLOW_OUT_W,
                         false,
                         TEMP_WITH_ALPHA(TEMP_ACCENT_COLOR, BT_GLOW_OUT_ALPHA));
    }

    draw_temp_icon_path(g_hBtIconPath, BT_ICON_POS_X, BT_ICON_POS_Y,
                        BT_ICON_STROKE_W, ui32Color);

    if ( bDot )
    {
        draw_temp_circle(BT_ICON_CX + BT_DOT_DX, BT_ICON_CY + BT_DOT_DY,
                         BT_DOT_R, 0.0f, true, ui32Color);
    }
}

//*****************************************************************************
//
// Description of a watch face.
//
// A watch face is "one dial + three hands + three pivots". Gathering them into
// one record rather than scattering them across globals in draw_watch() is
// because every item of the second face differs from the first: a 360x360 dial
// against 454x454, 32 px wide hands against 15 px, and completely different
// pivots. One shared set of hard-coded constants would only turn into a pile of
// if (face == 0).
//
// bFit is "whether to scale up to fill the panel in full-screen view". The first
// face was drawn for 454 px, within 3% of the 468 panel, where a 1:1 blit is
// sharpest (see the WATCH_DIAL_FIT section); the second is 360 px and would
// leave 54 px empty all round unless filled.
//
// The hands' scale factor is not stored here: it has to follow the dial and is
// derived by face_render() from the dial's actual factor. Stored separately, the
// two would eventually disagree, with the hands visibly shrunken against the ticks.
//
//*****************************************************************************
typedef struct
{
    const char *pcName;

    img_obj_t  *psDial;
    img_obj_t  *psHour;
    img_obj_t  *psMin;
    img_obj_t  *psSec;

    //
    // The three pivots, with the same meaning as PIVOT_*: the offset of the asset's top-left corner from the rotation centre (negative).
    //
    int16_t     i16HourX;
    int16_t     i16HourY;
    int16_t     i16MinX;
    int16_t     i16MinY;
    int16_t     i16SecX;
    int16_t     i16SecY;

    //
    // The centre cap; NULL means this face does not need one.
    //
    // Blitted at the rotation centre by face_render() **after** the three hands, so
    // it needs no pivot -- it does not rotate, and the asset's centre is the pivot.
    //
    // chrono and numbers are NULL: their hands each carry a counterweight disc at
    // the tail, and the three stacked together form the hub. The Ambiq face needs
    // one, and the reason is in the design -- the cap in the reference composite is
    // painted above the hands to cover the hubs, while the one on the dial is simply
    // covered by the hands.
    //
    img_obj_t  *psCap;

    bool        bFit;
} watchface_t;

static const watchface_t g_sFaces[] =
{
    {
        "chrono",
        &g_sWatchface, &g_sHourhand, &g_sMinuteHand, &g_sSecondHand,
        PIVOT_HOUR_X,    PIVOT_HOUR_Y,
        PIVOT_MIN_X,     PIVOT_MIN_Y,
        PIVOT_SEC_X,     PIVOT_SEC_Y,
        NULL,
        (WATCH_DIAL_FIT != 0)
    },
    {
        //
        // The official Ambiq Apollo510B style face. A 454x454 dial with cyan
        // circuit traces and glowing ticks around the rim.
        //
        // **This slot used to be aurora** (the deep blue face with the 390 dial).
        // It was replaced rather than added as a fourth face, because the GPU pool
        // cannot hold four: nema_texture_size() is not w * h * bpp, estimating with
        // the latter undercounts, and the pool is a NOLOAD array, so too small a
        // value produces no link error at all and only blows up at runtime. See the
        // VMEM_SIZE note in gcc/Makefile.
        //
        // aurora gave up its slot because its dial is the smallest (390 vs 454) and
        // the difference is therefore smallest. Its assets are still on eMMC and
        // PIVOT_F2_* remain, so switching back only means changing this entry.
        //
        // This is the only face that needs a centre cap (psCap); see watchface_t.
        //
        // The assets are converted from src/generated/Ambiq_Watch_face/ by
        // tools/face4_prep.py, which does two things the source images do not:
        // repairs a vertical hand ghost at the centre of the dial, and centres on
        // the knob rather than the outer circle. Both are explained in its header.
        //
        "ambiq",
        &g_sFace4Dial, &g_sFace4Hour, &g_sFace4Min, &g_sFace4Sec,
        PIVOT_F4_HOUR_X, PIVOT_F4_HOUR_Y,
        PIVOT_F4_MIN_X,  PIVOT_F4_MIN_Y,
        PIVOT_F4_SEC_X,  PIVOT_F4_SEC_Y,
        &g_sFace4Cap,
        false
    },
    {
        //
        // The blue numeral face. A 454x454 dial with 3D numerals 1 to 12.
        //
        // The hands share their artwork with aurora, but are a separate asset set:
        // those three were drawn for the 390 dial, and these have already been
        // multiplied by 454/390 by tools/face3_prep.py. Baking the upscale into the
        // asset instead of letting the GPU scale every frame is sharper with LANCZOS
        // than with bilinear, and spares this code an extra factor.
        //
        "numbers",
        &g_sFace3Dial, &g_sFace3Hour, &g_sFace3Min, &g_sFace3Sec,
        PIVOT_F3_HOUR_X, PIVOT_F3_HOUR_Y,
        PIVOT_F3_MIN_X,  PIVOT_F3_MIN_Y,
        PIVOT_F3_SEC_X,  PIVOT_F3_SEC_Y,
        NULL,
        false
    }
};

#define WATCHFACE_COUNT     (sizeof(g_sFaces) / sizeof(g_sFaces[0]))

//
// The currently selected watch face. Both the face page and the selector read it; only the selector writes it when applying.
//
// It always starts at 0 at boot: this choice has nowhere persistent to live yet
// (eMMC currently holds assets only, no configuration file). Once there is one,
// change where this initial value comes from and nothing else.
//
static uint32_t g_ui32Face = 0;

//*****************************************************************************
//
//! @brief Draw one watch face into the currently bound target.
//!
//! @details It only draws; it does not touch the command list or submit -- the
//! caller may be drawing the whole screen (draw_watch()) or a small snapshot for
//! the selector (face_snapshot()), and those two have entirely different targets
//! and lifetimes.
//!
//! It does not clear the background either: a full-screen face usually covers the
//! whole screen and an extra full-area fill is wasted; whether to clear and to
//! what colour is left to the caller, which knows what it is drawing.
//!
//! @param psFace Which face to draw.
//! @param i32CX  Where the dial's centre lands (screen coordinates). The hands rotate about this point too.
//! @param i32CY  The same, vertically.
//! @param fScale The dial's factor relative to the asset's original size. 1.0 is
//!               1:1, which takes point sampling -- without resampling, the ticks
//!               and the thin outer ring are sharpest.
//! @param fSec   The time to display, in seconds.
//!
//! @return None
//
//*****************************************************************************
static void
face_render(const watchface_t *psFace, int32_t i32CX, int32_t i32CY,
            float fScale, float fSec)
{
    img_obj_t *psDial = psFace->psDial;
    float      fAngle;
    float      fDialW;
    float      fDialH;
    int32_t    i32X;
    int32_t    i32Y;

    //
    // Nothing is drawn when the assets did not load (this face's files are not on
    // eMMC yet). A missing image is better than a screenful of uninitialized
    // memory -- the latter looks like a crash.
    //
    if ( psDial->bo.base_virt == NULL )
    {
        return;
    }

    fDialW = (float)psDial->w * fScale;
    fDialH = (float)psDial->h * fScale;

    i32X = (int32_t)((float)i32CX - fDialW / 2.0f + 0.5f);
    i32Y = (int32_t)((float)i32CY - fDialH / 2.0f + 0.5f);

    nema_bind_src_tex(psDial->bo.base_phys,
                      psDial->w,
                      psDial->h,
                      psDial->format,
                      psDial->stride,
                      (fScale == 1.0f) ? NEMA_FILTER_PS : NEMA_FILTER_BL);

    nema_set_blend_blit(NEMA_BL_SRC); /* Preserve the RGB565 palette. */

    if ( fScale == 1.0f )
    {
        nema_blit(i32X, i32Y);
    }
    else
    {
        nema_blit_rect_fit(i32X, i32Y, (int32_t)(fDialW + 0.5f),
                           (int32_t)(fDialH + 0.5f));
    }

    //
    // Draw the weather icon (off by default, see WATCHFACE_SHOW_WEATHER_ICON)
    //
#if WATCHFACE_SHOW_WEATHER_ICON
    draw_vg_icon(icon_to_draw);
#endif

    //
    // Set the blend mode for the hands.
    //
    nema_set_blend_blit(NEMA_BL_SIMPLE);

    //
    // The three hands. The pivots are passed in asset coordinates and draw_hand()
    // applies the factor, so no conversion is needed here -- which is exactly why
    // that parameter exists.
    //
    fAngle = fSec / 3600.0f / 12.0f * 360.0f;
    draw_hand(psFace->psHour, psFace->i16HourX, psFace->i16HourY, -fAngle,
              i32CX, i32CY, fScale);

    fAngle = fSec / 60.0f / 60.0f * 360.0f;
    draw_hand(psFace->psMin, psFace->i16MinX, psFace->i16MinY, -fAngle,
              i32CX, i32CY, fScale);

    fAngle = fSec / 60.0f * 360.0f;
    draw_hand(psFace->psSec, psFace->i16SecX, psFace->i16SecY, -fAngle,
              i32CX, i32CY, fScale);

    //
    // The centre cap, covering the hubs of the three hands. Only the faces that
    // need one have it (psCap, see watchface_t) -- the hands of chrono and numbers
    // each carry a counterweight disc that stacks into a hub.
    //
    // It must come after the three hands: covering the hubs is its only reason to
    // exist. It is skipped when the asset did not load, matching the behaviour for
    // a missing hand asset (that one is not drawn, the rest continues).
    //
    if ( (psFace->psCap != NULL) && (psFace->psCap->bo.base_virt != NULL) )
    {
        img_obj_t *psCap = psFace->psCap;
        float      fCapW = (float)psCap->w * fScale;
        float      fCapH = (float)psCap->h * fScale;

        nema_bind_src_tex(psCap->bo.base_phys,
                          psCap->w,
                          psCap->h,
                          psCap->format,
                          psCap->stride,
                          (fScale == 1.0f) ? NEMA_FILTER_PS : NEMA_FILTER_BL);

        //
        // The cap's edge is feathered (a round asset in a square canvas), so it has
        // to be composited rather than overwritten -- NEMA_BL_SRC would write the
        // transparent edge pixels straight out as black, a dark ring on the dial.
        //
        nema_set_blend_blit(NEMA_BL_SIMPLE);

        //
        // It does not rotate, so there is no pivot: the asset's centre goes to the rotation centre.
        //
        nema_blit_rect_fit((int32_t)((float)i32CX - fCapW / 2.0f + 0.5f),
                           (int32_t)((float)i32CY - fCapH / 2.0f + 0.5f),
                           (int32_t)(fCapW + 0.5f),
                           (int32_t)(fCapH + 0.5f));
    }
}

//*****************************************************************************
//
//! @brief Get the factor at which a face fills a given width.
//!
//! @details Both the face page and the snapshot need this, and getting it wrong
//! (hands shrunken against the ticks) is not obvious at a glance, so it is one
//! function rather than the same division written in two places.
//
//*****************************************************************************
static float
face_fit_scale(const watchface_t *psFace, int32_t i32W)
{
    if ( (psFace->psDial->w <= 0) || !psFace->bFit )
    {
        return 1.0f;
    }

    return (float)i32W / (float)psFace->psDial->w;
}

//*****************************************************************************
//
//! @brief Print the parameters each face actually uses.
//!
//! @details Whether the hands sit correctly depends on three independent places
//! -- the assets on eMMC, the sizes in asset_store.h, and the pivots in this
//! file. Any mismatch just looks like "the hands are in a strange position" with
//! no indication of which one is at fault. These lines lay all three out at once.
//!
//! tail is how far the hand's tail is from the centre (pivot Y minus asset
//! height): positive means the tail stops above the centre with a gap to the
//! central knob, negative means it crosses the centre.
//!
//! **Not printed at boot**: the PSRAM timing scan emits hundreds of lines and
//! floods the RTT up buffer (NO_BLOCK_SKIP, full means dropped), so anything
//! behind it never gets out. It prints when a face is applied instead, by which
//! time RTT is idle.
//!
//! All three faces are printed rather than only the current one: that makes
//! "the others are right and only this one is wrong" visible, which is the most common case.
//
//*****************************************************************************
static void
faces_dump(void)
{
    uint32_t ui32Face;

    for ( ui32Face = 0; ui32Face < WATCHFACE_COUNT; ui32Face++ )
    {
        const watchface_t *psF = &g_sFaces[ui32Face];

        am_util_stdio_printf("[face%u] %s dial %dx%d %s fit=%d\n",
                             (unsigned)ui32Face, psF->pcName,
                             psF->psDial->w, psF->psDial->h,
                             (psF->psDial->bo.base_virt != NULL) ? "ok"
                                                                 : "MISSING",
                             (int)psF->bFit);

        am_util_stdio_printf("        hour %dx%d %s pivot(%d,%d) tail %d\n",
                             psF->psHour->w, psF->psHour->h,
                             (psF->psHour->bo.base_virt != NULL) ? "ok"
                                                                 : "MISSING",
                             -psF->i16HourX, -psF->i16HourY,
                             (-psF->i16HourY) - psF->psHour->h);

        am_util_stdio_printf("        min  %dx%d %s pivot(%d,%d) tail %d\n",
                             psF->psMin->w, psF->psMin->h,
                             (psF->psMin->bo.base_virt != NULL) ? "ok"
                                                                : "MISSING",
                             -psF->i16MinX, -psF->i16MinY,
                             (-psF->i16MinY) - psF->psMin->h);

        am_util_stdio_printf("        sec  %dx%d %s pivot(%d,%d) tail %d\n",
                             psF->psSec->w, psF->psSec->h,
                             (psF->psSec->bo.base_virt != NULL) ? "ok"
                                                                : "MISSING",
                             -psF->i16SecX, -psF->i16SecY,
                             (-psF->i16SecY) - psF->psSec->h);
    }
}

//*****************************************************************************
//
//! @brief Draw the complete watch face
//!
//! Draws the currently selected face over the whole screen, with the brightness panel on top.
//
//*****************************************************************************
void
draw_watch(nema_img_obj_t* fb, nema_cmdlist_t* cl, float sec)
{
    const watchface_t *psFace = &g_sFaces[g_ui32Face];

    //
    // Reset and bind the command list
    //
    nema_cl_rewind(cl);
    nema_cl_bind_circular(cl);

    //
    // Bind the target buffer
    //
    nema_bind_dst_tex(fb->bo.base_phys,
                      fb->w,
                      fb->h,
                      fb->format,
                      fb->stride);

    nema_set_clip(0, 0, fb->w, fb->h);

    //
    // When the dial does not fill the panel it must be cleared to black first, or
    // the border keeps the previous frame's remains. When it does fill, that
    // full-area fill is saved.
    //
    if ( !psFace->bFit )
    {
        nema_clear(0xff000000);
    }

    face_render(psFace, fb->w / 2, fb->h / 2,
                face_fit_scale(psFace, fb->w), sec);

    //
    // The brightness panel goes on top of the face.
    //
    if ( g_bBlOpen )
    {
        bl_draw();
    }

    //
    // Submit the command list
    //
    nema_cl_submit(cl);
}

//*****************************************************************************
//
//! @brief Timer callback that rotates the weather icon.
//
//*****************************************************************************
void icon_change(TimerHandle_t timer)
{
    static uint32_t count = 0;

    icon_to_draw = icon_array[count];

    count++;
    if ( count >= 3 )
    {
        count = 0;
    }
}

//*****************************************************************************
//
// The BLE display state of the temperature page.
//
// The design lists five "screen appearances", while underneath there are only two
// signals: the connection flag from ble_pair_conn_is_open() and the pairing state
// from ble_pair_state_get() (see ble_pair.h). This enum is what the screen
// actually shows once the two are combined, so each drawing function does not have
// to decide again what "connected but pairing failed" counts as.
//
//*****************************************************************************
typedef enum
{
    TEMP_BLE_DISCONNECTED = 0,  // BLE READY / DISCONNECTED
    TEMP_BLE_PAIRING,           // PAIRING CODE / six digits / CONFIRM THE CODE
    TEMP_BLE_CONNECTED,         // BLE READY / CONNECTED
    TEMP_BLE_PAIR_PASS,         // green tick / PAIRING SUCCESS / PAIRED
    TEMP_BLE_PAIR_FAIL          // red exclamation mark / PAIRING FAILED / PAIRING ERROR
} temp_ble_view_e;

//*****************************************************************************
//
//! @brief Decide which appearance to show from the BLE connection and pairing state.
//!
//! @details The order is "pairing outranks connection": while pairing is in
//! progress or a result has just arrived, that matters far more than "is a
//! connection up", since the user is looking at the dialog on their phone.
//!
//! A pairing Pass / Fail is only valid during the connection in which it
//! happened -- ble_pair_reset() is called on both DM_CONN_OPEN_IND and
//! DM_CONN_CLOSE_IND, so a result never survives into the next connection and no
//! expiry check is needed here.
//!
//! @param pui32Passkey When non-NULL, receives the six comparison digits; the
//!                     value is meaningless unless the return is TEMP_BLE_PAIRING
//!
//! @return The state to display
//
//*****************************************************************************
static temp_ble_view_e
temp_ble_view_get(uint32_t *pui32Passkey)
{
    uint32_t         ui32Passkey = 0;
    ble_pair_state_e ePairState  = ble_pair_state_get(&ui32Passkey);

    if ( pui32Passkey != NULL )
    {
        *pui32Passkey = ui32Passkey;
    }

    switch ( ePairState )
    {
        case BLE_PAIR_COMPARE:
            return TEMP_BLE_PAIRING;

        case BLE_PAIR_PASS:
            return TEMP_BLE_PAIR_PASS;

        case BLE_PAIR_FAIL:
            return TEMP_BLE_PAIR_FAIL;

        default:
            break;
    }

    return ble_pair_conn_is_open() ? TEMP_BLE_CONNECTED : TEMP_BLE_DISCONNECTED;
}

//*****************************************************************************
//
//! @brief Linearly interpolate between two ABGR colours.
//!
//! @details Used by the gauge gradient. All four channels interpolate, alpha
//! included -- both ends are opaque today, but leaving one channel out would
//! produce an unexplainable colour shift the day they become semi-transparent.
//!
//! @param ui32From The colour at fT = 0
//! @param ui32To   The colour at fT = 1
//! @param fT       The position; the caller guarantees 0 to 1
//!
//! @return The interpolated colour (ABGR)
//
//*****************************************************************************
static uint32_t
temp_color_lerp(uint32_t ui32From, uint32_t ui32To, float fT)
{
    uint32_t ui32Out = 0;
    uint32_t ui32Shift;

    for ( ui32Shift = 0; ui32Shift < 32; ui32Shift += 8 )
    {
        float fA = (float)((ui32From >> ui32Shift) & 0xffU);
        float fB = (float)((ui32To   >> ui32Shift) & 0xffU);
        uint32_t ui32Ch = (uint32_t)(fA + ((fB - fA) * fT) + 0.5f);

        ui32Out |= (ui32Ch & 0xffU) << ui32Shift;
    }

    return ui32Out;
}

//*****************************************************************************
//
//! @brief Draw one segment of the gauge arc.
//!
//! @details The start and end angles are given unreduced, i.e. values above 360
//! are allowed -- the gauge counts from 12 o'clock (270 degrees) and is bound to
//! cross 0 at some point. This function folds that into the form
//! nema_vg_draw_ring() accepts.
//!
//! Why fold it here: the library is binary-only and there is no way to confirm
//! how it handles arguments outside 0 to 360 or a start above the end (every SDK
//! example using draw_ring is increasing and in range). Rather than bet on its
//! behaviour, it is fed exactly one well-defined form: start below end, both in
//! 0 to 360. A segment crossing 0 is split into two calls; the seam is at 0
//! degrees and both halves share a colour, so it does not show.
//!
//! @param fDeg0     Start angle (may exceed 360)
//! @param fDeg1     End angle; must be greater than fDeg0 and the span at most 360
//! @param ui32Color Colour (ABGR)
//
//*****************************************************************************
static void
draw_temp_arc_seg(float fDeg0, float fDeg1, uint32_t ui32Color)
{
    //
    // A span too small to see is not drawn. Besides saving a draw, this rejects the
    // degenerate case of a start equal to the end -- where "the whole circle" and
    // "empty" would be indistinguishable.
    //
    if ( (fDeg1 - fDeg0) < 0.05f )
    {
        return;
    }

    //
    // Both ends shift together, preserving the span. A loop subtracting rather than
    // fmodf(): the start is at most 270 + 360 = 630, so one subtraction is enough
    // and math.h need not be pulled in for it.
    //
    while ( fDeg0 >= 360.0f )
    {
        fDeg0 -= 360.0f;
        fDeg1 -= 360.0f;
    }

    nema_vg_paint_set_paint_color(g_hBtIconPaint, ui32Color);

    if ( fDeg1 > 360.0f )
    {
        nema_vg_draw_ring(TEMP_DIAL_CX, TEMP_DIAL_CY, TEMP_ARC_R,
                          fDeg0, 360.0f, g_hBtIconPaint);

        if ( (fDeg1 - 360.0f) >= 0.05f )
        {
            nema_vg_draw_ring(TEMP_DIAL_CX, TEMP_DIAL_CY, TEMP_ARC_R,
                              0.0f, fDeg1 - 360.0f, g_hBtIconPaint);
        }
    }
    else
    {
        nema_vg_draw_ring(TEMP_DIAL_CX, TEMP_DIAL_CY, TEMP_ARC_R,
                          fDeg0, fDeg1, g_hBtIconPaint);
    }
}

//*****************************************************************************
//
//! @brief Draw the temperature page gauge: the full track, the gradient arc, 12 ticks and the outer hairline.
//!
//! @details The caller must already have bound the frame buffer and command list.
//!
//! The order is "track -> arc -> ticks -> rim", i.e. bottom to top. The ticks come
//! after the arc because their radii do not overlap (ticks at 176 to 192, arc
//! centreline at 206), so the order makes no difference to the result; it is
//! arranged this way only to match the program order to the visual layering.
//!
//! @param fFill The filled fraction; the caller guarantees 0 to 1. On a sensor
//!              fault it is 0, leaving only the track rather than an angle that
//!              would look like a real value.
//
//*****************************************************************************
static void
draw_temp_gauge(float fFill)
{
    int32_t i32Idx;

    if ( g_hBtIconPaint == NULL )
    {
        return;
    }

    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_STROKE);
    nema_vg_stroke_set_width(TEMP_ARC_W);

    //
    // Both ends of the ring always use butt caps: adjacent segments have to join seamlessly, and round caps would bulge at the seam.
    //
    nema_vg_stroke_set_cap_style(NEMA_VG_CAP_BUTT, NEMA_VG_CAP_BUTT);

    //
    // The full track. Split into two half-circles for draw_temp_arc_seg(); see that function.
    //
    draw_temp_arc_seg(TEMP_ARC_START_DEG,
                      TEMP_ARC_START_DEG + 180.0f,
                      TEMP_TRACK_COLOR);
    draw_temp_arc_seg(TEMP_ARC_START_DEG + 180.0f,
                      TEMP_ARC_START_DEG + 360.0f,
                      TEMP_TRACK_COLOR);

    //
    // The filled arc, segmented into a gradient.
    //
    if ( fFill > 0.0f )
    {
        float fStep = (fFill * TEMP_ARC_SWEEP_DEG) / (float)TEMP_ARC_SEGMENTS;

        for ( i32Idx = 0; i32Idx < TEMP_ARC_SEGMENTS; i32Idx++ )
        {
            float fA0 = TEMP_ARC_START_DEG + ((float)i32Idx * fStep);

            //
            // Each segment overlaps the next by 0.4 degrees, so the antialiasing at
            // the seam does not leave a thin line. The colours are opaque and the
            // overlap simply overwrites without darkening. The last segment does not
            // add it -- nothing follows to join, and the extra 0.4 degrees would
            // overshoot the current value.
            //
            float fA1 = fA0 + fStep +
                        ((i32Idx < (TEMP_ARC_SEGMENTS - 1)) ? 0.4f : 0.0f);

            draw_temp_arc_seg(fA0, fA1,
                              temp_color_lerp(TEMP_ACCENT_COLOR,
                                              TEMP_ACCENT_END,
                                              ((float)i32Idx + 0.5f) /
                                                  (float)TEMP_ARC_SEGMENTS));
        }
    }

    //
    // The 12 ticks. Fixed positions that do not follow the value -- they are part of the dial, not an indicator.
    //
    {
        nema_matrix3x3_t matrix;

        nema_mat3x3_load_identity(matrix);
        nema_vg_stroke_set_width(TEMP_TICK_W);
        nema_vg_paint_set_paint_color(g_hBtIconPaint, TEMP_TICK_COLOR);

        for ( i32Idx = 0; i32Idx < TEMP_TICK_COUNT; i32Idx++ )
        {
            float fDeg = TEMP_ARC_START_DEG +
                         ((360.0f / (float)TEMP_TICK_COUNT) * (float)i32Idx);
            float fCos;
            float fSin;

            //
            // Reduce to 0 to 360. nema_sin() / nema_cos() are binary-only and it is
            // not certain whether they range-reduce out-of-range angles themselves,
            // so reducing first is the simplest option.
            //
            while ( fDeg >= 360.0f )
            {
                fDeg -= 360.0f;
            }

            fCos = nema_cos(fDeg);
            fSin = nema_sin(fDeg);

            nema_vg_draw_line(TEMP_DIAL_CX + (TEMP_TICK_R_IN * fCos),
                              TEMP_DIAL_CY + (TEMP_TICK_R_IN * fSin),
                              TEMP_DIAL_CX + (TEMP_TICK_R_OUT * fCos),
                              TEMP_DIAL_CY + (TEMP_TICK_R_OUT * fSin),
                              matrix, g_hBtIconPaint);
        }
    }

    //
    // The outermost hairline, framing the screen.
    //
    draw_temp_circle(TEMP_DIAL_CX, TEMP_DIAL_CY, TEMP_RIM_R, TEMP_RIM_W,
                     false, TEMP_RIM_COLOR);
}

//*****************************************************************************
//
//! @brief Compute the width available to one row inside the round panel.
//!
//! @details The length of a horizontal line inside a circle varies with its
//! distance from the centre, shrinking towards the top and bottom. The return is
//! the maximum width at which this row's text stays inside TEMP_TEXT_SAFE_R.
//!
//! It takes whichever of the text's top and bottom edges is further from the
//! centre, not the centreline: text has height, and the centreline overestimates
//! -- row y = 386 gives 214 px by the centreline but only 191 px in reality, and
//! "CONFIRM THE CODE" falls right inside those 23 px.
//!
//! @param fCy   The centre y of this row
//! @param fSize The font size of this row
//!
//! @return The available width in px. Returns 0 when the whole row lies outside the safe radius.
//
//*****************************************************************************
static float
temp_row_max_w(float fCy, float fSize)
{
    float fDy = nema_absf(fCy - TEMP_DIAL_CY) + TEMP_ROW_HALF_H(fSize);

    if ( fDy >= TEMP_TEXT_SAFE_R )
    {
        return 0.0f;
    }

    return 2.0f * nema_sqrt((TEMP_TEXT_SAFE_R * TEMP_TEXT_SAFE_R) -
                            (fDy * fDy));
}

//*****************************************************************************
//
//! @brief Draw one horizontally centred line of vector text, shrinking it when too wide.
//!
//! @details The vertical position given is "the centre y of this line" rather than
//! the top of the text block: the five states have different numbers of rows, and
//! centreline layout lets each be specified independently without deriving it from
//! the row above.
//!
//! fSize is "how large it should be". When the measured width exceeds the row's
//! chord it shrinks proportionally -- on a round panel whatever overflows is
//! simply clipped, and since the font sizes and string lengths still follow the
//! design, hand arithmetic would eventually miss a row ("CONFIRM THE CODE" was
//! missed once).
//!
//! @param pcStr     The string to draw
//! @param fSize     The desired font size (pixels)
//! @param fCy       The centre y of this line
//! @param ui32Color Text colour (ABGR)
//
//*****************************************************************************
static void
draw_temp_line(const char *pcStr, float fSize, float fCy, uint32_t ui32Color)
{
    float fMaxW = temp_row_max_w(fCy, fSize);
    float fW    = 0.0f;
    float fH    = 0.0f;
    float fBoxH;

    if ( g_hBtIconPaint == NULL )
    {
        return;
    }

    //
    // A font's glyphs are closed outlines meant to be filled, so the fill rule must
    // be non-zero; leaving the NEMA_VG_STROKE set earlier for the arc would only
    // outline the glyphs (hollow text).
    //
    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);

    nema_vg_set_font_size(fSize);
    (void)nema_vg_string_get_bbox(pcStr, &fW, &fH, (float)FB_RESX, 0);

    //
    // Text width is proportional to the font size, so the scale factor is simply the
    // width ratio. Shrinking makes the row shorter and the available width slightly
    // wider, but it is not recomputed -- the extra headroom is kept as margin, and
    // there is no worry about an iteration failing to converge.
    //
    // fW <= 0 means the measurement failed (a problem with the font or the VG
    // context); nothing is shrunk then and it is drawn centred at the original size:
    // at least it is still a centred line, rather than infinite scaling from a 0.
    //
    if ( (fW > fMaxW) && (fW > 1.0f) && (fMaxW > 1.0f) )
    {
        fSize *= fMaxW / fW;
    }

    fBoxH = TEMP_TEXT_BOX_H(fSize);

    nema_vg_set_font_size(fSize);
    nema_vg_paint_set_paint_color(g_hBtIconPaint, ui32Color);
    nema_vg_print(g_hBtIconPaint, pcStr,
                  0.0f, fCy - (fBoxH / 2.0f),
                  (float)FB_RESX, fBoxH,
                  NEMA_VG_ALIGNX_CENTER | NEMA_VG_ALIGNY_CENTER,
                  NULL);
}

//*****************************************************************************
//
//! @brief Draw the large temperature digits and the "degC".
//!
//! @details The two are centred as one unit, so each width is measured before the
//! start is computed -- centring the digits and appending the unit to the right
//! would push the whole group further left the longer the unit is.
//!
//! The degree symbol is not a glyph: the font covers 0x20 to 0x7E and has no
//! degree sign. A small ring is drawn instead, whose position and size can be set
//! freely and aligns with the top of the digits better than the glyph would.
//!
//! After measuring, it shrinks once more to this row's chord (temp_row_max_w()),
//! for the reason in the comment below -- on this round panel, text that is too
//! wide is not ugly but simply clipped, and "tuning the font size to fit" cannot
//! guarantee it.
//!
//! @param pcValue The already formatted temperature string, e.g. "36.80"
//
//*****************************************************************************
static void
draw_temp_value(const char *pcValue)
{
    //
    // Besides the digits and the "C", this group has parts that do not scale with
    // the font size: the gap between the digits and the degree symbol, the diameter
    // of the degree ring, and the 3 px between that ring and the "C".
    //
    const float fFixedW = TEMP_UNIT_GAP + (2.0f * TEMP_DEG_R) + 3.0f;

    float fValFont  = TEMP_VALUE_FONT;
    float fUnitFont = TEMP_UNIT_FONT;
    float fValW  = 0.0f;
    float fValH  = 0.0f;
    float fUnitW = 0.0f;
    float fUnitH = 0.0f;
    float fGroupW;
    float fX;
    float fDegCx;

    if ( g_hBtIconPaint == NULL )
    {
        return;
    }

    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);

    //
    // Measure the widths. nema_vg_string_get_bbox() works from the current font
    // size, so the size has to be set before each of the two measurements.
    //
    nema_vg_set_font_size(fValFont);
    (void)nema_vg_string_get_bbox(pcValue, &fValW, &fValH, (float)FB_RESX, 0);

    nema_vg_set_font_size(fUnitFont);
    (void)nema_vg_string_get_bbox("C", &fUnitW, &fUnitH, (float)FB_RESX, 0);

    fGroupW = fValW + fFixedW + fUnitW;

    //
    // Too wide shrinks the whole group proportionally. The limit uses the same chord
    // calculation as the small status text (temp_row_max_w()), so "how wide the
    // circle allows" has exactly one definition.
    //
    // Why this exists: how the size given to nema_vg_set_font_size() maps to pixels
    // is not documented, and measurement puts it well above an em-based estimate. If
    // only TEMP_VALUE_FONT were "tuned to fit", changing the font or the number of
    // characters (a minus sign when the temperature drops below zero) would overflow
    // again -- and on a round panel whatever overflows is simply clipped.
    //
    // The scale factor is the width ratio. Text width is proportional to the size, so
    // the shrunk string is (fValW * fScale) wide; the fixed-width part does not shrink
    // with it, so the total comes out a little under the limit -- conservative, but it
    // never oscillates and needs no second measurement.
    //
    {
        float fMaxW = temp_row_max_w(TEMP_VALUE_CY, fValFont);

        if ( (fGroupW > fMaxW) && (fGroupW > 1.0f) && (fMaxW > 1.0f) )
        {
            float fScale = fMaxW / fGroupW;

            fValFont  *= fScale;
            fUnitFont *= fScale;
            fValW     *= fScale;
            fUnitW    *= fScale;

            fGroupW = fValW + fFixedW + fUnitW;
        }
    }

    //
    // On a failed measurement (0 or negative) it falls back to centring the whole group,
    // which at least stays on screen. If it ever happens there is a problem with the font
    // or the VG context, and one log line is easier to chase than drawing wrong in silence.
    //
    if ( fValW <= 0.0f )
    {
        am_util_debug_printf("[render] temp value bbox invalid\n");
        draw_temp_line(pcValue, TEMP_VALUE_FONT, TEMP_VALUE_CY,
                       TEMP_VALUE_COLOR);
        return;
    }

    fX = TEMP_DIAL_CX - (fGroupW / 2.0f);

    //
    // The digits. Left-aligned at the computed start, vertically centred on TEMP_VALUE_CY.
    //
    nema_vg_set_font_size(fValFont);
    nema_vg_paint_set_paint_color(g_hBtIconPaint, TEMP_VALUE_COLOR);
    nema_vg_print(g_hBtIconPaint, pcValue,
                  fX,
                  TEMP_VALUE_CY - (TEMP_TEXT_BOX_H(fValFont) / 2.0f),
                  (float)FB_RESX, TEMP_TEXT_BOX_H(fValFont),
                  NEMA_VG_ALIGNX_LEFT | NEMA_VG_ALIGNY_CENTER,
                  NULL);

    //
    // The degree symbol. Placed above the digits' centreline as a superscript, matching
    // the design. The rise is a fraction of the font size, so the symbol moves back
    // towards the centreline when the size shrinks.
    //
    fDegCx = fX + fValW + TEMP_UNIT_GAP + TEMP_DEG_R;
    draw_temp_circle(fDegCx,
                     TEMP_VALUE_CY - (fValFont * TEMP_DEG_RISE_RATIO),
                     TEMP_DEG_R, TEMP_DEG_W, false, TEMP_UNIT_COLOR);

    //
    // The "C". Slightly below the digits' centreline, so "degC" reads as one superscript group.
    //
    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);
    nema_vg_set_font_size(fUnitFont);
    nema_vg_paint_set_paint_color(g_hBtIconPaint, TEMP_UNIT_COLOR);
    nema_vg_print(g_hBtIconPaint, "C",
                  fDegCx + TEMP_DEG_R + 3.0f,
                  (TEMP_VALUE_CY + (fValFont * TEMP_UNIT_DROP_RATIO)) -
                      (TEMP_TEXT_BOX_H(fUnitFont) / 2.0f),
                  (float)FB_RESX, TEMP_TEXT_BOX_H(fUnitFont),
                  NEMA_VG_ALIGNX_LEFT | NEMA_VG_ALIGNY_CENTER,
                  NULL);
}

//*****************************************************************************
//
//! @brief Draw the pairing result badge (a green tick / a red exclamation mark).
//!
//! @details A thin ring with the symbol inside. The circle and the symbol are drawn
//! separately: the symbol is a stroked polyline, while the circle needs a full turn,
//! and putting it in the same path would mean approximating it with line segments --
//! visibly polygonal at 24 px.
//!
//! @param bPass true draws the tick (success), false the exclamation mark (failure)
//
//*****************************************************************************
static void
draw_temp_badge(bool bPass)
{
    uint32_t ui32Color = bPass ? TEMP_PAIR_PASS_COLOR : TEMP_PAIR_FAIL_COLOR;

    draw_temp_circle(TEMP_DIAL_CX, TEMP_BADGE_CY, TEMP_BADGE_R, TEMP_BADGE_W,
                     false, ui32Color);

    //
    // The symbol's path has (0,0) as its top-left corner and an edge equal to the badge
    // diameter, so translating it to the circle's top-left lands it on the centre.
    //
    draw_temp_icon_path(bPass ? g_hBadgeOkPath : g_hBadgeErrPath,
                        TEMP_DIAL_CX - TEMP_BADGE_R,
                        TEMP_BADGE_CY - TEMP_BADGE_R,
                        BADGE_STROKE_W, ui32Color);
}

//*****************************************************************************
//
//! @brief Draw the BLE status area in the lower half of the temperature page.
//!
//! @details What each of the five states contains (following the design):
//!
//!   DISCONNECTED  BLE READY (grey-blue) / DISCONNECTED (white)
//!   PAIRING       PAIRING CODE (cyan) / six digits (cyan, large) /
//!                 CONFIRM THE CODE (white)
//!   CONNECTED     BLE READY (cyan) / CONNECTED (white)
//!   PAIR_PASS     green tick / PAIRING SUCCESS (cyan) / PAIRED (white)
//!   PAIR_FAIL     red exclamation mark / PAIRING FAILED (red) / PAIRING ERROR (white)
//!
//! The first row is the small label of "what this is about", the second is "the result
//! now", and the colour roles are fixed: the label carries the state colour and the
//! result is always white. One glance at the colour gives the state without reading
//! the words; which is also why the first row is red on failure while the second stays white.
//!
//! @param eView       The state to display
//! @param ui32Passkey The six comparison digits; only TEMP_BLE_PAIRING uses it
//
//*****************************************************************************
static void
draw_temp_status(temp_ble_view_e eView, uint32_t ui32Passkey)
{
    //
    // "%06u" normally produces exactly 6 characters (the comparison value ranges 0 to
    // 999999); the buffer is 12 so that an out-of-range value from upstream cannot
    // corrupt the stack.
    //
    char pcCode[12];

    switch ( eView )
    {
        case TEMP_BLE_PAIRING:
            //
            // Zero-padded to six digits: the comparison value may be below 100000, the
            // phone always shows six, and one digit short would not match.
            //
            am_util_stdio_sprintf(pcCode, "%06u", (unsigned)ui32Passkey);

            draw_temp_line("PAIRING CODE", TEMP_LABEL_FONT,
                           TEMP_ST3_L1_CY, TEMP_ACCENT_COLOR);
            draw_temp_line(pcCode, TEMP_CODE_FONT,
                           TEMP_ST3_CODE_CY, TEMP_ACCENT_COLOR);
            draw_temp_line("CONFIRM THE CODE", TEMP_LABEL_FONT,
                           TEMP_ST3_L2_CY, TEMP_VALUE_COLOR);
            break;

        case TEMP_BLE_CONNECTED:
            draw_temp_line("BLE READY", TEMP_LABEL_FONT,
                           TEMP_ST2_L1_CY, TEMP_ACCENT_COLOR);
            draw_temp_line("CONNECTED", TEMP_LABEL_FONT,
                           TEMP_ST2_L2_CY, TEMP_VALUE_COLOR);
            break;

        case TEMP_BLE_PAIR_PASS:
            draw_temp_badge(true);
            draw_temp_line("PAIRING SUCCESS", TEMP_LABEL_FONT,
                           TEMP_STB_L1_CY, TEMP_ACCENT_COLOR);
            draw_temp_line("PAIRED", TEMP_LABEL_FONT,
                           TEMP_STB_L2_CY, TEMP_VALUE_COLOR);
            break;

        case TEMP_BLE_PAIR_FAIL:
            draw_temp_badge(false);
            draw_temp_line("PAIRING FAILED", TEMP_LABEL_FONT,
                           TEMP_STB_L1_CY, TEMP_PAIR_FAIL_COLOR);
            draw_temp_line("PAIRING ERROR", TEMP_LABEL_FONT,
                           TEMP_STB_L2_CY, TEMP_VALUE_COLOR);
            break;

        case TEMP_BLE_DISCONNECTED:
        default:
            draw_temp_line("BLE READY", TEMP_LABEL_FONT,
                           TEMP_ST2_L1_CY, TEMP_IDLE_COLOR);
            draw_temp_line("DISCONNECTED", TEMP_LABEL_FONT,
                           TEMP_ST2_L2_CY, TEMP_VALUE_COLOR);
            break;
    }
}

//*****************************************************************************
//
//! @brief Draw the temperature page.
//!
//! @details Shows the TMP119 temperature from SensorTask's cache alongside the BLE
//! state. When the sensor is absent, the read failed or the cache is stale, a message
//! is shown instead of a number that would look like a real value, and the gauge is
//! left empty too.
//!
//! The layout from top to bottom is "Bluetooth icon -> large temperature digits ->
//! status area", with the gauge as a full ring around them. The icon and the status
//! area change with the BLE state, in five variants (see temp_ble_view_e).
//!
//! Everything on this page is drawn with NemaVG (vector) and no textures: the gauge is
//! arcs and line segments, the text is a vector font, the icons are stroked paths. The
//! benefit is that changing a size or a colour needs no new asset.
//!
//! @param fb The target frame buffer
//! @param cl The command list that records the drawing commands
//!
//! @return None
//
//*****************************************************************************
static void
draw_temp(img_obj_t* fb, nema_cmdlist_t* cl)
{
    char            pcValue[16];
    int32_t         i32TempC100;
    uint32_t        ui32Passkey = 0;
    temp_ble_view_e eView;
    float           fFill = 0.0f;

    //
    // Reads only SensorTask's cache and never touches I2C. A stale cache returns
    // non-zero, and the error display branch below is taken in that case.
    //
    uint32_t ui32Status = temp_cache_get(&i32TempC100, TEMP_CACHE_MAX_AGE_TICKS);

    eView = temp_ble_view_get(&ui32Passkey);

    nema_cl_rewind(cl);
    nema_cl_bind_circular(cl);

    nema_bind_dst_tex(fb->bo.base_phys,
                      fb->w,
                      fb->h,
                      fb->format,
                      fb->stride);

    nema_set_clip(0, 0, fb->w, fb->h);

    //
    // The dark background.
    //
    nema_set_blend_fill(NEMA_BL_SRC);
    nema_fill_rect(0, 0, fb->w, fb->h, TEMP_BG_COLOR);

    //
    // The vector font is rebound once per frame. The binding is VG context state, and
    // the same command list also holds arcs, paths and other VG drawing, so it is not
    // assumed to be untouched -- the call only stores a pointer and does one version
    // check, which costs nothing worth counting.
    //
    nema_vg_bind_font(&DejaVuSans_ttf_kern);

    //
    // The gauge. On a sensor fault fFill stays 0 and only the track is drawn.
    //
    if ( ui32Status == 0 )
    {
        float fTempC = (float)i32TempC100 / 100.0f;

        fFill = (fTempC - TEMP_GAUGE_MIN_C) /
                (TEMP_GAUGE_MAX_C - TEMP_GAUGE_MIN_C);
        fFill = nema_clamp(fFill, 0.0f, 1.0f);
    }

    draw_temp_gauge(fFill);

    //
    // The Bluetooth icon. Colour and decoration differ across the five states:
    //
    //   pairing / connected / pairing passed -> cyan (BLE is alive)
    //   not connected / pairing failed       -> grey-blue (BLE is not usable)
    //
    // Pairing failed belongs in the grey-blue group because after a failure that
    // connection cannot carry data, and a lit icon would give the wrong impression;
    // the actual reason is explained by the red badge and text below.
    //
    switch ( eView )
    {
        case TEMP_BLE_PAIRING:
            draw_bt_icon(TEMP_ACCENT_COLOR, true, false);
            break;

        case TEMP_BLE_CONNECTED:
            draw_bt_icon(TEMP_ACCENT_COLOR, false, true);
            break;

        case TEMP_BLE_PAIR_PASS:
            draw_bt_icon(TEMP_ACCENT_COLOR, false, false);
            break;

        case TEMP_BLE_DISCONNECTED:
        case TEMP_BLE_PAIR_FAIL:
        default:
            draw_bt_icon(TEMP_IDLE_COLOR, false, false);
            break;
    }

    //
    // The temperature value.
    //
    if ( ui32Status == 0 )
    {
        //
        // Restored from the x100 integer into two decimal places. The minus sign has to
        // be assembled by hand: take the absolute value first, then split into integer
        // and fraction, or -0.5 degrees would split into whole = 0 and frac = -50, and
        // taking the absolute value of the fraction would give "0.50" with the sign
        // gone entirely.
        //
        int32_t i32Abs = (i32TempC100 < 0) ? -i32TempC100 : i32TempC100;

        am_util_stdio_sprintf(pcValue, "%s%d.%02d",
                              (i32TempC100 < 0) ? "-" : "",
                              (int)(i32Abs / 100),
                              (int)(i32Abs % 100));

        draw_temp_value(pcValue);
    }
    else
    {
        draw_temp_line(TMP119_is_ready() ? "READ ERROR" : "NO SENSOR",
                       TEMP_ERROR_FONT, TEMP_VALUE_CY, TEMP_ERROR_COLOR);
    }

    //
    // The BLE status area.
    //
    draw_temp_status(eView, ui32Passkey);

    //
    // The brightness panel goes on top of the temperature page.
    //
    if ( g_bBlOpen )
    {
        bl_draw();
    }

    nema_cl_submit(cl);
}

//*****************************************************************************
//
//! @brief Push the latest temperature into the trend buffer, once a second.
//!
//! @details Called by RenderTask every frame, deciding for itself whether a second has
//! passed since the last sample. It lives outside the drawing and runs on every page,
//! because the curve is about "the last minute" -- collected only on the trend page,
//! every visit would start from an empty line.
//!
//! When no valid temperature can be read, a TREND_SAMPLE_NONE placeholder is pushed
//! rather than skipping. The time axis therefore stays evenly spaced and the drawing
//! treats invalid points as a break; skipping would quietly compress a blank stretch
//! and make it look as if the temperature had not changed.
//!
//! @return None
//
//*****************************************************************************
static void
trend_sample_update(void)
{
    uint32_t ui32Now = xTaskGetTickCount();
    int32_t  i32TempC100;

    //
    // The first sample is taken immediately, then one every TEMP_SAMPLE_PERIOD_TICKS.
    // The comparison uses subtraction, so it is still correct across tick overflow (49.7 days).
    //
    if ( (g_ui32TrendCount != 0) &&
         ((ui32Now - g_ui32TrendLastTick) < TEMP_SAMPLE_PERIOD_TICKS) )
    {
        return;
    }

    g_ui32TrendLastTick = ui32Now;

    if ( temp_cache_get(&i32TempC100, TEMP_CACHE_MAX_AGE_TICKS) != 0 )
    {
        i32TempC100 = TREND_SAMPLE_NONE;
    }

    g_ai16TrendC100[g_ui32TrendHead] = (int16_t)i32TempC100;

    g_ui32TrendHead = (g_ui32TrendHead + 1U) % TREND_SAMPLE_COUNT;

    if ( g_ui32TrendCount < TREND_SAMPLE_COUNT )
    {
        g_ui32TrendCount++;
    }
}

//*****************************************************************************
//
//! @brief Take the ui32Idx-th oldest sample from the trend buffer.
//!
//! @details Index 0 is the oldest and g_ui32TrendCount - 1 the newest, so the drawing
//! side can loop straight from left to right without working out the ring's start itself.
//!
//! @param ui32Idx The index from oldest to newest
//!
//! @return Celsius x100, or TREND_SAMPLE_NONE
//
//*****************************************************************************
static int16_t
trend_sample_at(uint32_t ui32Idx)
{
    uint32_t ui32Base;

    if ( ui32Idx >= g_ui32TrendCount )
    {
        return TREND_SAMPLE_NONE;
    }

    //
    // While the buffer is not full the oldest is at 0; once it is full, head points at the oldest.
    //
    ui32Base = (g_ui32TrendCount < TREND_SAMPLE_COUNT) ? 0U : g_ui32TrendHead;

    return g_ai16TrendC100[(ui32Base + ui32Idx) % TREND_SAMPLE_COUNT];
}

//*****************************************************************************
//
//! @brief Project (u, v) into screen coordinates.
//!
//! @details u is the time axis (0 = oldest, 1 = newest) and v the temperature axis
//! (0 = the axis minimum, 1 = the maximum). y is reduced by a further
//! u * TREND_PLOT_RISE, which lifts the whole plot from left to right and looks like
//! a receding ground plane -- all of this page's sense of depth comes from it.
//!
//! @param fU    Position on the time axis
//! @param fV    Position on the temperature axis
//! @param pfX   Output screen x
//! @param pfY   Output screen y
//!
//! @return None
//
//*****************************************************************************
static void
trend_project(float fU, float fV, float *pfX, float *pfY)
{
    *pfX = TREND_PLOT_X0 + (fU * TREND_PLOT_W);
    *pfY = TREND_PLOT_Y1 - (fV * TREND_PLOT_H) - (fU * TREND_PLOT_RISE);
}

//*****************************************************************************
//
//! @brief Work out the range the temperature axis should currently show.
//!
//! @details The range follows the data rather than being fixed: body temperature,
//! room temperature and a sensor held in the hand differ widely in absolute value,
//! and a fixed axis would only pin the curve to the top or the bottom for long stretches.
//!
//! Two safeguards: TREND_SPAN_PAD of margin above and below so the curve does not
//! touch the frame; and a span below TREND_SPAN_MIN_C is widened to that floor, or
//! the +-0.02 degree quantization jitter at rest would be amplified into a
//! screenful of sawtooth and look broken.
//!
//! @param pfMin Output axis minimum (Celsius)
//! @param pfMax Output axis maximum (Celsius)
//!
//! @return None
//
//*****************************************************************************
static void
trend_range_get(float *pfMin, float *pfMax)
{
    int32_t  i32Min  = 0;
    int32_t  i32Max  = 0;
    bool     bAny    = false;
    uint32_t ui32Idx;
    float    fMid;
    float    fSpan;

    for ( ui32Idx = 0; ui32Idx < g_ui32TrendCount; ui32Idx++ )
    {
        int16_t i16Val = trend_sample_at(ui32Idx);

        if ( i16Val == TREND_SAMPLE_NONE )
        {
            continue;
        }

        if ( !bAny )
        {
            i32Min = i16Val;
            i32Max = i16Val;
            bAny   = true;
        }
        else if ( i16Val < i32Min )
        {
            i32Min = i16Val;
        }
        else if ( i16Val > i32Max )
        {
            i32Max = i16Val;
        }
    }

    //
    // Not one valid sample: use a plausible-looking room temperature interval. The
    // curve is not drawn in this case anyway, and the axis numbers only need to
    // avoid showing blank or 0.0.
    //
    if ( !bAny )
    {
        *pfMin = 24.0f;
        *pfMax = 24.0f + TREND_SPAN_MIN_C;
        return;
    }

    fMid  = ((float)i32Min + (float)i32Max) / 200.0f;
    fSpan = ((float)i32Max - (float)i32Min) / 100.0f;

    fSpan *= (1.0f + (2.0f * TREND_SPAN_PAD));

    if ( fSpan < TREND_SPAN_MIN_C )
    {
        fSpan = TREND_SPAN_MIN_C;
    }

    *pfMin = fMid - (fSpan / 2.0f);
    *pfMax = fMid + (fSpan / 2.0f);
}

//*****************************************************************************
//
//! @brief A single line of text on the trend page.
//!
//! @details Alignment works through the "text box": nema_vg_print() lays the text
//! out inside (fX, fW), so centring takes the whole screen width, and right
//! alignment puts the box's right edge where the text should end.
//!
//! The font sizes on this page are fixed values chosen for the layout and do not
//! need the automatic shrink of the temperature page -- the only thing that could
//! reach the round frame is the large clock, and that is centred, with even the
//! widest "88:88" just over 200 px.
//!
//! @param pcStr      The string to draw
//! @param fX         Left edge of the text box
//! @param fW         Width of the text box
//! @param fCy        Vertical centre of the text box
//! @param fSize      Font size
//! @param ui32Color  Colour
//! @param ui32AlignX NEMA_VG_ALIGNX_LEFT / _CENTER / _RIGHT
//!
//! @return None
//
//*****************************************************************************
static void
trend_print(const char *pcStr, float fX, float fW, float fCy, float fSize,
            uint32_t ui32Color, uint32_t ui32AlignX)
{
    float fBoxH = TEMP_TEXT_BOX_H(fSize);

    if ( g_hBtIconPaint == NULL )
    {
        return;
    }

    //
    // Glyphs are closed outlines meant to be filled, so the fill rule must be
    // non-zero; keeping the NEMA_VG_STROKE set earlier for the lines would only
    // outline them into hollow text.
    //
    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);

    nema_vg_set_font_size(fSize);
    nema_vg_paint_set_paint_color(g_hBtIconPaint, ui32Color);

    nema_vg_print(g_hBtIconPaint, pcStr,
                  fX, fCy - (fBoxH / 2.0f),
                  fW, fBoxH,
                  ui32AlignX | NEMA_VG_ALIGNY_CENTER,
                  NULL);
}

//*****************************************************************************
//
//! @brief Build a date string from the seconds since boot, e.g. "FRI 18 SEP".
//!
//! @details The board has no RTC, so like the time on the watch face page this is
//! "how long since boot" plus a starting point (TREND_START_*), advancing one day
//! every 86400 seconds.
//!
//! Leap years are not handled: February is always 28 days. This date is not a real
//! date to begin with and the whole section is replaced once an RTC is attached;
//! writing leap year rules for a display-only fake date is not worth it.
//!
//! @param fSec    Seconds since boot (already including WATCH_START_SEC)
//! @param pcOut   Output buffer
//! @param ui32Len Buffer size
//!
//! @return None
//
//*****************************************************************************
static void
trend_date_str(float fSec, char *pcOut, uint32_t ui32Len)
{
    static const char * const pcWeek[7] =
    {
        "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
    };
    static const char * const pcMonth[12] =
    {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    static const uint8_t pui8MonthDays[12] =
    {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    uint32_t ui32Days  = (uint32_t)(fSec / 86400.0f);
    uint32_t ui32Wday  = ((uint32_t)TREND_START_WDAY + ui32Days) % 7U;
    uint32_t ui32Day   = (uint32_t)TREND_START_DAY;
    uint32_t ui32Month = (uint32_t)TREND_START_MONTH - 1U;   // 0-based

    //
    // The longest output is "WED 31 SEP" plus the terminator, 11 bytes. With too
    // small a buffer nothing is drawn, which beats writing past it into someone
    // else's variables.
    //
    if ( (pcOut == NULL) || (ui32Len < 12U) )
    {
        return;
    }

    //
    // Advanced one day at a time. Within a few days of boot the iteration count is
    // the number of days and the cost is negligible; computing it with a single
    // division would mean handling months of unequal length, which is more trouble.
    //
    while ( ui32Days > 0U )
    {
        ui32Day++;

        if ( ui32Day > (uint32_t)pui8MonthDays[ui32Month] )
        {
            ui32Day   = 1U;
            ui32Month = (ui32Month + 1U) % 12U;
        }

        ui32Days--;
    }

    //
    // Always am_util_stdio_sprintf(), never snprintf().
    //
    // AmbiqSuite's am_util_stdio_vsnprintf() copies only ui32NumChars characters and
    // **does not** append a terminating 0 -- which is not what standard snprintf
    // does. When the buffer is reused, the tail of a longer previous string shows
    // past the end of the new one (format a temperature after a date, for instance,
    // and the screen reads "26.288 SEP"). And when the string exceeds n it simply
    // returns 0 without writing a single character, leaving the buffer untouched.
    //
    // The vsprintf() path ends with *pcBuf = 0x0 and is safe; the length is
    // guaranteed jointly by the check above and the caller's buffer size.
    //
    am_util_stdio_sprintf(pcOut, "%s %u %s",
                          pcWeek[ui32Wday],
                          (unsigned)ui32Day,
                          pcMonth[ui32Month]);
}

//*****************************************************************************
//
//! @brief The trend page's bezel: a full ring of fine ticks plus one thick arc on each side.
//!
//! @details Purely decorative and carries no value. It is there so this page looks
//! like part of the same watch as the face and temperature pages -- the plot in the
//! middle is rectangular, and without a bezel the page would look like a pasted screenshot.
//!
//! @return None
//
//*****************************************************************************
static void
trend_draw_rim(void)
{
    nema_matrix3x3_t matrix;
    int32_t          i32Idx;

    if ( g_hBtIconPaint == NULL )
    {
        return;
    }

    nema_mat3x3_load_identity(matrix);

    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_STROKE);
    nema_vg_stroke_set_cap_style(NEMA_VG_CAP_BUTT, NEMA_VG_CAP_BUTT);

    //
    // The fine ticks.
    //
    nema_vg_stroke_set_width(TREND_RIM_W);
    nema_vg_paint_set_paint_color(g_hBtIconPaint, TREND_RIM_COLOR);

    for ( i32Idx = 0; i32Idx < TREND_RIM_COUNT; i32Idx++ )
    {
        float fDeg = (360.0f / (float)TREND_RIM_COUNT) * (float)i32Idx;
        float fCos = nema_cos(fDeg);
        float fSin = nema_sin(fDeg);

        nema_vg_draw_line(TEMP_DIAL_CX + ((TREND_RIM_R - TREND_RIM_LEN) * fCos),
                          TEMP_DIAL_CY + ((TREND_RIM_R - TREND_RIM_LEN) * fSin),
                          TEMP_DIAL_CX + (TREND_RIM_R * fCos),
                          TEMP_DIAL_CY + (TREND_RIM_R * fSin),
                          matrix, g_hBtIconPaint);
    }

    //
    // The two thick arcs. Blue on the left, orange on the right, the same pair as the cool and warm ends of the curve.
    //
    // The right one's angles are written as 332 to 388 degrees (crossing 0) and the
    // whole span has 360 subtracted before drawing, giving -28 to 28:
    // nema_vg_draw_ring() needs its two angles increasing, and passing 332 and 28
    // directly would draw a large arc the other way round.
    //
    nema_vg_stroke_set_width(TREND_ARC_W);

    nema_vg_paint_set_paint_color(g_hBtIconPaint, TREND_COLD_COLOR);
    nema_vg_draw_ring(TEMP_DIAL_CX, TEMP_DIAL_CY, TREND_ARC_R,
                      TREND_ARC_L_DEG0, TREND_ARC_L_DEG1, g_hBtIconPaint);

    nema_vg_paint_set_paint_color(g_hBtIconPaint, TREND_WARM_COLOR);
    nema_vg_draw_ring(TEMP_DIAL_CX, TEMP_DIAL_CY, TREND_ARC_R,
                      TREND_ARC_R_DEG0 - 360.0f, TREND_ARC_R_DEG1 - 360.0f,
                      g_hBtIconPaint);
}

//*****************************************************************************
//
//! @brief The trend page's 3D gridlines.
//!
//! @details Lines of constant u stay vertical after projection while lines of
//! constant v slope up to the right (see trend_project()); stacked together they
//! read as a receding ground plane.
//!
//! The lowest one (v = 0) is drawn once more in a brighter colour: it is the bottom
//! of the temperature axis and the base of the fill under the curve, and in the same
//! colour as the other gridlines the whole area would blur together.
//!
//! @return None
//
//*****************************************************************************
static void
trend_draw_grid(void)
{
    nema_matrix3x3_t matrix;
    int32_t          i32Idx;
    float            fX0;
    float            fY0;
    float            fX1;
    float            fY1;

    if ( g_hBtIconPaint == NULL )
    {
        return;
    }

    nema_mat3x3_load_identity(matrix);

    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_STROKE);
    nema_vg_stroke_set_width(1.0f);
    nema_vg_stroke_set_cap_style(NEMA_VG_CAP_BUTT, NEMA_VG_CAP_BUTT);
    nema_vg_paint_set_paint_color(g_hBtIconPaint, TREND_GRID_COLOR);

    //
    // Vertical lines.
    //
    for ( i32Idx = 0; i32Idx <= TREND_GRID_COLS; i32Idx++ )
    {
        float fU = (float)i32Idx / (float)TREND_GRID_COLS;

        trend_project(fU, 0.0f, &fX0, &fY0);
        trend_project(fU, 1.0f, &fX1, &fY1);

        nema_vg_draw_line(fX0, fY0, fX1, fY1, matrix, g_hBtIconPaint);
    }

    //
    // Horizontal lines. The v = 0 one is left to the baseline below.
    //
    for ( i32Idx = 1; i32Idx <= TREND_GRID_ROWS; i32Idx++ )
    {
        float fV = (float)i32Idx / (float)TREND_GRID_ROWS;

        trend_project(0.0f, fV, &fX0, &fY0);
        trend_project(1.0f, fV, &fX1, &fY1);

        nema_vg_draw_line(fX0, fY0, fX1, fY1, matrix, g_hBtIconPaint);
    }

    //
    // The baseline.
    //
    nema_vg_stroke_set_width(2.0f);
    nema_vg_paint_set_paint_color(g_hBtIconPaint, TREND_GRID_BASE);

    trend_project(0.0f, 0.0f, &fX0, &fY0);
    trend_project(1.0f, 0.0f, &fX1, &fY1);

    nema_vg_draw_line(fX0, fY0, fX1, fY1, matrix, g_hBtIconPaint);
}

//*****************************************************************************
//
//! @brief Format a Celsius temperature into a string with a fixed number of decimals.
//!
//! @details am_util_stdio_sprintf() has no %f, so the degrees are split into an
//! integer and a fractional part and printed separately. It rounds rather than
//! truncates: 36.789 should read 36.79, and truncation would give 36.78, which
//! would not match the value sent over BLE.
//!
//! Negatives are handled separately. A TMP119 on a wrist rarely reads below zero,
//! but i32 division and remainder round towards zero for negatives, and printing
//! directly would produce things like "-3.-5".
//!
//! @param i32C100   Celsius x100
//! @param ui32Digits Decimal places, 1 or 2
//! @param pcOut     Output buffer
//! @param ui32Len   Buffer size
//!
//! @return None
//
//*****************************************************************************
static void
trend_temp_str(int32_t i32C100, uint32_t ui32Digits, char *pcOut, uint32_t ui32Len)
{
    int32_t i32Scaled;
    int32_t i32Div;
    bool    bNeg;

    //
    // The longest output is "-123.45" plus the terminator, 8 bytes. 12 leaves some
    // headroom, for the same reason as trend_date_str() -- the sprintf() below does
    // not check the length.
    //
    if ( (pcOut == NULL) || (ui32Len < 12U) )
    {
        return;
    }

    //
    // Reduce to the decimals to be shown first, then round. At one decimal 3678 -> 368; at two it is unchanged.
    //
    if ( ui32Digits == 1U )
    {
        i32Scaled = (i32C100 >= 0) ? ((i32C100 + 5) / 10) : ((i32C100 - 5) / 10);
        i32Div    = 10;
    }
    else
    {
        i32Scaled = i32C100;
        i32Div    = 100;
    }

    bNeg = (i32Scaled < 0);

    if ( bNeg )
    {
        i32Scaled = -i32Scaled;
    }

    //
    // sprintf() rather than snprintf(), for the reason in trend_date_str() -- the
    // SDK's snprintf does not append the terminating 0.
    //
    if ( ui32Digits == 1U )
    {
        am_util_stdio_sprintf(pcOut, "%s%d.%01u",
                              bNeg ? "-" : "",
                              (int)(i32Scaled / i32Div),
                              (unsigned)(i32Scaled % i32Div));
    }
    else
    {
        am_util_stdio_sprintf(pcOut, "%s%d.%02u",
                              bNeg ? "-" : "",
                              (int)(i32Scaled / i32Div),
                              (unsigned)(i32Scaled % i32Div));
    }
}

//*****************************************************************************
//
//! @brief Get the screen position of sample ui32Idx.
//!
//! @details The time axis conversion lives here and nowhere else: the newest sample
//! is always at u = 1 and older ones extend left, so the axis is always a fixed one
//! minute and a short buffer leaves blank space on the left rather than stretching a
//! few samples across the full width. The fill, the polyline and the end marker all
//! need this conversion, and three copies would eventually miss one.
//!
//! @param ui32Idx The index from oldest to newest
//! @param fMin    Axis minimum
//! @param fSpan   Axis span; the caller guarantees it is above 0
//! @param pfX     Output screen x
//! @param pfY     Output screen y
//! @param pfBaseY Output y of the same u on the baseline; pass NULL when not needed
//!
//! @return true when the sample is valid
//
//*****************************************************************************
static bool
trend_point(uint32_t ui32Idx, float fMin, float fSpan,
            float *pfX, float *pfY, float *pfBaseY)
{
    int16_t i16Val = trend_sample_at(ui32Idx);
    float   fU;
    float   fBaseX;

    if ( i16Val == TREND_SAMPLE_NONE )
    {
        return false;
    }

    fU = 1.0f - ((float)(g_ui32TrendCount - 1U - ui32Idx) /
                 (float)(TREND_SAMPLE_COUNT - 1U));

    trend_project(fU,
                  nema_clamp((((float)i16Val / 100.0f) - fMin) / fSpan,
                             0.0f, 1.0f),
                  pfX, pfY);

    if ( pfBaseY != NULL )
    {
        trend_project(fU, 0.0f, &fBaseX, pfBaseY);
    }

    return true;
}

//*****************************************************************************
//
//! @brief The fill under the curve, and the dark bands that fade it downward.
//!
//! @details Must be called before the gridlines: the bands are semi-transparent
//! horizontal strips laid over the plot area, and gridlines drawn first would be
//! darkened along with it (see TREND_FADE_BANDS).
//!
//! The fill goes through the ordinary fill pipeline rather than VG: all that is
//! needed is a pile of quads, and nema_fill_quad_f() takes one at a time, skipping
//! the whole tessellation that assembling a VG path would require.
//!
//! @param fMin Axis minimum
//! @param fMax Axis maximum
//!
//! @return None
//
//*****************************************************************************
static void
trend_draw_fill(float fMin, float fMax)
{
    uint32_t ui32Idx;
    float    fSpan = fMax - fMin;

    if ( (g_ui32TrendCount < 2U) || (fSpan <= 0.0f) )
    {
        return;
    }

    nema_set_blend_fill(NEMA_BL_SIMPLE);

    for ( ui32Idx = 0U; (ui32Idx + 1U) < g_ui32TrendCount; ui32Idx++ )
    {
        float fX0;
        float fY0;
        float fB0;
        float fX1;
        float fY1;
        float fB1;

        if ( !trend_point(ui32Idx, fMin, fSpan, &fX0, &fY0, &fB0) ||
             !trend_point(ui32Idx + 1U, fMin, fSpan, &fX1, &fY1, &fB1) )
        {
            continue;
        }

        //
        // The vertex order is top-left -> top-right -> bottom-right -> bottom-left, so the quad never self-intersects.
        //
        nema_fill_quad_f(fX0, fY0, fX1, fY1, fX1, fB1, fX0, fB0,
                         TREND_FILL_COLOR);
    }

    //
    // Dark bands decreasing from the bottom up, subduing the lower half of the fill into something like a gradient.
    //
    for ( ui32Idx = 0U; ui32Idx < TREND_FADE_BANDS; ui32Idx++ )
    {
        float    fTop  = TREND_PLOT_Y1 -
                         ((TREND_PLOT_H * (float)(ui32Idx + 1U)) /
                          (float)TREND_FADE_BANDS);
        float    fBot  = TREND_PLOT_Y1 -
                         ((TREND_PLOT_H * (float)ui32Idx) /
                          (float)TREND_FADE_BANDS);
        uint32_t ui32A = (uint32_t)((float)TREND_FADE_ALPHA *
                                    (1.0f - ((float)ui32Idx /
                                             (float)TREND_FADE_BANDS)));

        //
        // The bands have to shear with the projection, or with TREND_PLOT_RISE
        // non-zero a horizontal strip would not line up with the sheared fill.
        // Using quads rather than rectangles settles that at the same time.
        //
        nema_fill_quad_f(TREND_PLOT_X0, fTop,
                         TREND_PLOT_X1, fTop - TREND_PLOT_RISE,
                         TREND_PLOT_X1, fBot - TREND_PLOT_RISE,
                         TREND_PLOT_X0, fBot,
                         CC_ABGR(ui32A, TREND_BG_R, TREND_BG_G, TREND_BG_B));
    }
}

//*****************************************************************************
//
//! @brief Draw the temperature polyline and the bright point at its end.
//!
//! @details Each segment is drawn twice: first a wide faint line in the same colour
//! as a glow, then the body on top. The GPU has no blur, and this is the closest
//! thing to a glow at this size.
//!
//! The colour runs cool to warm, parameterized by "this point's relative position
//! among the data present" rather than its position on the time axis: while the
//! buffer is not yet full the curve occupies only a short stretch on the right, and
//! the latter would make the whole thing orange with no visible head or tail.
//!
//! An invalid sample (a failed sensor read) skips that segment and breaks the curve
//! -- deliberately, since bridging it by interpolation would suggest something was
//! measured during that time.
//!
//! @param fMin   Axis minimum
//! @param fMax   Axis maximum
//! @param pfEndX Output x of the curve's end
//! @param pfEndY Output y of the curve's end
//!
//! @return true when at least one segment was drawn and the end coordinates are valid
//
//*****************************************************************************
static bool
trend_draw_curve(float fMin, float fMax, float *pfEndX, float *pfEndY)
{
    nema_matrix3x3_t matrix;
    uint32_t         ui32Idx;
    float            fSpan  = fMax - fMin;
    float            fEndX  = 0.0f;
    float            fEndY  = 0.0f;
    float            fEndB  = 0.0f;
    bool             bEnd   = false;

    if ( (g_hBtIconPaint == NULL) || (g_ui32TrendCount < 2U) ||
         (fSpan <= 0.0f) )
    {
        return false;
    }

    nema_mat3x3_load_identity(matrix);

    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_STROKE);

    //
    // Round caps on the segment ends: at a bend the polyline is two independent lines, and butt caps would leave a notch on the outside.
    //
    nema_vg_stroke_set_cap_style(NEMA_VG_CAP_ROUND, NEMA_VG_CAP_ROUND);

    for ( ui32Idx = 0U; (ui32Idx + 1U) < g_ui32TrendCount; ui32Idx++ )
    {
        float    fX0;
        float    fY0;
        float    fX1;
        float    fY1;
        float    fB1;
        float    fT;
        uint32_t ui32Color;

        if ( !trend_point(ui32Idx, fMin, fSpan, &fX0, &fY0, NULL) ||
             !trend_point(ui32Idx + 1U, fMin, fSpan, &fX1, &fY1, &fB1) )
        {
            continue;
        }

        fT = ((float)ui32Idx + 0.5f) / (float)(g_ui32TrendCount - 1U);
        fT = nema_clamp((fT - TREND_WARM_START) / (1.0f - TREND_WARM_START),
                        0.0f, 1.0f);

        ui32Color = temp_color_lerp(TREND_COLD_COLOR, TREND_WARM_COLOR, fT);

        //
        // The glow. Only the alpha changes; the RGB is this segment's colour.
        //
        nema_vg_stroke_set_width(TREND_GLOW_W);
        nema_vg_paint_set_paint_color(
            g_hBtIconPaint,
            (ui32Color & 0x00ffffffU) | ((uint32_t)TREND_GLOW_ALPHA << 24));
        nema_vg_draw_line(fX0, fY0, fX1, fY1, matrix, g_hBtIconPaint);

        //
        // The body.
        //
        nema_vg_stroke_set_width(TREND_LINE_W);
        nema_vg_paint_set_paint_color(g_hBtIconPaint, ui32Color);
        nema_vg_draw_line(fX0, fY0, fX1, fY1, matrix, g_hBtIconPaint);

        //
        // Remember the end of the last segment drawn successfully. It is used after the
        // loop because the final sample may be invalid, in which case the end is not at
        // the highest index.
        //
        fEndX = fX1;
        fEndY = fY1;
        fEndB = fB1;
        bEnd  = true;
    }

    if ( !bEnd )
    {
        return false;
    }

    //
    // The vertical line dropping from the end to the baseline. The baseline y comes from
    // the same u (fEndB) rather than always u = 1: with the projection sheared the two
    // differ, and they also differ when the end is not at the far right.
    //
    nema_vg_stroke_set_width(2.5f);
    nema_vg_stroke_set_cap_style(NEMA_VG_CAP_BUTT, NEMA_VG_CAP_BUTT);
    nema_vg_paint_set_paint_color(g_hBtIconPaint, TREND_WARM_COLOR);
    nema_vg_draw_line(fEndX, fEndY, fEndX, fEndB, matrix, g_hBtIconPaint);

    //
    // The bright point at the end: a solid dot with a ring around it, like a cursor. Drawn last, over the lines.
    //
    draw_temp_circle(fEndX, fEndY, TREND_NOW_RING_R, TREND_NOW_RING_W,
                     false, TREND_WARM_COLOR);
    draw_temp_circle(fEndX, fEndY, TREND_NOW_DOT_R, 0.0f,
                     true, TREND_TEXT_COLOR);

    *pfEndX = fEndX;
    *pfEndY = fEndY;

    return true;
}

//*****************************************************************************
//
//! @brief Draw the temperature trend page.
//!
//! @details Top to bottom: the large clock, the date, the 3D temperature curve and
//! the footer. The time shares its source with the watch face page (seconds since
//! boot plus WATCH_START_SEC), so switching between the two never disagrees.
//!
//! The curve's data is collected in the background by trend_sample_update(); this only draws it.
//!
//! @param fb  The target frame buffer
//! @param cl  The command list that records the drawing commands
//! @param fSec Seconds since boot, already including WATCH_START_SEC
//!
//! @return None
//
//*****************************************************************************
static void
draw_trend(img_obj_t* fb, nema_cmdlist_t* cl, float fSec)
{
    char     pcBuf[24];
    char     pcBuf2[24];
    float    fMin;
    float    fMax;
    float    fAxisX;
    float    fAxisY;
    float    fEndX   = 0.0f;
    float    fEndY   = 0.0f;
    bool     bHasEnd;
    int32_t  i32TempC100;
    uint32_t ui32Status;
    uint32_t ui32Total = (uint32_t)fSec;
    uint32_t ui32Idx;

    ui32Status = temp_cache_get(&i32TempC100, TEMP_CACHE_MAX_AGE_TICKS);

    nema_cl_rewind(cl);
    nema_cl_bind_circular(cl);

    nema_bind_dst_tex(fb->bo.base_phys,
                      fb->w,
                      fb->h,
                      fb->format,
                      fb->stride);

    nema_set_clip(0, 0, fb->w, fb->h);

    nema_set_blend_fill(NEMA_BL_SRC);
    nema_fill_rect(0, 0, fb->w, fb->h, TREND_BG_COLOR);

    //
    // The vector font is rebound every frame, for the same reason as the temperature page: the VG context's binding is not assumed untouched.
    //
    nema_vg_bind_font(&DejaVuSans_ttf_kern);

    trend_draw_rim();

    trend_range_get(&fMin, &fMax);

    //
    // The order is "fill -> gridlines -> curve".
    //
    // The fill also draws the dark bands that subdue the lower half (see
    // trend_draw_fill()); those are semi-transparent strips over the whole plot area,
    // so they must come before the gridlines or the gridlines are darkened too.
    //
    trend_draw_fill(fMin, fMax);
    trend_draw_grid();

    bHasEnd = trend_draw_curve(fMin, fMax, &fEndX, &fEndY);

    //
    // The large clock. Hours and minutes with no leading zero -- "3:00" reads more naturally on a watch than "03:00".
    //
    am_util_stdio_sprintf(pcBuf, "%u:%02u",
                          (unsigned)((ui32Total / 3600U) % 24U),
                          (unsigned)((ui32Total / 60U) % 60U));

    trend_print(pcBuf, 0.0f, (float)FB_RESX, TREND_TIME_CY,
                TREND_TIME_FONT, TREND_TEXT_COLOR, NEMA_VG_ALIGNX_CENTER);

    //
    // The date.
    //
    trend_date_str(fSec, pcBuf, sizeof(pcBuf));

    trend_print(pcBuf, 0.0f, (float)FB_RESX, TREND_DATE_CY,
                TREND_DATE_FONT, TREND_DIM_COLOR, NEMA_VG_ALIGNX_CENTER);

    //
    // The current temperature, as a label hanging beside the end of the curve.
    //
    // The degree symbol is a small ring as on the temperature page (the font's glyphs
    // stop at 0x7E and have no degree sign). The group is "digits + degree ring + C",
    // built by subtracting the width the last two need (fTailW) from the right edge;
    // the digits are right-aligned to that point and the ring and the C follow to the right.
    //
    // The digits' actual width need not be measured: right alignment is handled by
    // nema_vg_print()'s text box, and all that is needed here is where that box's right edge is.
    //
    if ( ui32Status == 0 )
    {
        //
        // The label hangs above and to the left of the end point. When that point is
        // high it is clamped by TREND_VALUE_CY_MIN -- better a more slanted leader
        // line than overlapping the date row.
        //
        float fValCy    = bHasEnd ? (fEndY - TREND_VALUE_GAP)
                                  : TREND_VALUE_CY_MIN;
        float fValRight = (bHasEnd ? fEndX : TREND_PLOT_X1) -
                          TREND_VALUE_LEAD;
        float fTailW    = (TREND_DEG_R * 2.0f) + 3.0f +
                          (TREND_VALUE_FONT * 0.55f);
        float fDegCx    = (fValRight - fTailW) + TREND_DEG_R + 2.0f;

        if ( fValCy < TREND_VALUE_CY_MIN )
        {
            fValCy = TREND_VALUE_CY_MIN;
        }

        trend_temp_str(i32TempC100, 2U, pcBuf, sizeof(pcBuf));

        //
        // The leader line from the label's right edge to the bright point. Drawn first, so the text sits over it.
        //
        if ( bHasEnd && (g_hBtIconPaint != NULL) )
        {
            nema_matrix3x3_t matrix;

            nema_mat3x3_load_identity(matrix);

            nema_vg_set_blend(NEMA_BL_SIMPLE);
            nema_vg_set_fill_rule(NEMA_VG_STROKE);
            nema_vg_stroke_set_width(1.0f);
            nema_vg_stroke_set_cap_style(NEMA_VG_CAP_BUTT, NEMA_VG_CAP_BUTT);
            nema_vg_paint_set_paint_color(g_hBtIconPaint, TREND_WARM_COLOR);

            nema_vg_draw_line(fValRight + 4.0f, fValCy,
                              fEndX - TREND_NOW_RING_R - 3.0f, fEndY,
                              matrix, g_hBtIconPaint);
        }

        trend_print(pcBuf, 0.0f, fValRight - fTailW,
                    fValCy, TREND_VALUE_FONT,
                    TREND_TEXT_COLOR, NEMA_VG_ALIGNX_RIGHT);

        draw_temp_circle(fDegCx,
                         fValCy - (TREND_VALUE_FONT * TREND_DEG_RISE),
                         TREND_DEG_R, TREND_DEG_W, false, TREND_WARM_COLOR);

        trend_print("C", fDegCx + TREND_DEG_R + 2.0f,
                    fValRight - (fDegCx + TREND_DEG_R + 2.0f),
                    fValCy, TREND_VALUE_FONT * 0.72f,
                    TREND_WARM_COLOR, NEMA_VG_ALIGNX_LEFT);
    }
    else
    {
        trend_print("--.--", 0.0f, TREND_PLOT_X1, TREND_VALUE_CY_MIN,
                    TREND_VALUE_FONT, TREND_DIM_COLOR, NEMA_VG_ALIGNX_RIGHT);
    }

    //
    // The three ticks of the temperature axis: maximum, midpoint and minimum.
    //
    // The y comes from trend_project() rather than three hand-placed constants -- so
    // the labels follow whenever the axis height or the rise changes. The x always
    // takes the u = 0 side, i.e. the left edge of the plot.
    //
    for ( ui32Idx = 0U; ui32Idx < 3U; ui32Idx++ )
    {
        float fV = (float)ui32Idx / 2.0f;

        trend_project(0.0f, fV, &fAxisX, &fAxisY);

        trend_temp_str((int32_t)(((fMin + ((fMax - fMin) * fV)) * 100.0f) + 0.5f),
                       1U, pcBuf, sizeof(pcBuf));

        trend_print(pcBuf, 0.0f, fAxisX - 7.0f, fAxisY,
                    TREND_AXIS_FONT, TREND_DIM_COLOR, NEMA_VG_ALIGNX_RIGHT);
    }

    //
    // NOW: marked below the right end of the baseline, denoting the right bound of the
    // time axis. The y subtracts the projection's rise, so with TREND_PLOT_RISE
    // non-zero it hugs the right end of the baseline instead of floating in mid-air.
    //
    trend_print("NOW", 0.0f, TREND_PLOT_X1,
                TREND_PLOT_Y1 - TREND_PLOT_RISE + 14.0f,
                TREND_AXIS_FONT, TREND_DIM_COLOR, NEMA_VG_ALIGNX_RIGHT);

    //
    // The footer: the title and the current axis range.
    //
    trend_print("TEMP TREND", 0.0f, (float)FB_RESX, TREND_FOOT_CY,
                TREND_FOOT_FONT, TREND_TEXT_COLOR, NEMA_VG_ALIGNX_CENTER);

    trend_temp_str((int32_t)((fMin * 100.0f) + 0.5f), 1U, pcBuf, sizeof(pcBuf));
    trend_temp_str((int32_t)((fMax * 100.0f) + 0.5f), 1U, pcBuf2, sizeof(pcBuf2));

    {
        char pcRange[40];

        am_util_stdio_sprintf(pcRange, "%s - %s C", pcBuf, pcBuf2);

        trend_print(pcRange, 0.0f, (float)FB_RESX, TREND_RANGE_CY,
                    TREND_RANGE_FONT, TREND_DIM_COLOR, NEMA_VG_ALIGNX_CENTER);
    }

    //
    // The brightness panel goes on top of the trend page.
    //
    if ( g_bBlOpen )
    {
        bl_draw();
    }

    nema_cl_submit(cl);
}

#if WATCHFACE_DESIGN_FONT_PROBE
//*****************************************************************************
//
//! @brief Measure a few strings' on-target widths and print them, to calibrate the Designer's preview estimate.
//!
//! @details The full motivation is under WATCHFACE_DESIGN_FONT_PROBE at the top of the file.
//!
//! The samples include the two strings the generated page actually uses, plus a
//! single character and that character four times -- the latter separates "the width
//! of each character" from "whether extra spacing is added at the ends of a string",
//! which a whole sentence alone cannot show.
//! The same string is measured at two sizes, to confirm that the width really is proportional to the size (the premise of the automatic shrink).
//!
//! Must be called after nema_vg_init() and ui_designer_init(): measuring needs a bound
//! font and a set size. It rebinds them itself rather than relying on what anyone else bound.
//!
//! @return None
//
//*****************************************************************************
static void
design_font_probe(void)
{
    static const char * const pcSamples[] =
    {
        "Ambiq_EdgePilot",  // the generated page's label, box width 251
        "test",             // the generated page's button, box width 180
        "A",                // a single character
        "AAAA"              // the same character four times, to reveal any extra inter-character spacing
    };
    static const float pfSizes[] = { 28.0f, 56.0f };

    uint32_t ui32Sample;
    uint32_t ui32Size;

    nema_vg_bind_font(&DejaVuSans_ttf_kern);

    for ( ui32Size = 0; ui32Size < (sizeof(pfSizes) / sizeof(pfSizes[0])); ui32Size++ )
    {
        for ( ui32Sample = 0;
              ui32Sample < (sizeof(pcSamples) / sizeof(pcSamples[0]));
              ui32Sample++ )
        {
            float fW = 0.0f;
            float fH = 0.0f;
            int32_t i32W;
            int32_t i32H;
            int32_t i32Size;

            nema_vg_set_font_size(pfSizes[ui32Size]);
            (void)nema_vg_string_get_bbox(pcSamples[ui32Sample], &fW, &fH,
                                          (float)FB_RESX, 0);

            //
            // am_util_stdio_printf() has no %f, so everything is converted to
            // hundredths and split into "integer.two decimals" by hand. It rounds
            // rather than truncates, so 252.999 does not print as 252.99 and suggest
            // a whole pixel of difference.
            //
            i32Size = (int32_t)((pfSizes[ui32Size] * 100.0f) + 0.5f);
            i32W    = (int32_t)((fW * 100.0f) + 0.5f);
            i32H    = (int32_t)((fH * 100.0f) + 0.5f);

            am_util_debug_printf(
                "[render] font probe: size=%d.%02d \"%s\" w=%d.%02d h=%d.%02d\n",
                (int)(i32Size / 100), (int)(i32Size % 100),
                pcSamples[ui32Sample],
                (int)(i32W / 100), (int)(i32W % 100),
                (int)(i32H / 100), (int)(i32H % 100));
        }
    }
}
#endif // WATCHFACE_DESIGN_FONT_PROBE

//*****************************************************************************
//
//! @brief Draw page 1: the UI Designer generated page.
//!
//! @details The contents of this page are entirely up to ui_designer_draw(); this
//! function only fits it into the project's existing drawing framework: create and
//! bind the command list, bind the frame buffer, overlay the brightness panel and
//! submit. The generated side deliberately does none of this -- README_INTEGRATION.md
//! states that RenderTask remains the sole owner of the framebuffer and the command
//! list, so re-running Generate C needs no change to how this page is attached.
//!
//! Three things are guaranteed by the generated side and need no guarding here:
//!
//!   - No nema_clear() is needed; the first thing the scene does is fill the whole canvas with the background colour.
//!   - When ui_designer_init() fails, ui_designer_draw() still fills the canvas, so
//!     nothing of the previous page remains.
//!   - The frame interval is computed by ui_designer_tick_to() itself, so the caller need not keep the previous frame's time.
//!
//! @param fb The target frame buffer
//! @param cl The command list that records the drawing commands
//!
//! @return None
//
//*****************************************************************************
static void
draw_design(img_obj_t* fb, nema_cmdlist_t* cl)
{
#if WATCHFACE_DESIGN_FONT_PROBE
    //
    // Measure the string widths once, the first time this page is drawn.
    //
    // Not in RenderTask's init: that happens within a second of boot, and in
    // NO_BLOCK_TRIM mode RTT discards everything before the host connects, so those
    // lines would be long gone by the time a viewer attaches. Here, seeing this page
    // means RTT is attached.
    //
    // It also satisfies the measurement's prerequisites: the GPU is already awake from the main loop and the font is bound.
    //
    static bool bProbeDone = false;
    if ( !bProbeDone )
    {
        bProbeDone = true;
        design_font_probe();
    }
#endif

    nema_cl_rewind(cl);
    nema_cl_bind_circular(cl);

    nema_bind_dst_tex(fb->bo.base_phys,
                      fb->w,
                      fb->h,
                      fb->format,
                      fb->stride);

    nema_set_clip(0, 0, fb->w, fb->h);

    //
    // Advance the animation before drawing, so this frame shows the new angle.
    //
    ui_designer_tick_to(TICKS_TO_MS(xTaskGetTickCount()));

    //
    // This project uses only the generated side's first scene. After adding a scene in
    // the Designer, this has to select with a variable of its own rather than reusing
    // the page enumeration -- pages and scenes are two independent numberings.
    //
    ui_designer_draw(UI_SCENE_DEFAULT);

    //
    // The brightness panel goes on top of the generated page, as on every other page.
    //
    if ( g_bBlOpen )
    {
        bl_draw();
    }

    nema_cl_submit(cl);
}

#if WATCHFACE_ENABLE_TOUCH
//*****************************************************************************
//
//! @brief Test whether a point falls inside the control centre panel.
//!
//! @details Used to separate "wants to use the menu" from "wants to close it". Like
//! cc_hit_test(), it compares against the bounding rectangle and does not subtract the rounded corners.
//!
//! @param ui16X Tap x
//! @param ui16Y Tap y
//!
//! @return true when the point is on the panel.
//
//*****************************************************************************
static bool
cc_point_in_panel(uint16_t ui16X, uint16_t ui16Y)
{
    int32_t i32X = (int32_t)ui16X;
    int32_t i32Y = (int32_t)ui16Y;

    return ( (i32X >= CC_PANEL_X) && (i32X < (CC_PANEL_X + CC_PANEL_W)) &&
             (i32Y >= CC_PANEL_Y) && (i32Y < (CC_PANEL_Y + CC_PANEL_H)) );
}

//*****************************************************************************
//
//! @brief Test whether a point falls inside the brightness panel.
//!
//! @details Used to separate "wants to use the slider" from "wants to close the
//! panel". Like cc_point_in_panel(), it compares against the bounding rectangle and
//! does not subtract the rounded corners.
//!
//! @param ui16X Position x
//! @param ui16Y Position y
//!
//! @return true when the point is on the panel.
//
//*****************************************************************************
static bool
bl_point_in_panel(uint16_t ui16X, uint16_t ui16Y)
{
    int32_t i32X = (int32_t)ui16X;
    int32_t i32Y = (int32_t)ui16Y;

    return ( (i32X >= BL_PANEL_X) && (i32X < (BL_PANEL_X + BL_PANEL_W)) &&
             (i32Y >= BL_PANEL_Y) && (i32Y < (BL_PANEL_Y + BL_PANEL_H)) );
}

//*****************************************************************************
//
//! @brief Decide whether a press counts as "grabbing the slider".
//!
//! @details The hit area is deliberately wider than the slider itself: x takes the
//! whole panel width rather than the slider's 96 px. The slider is only 96 px wide
//! while a fingertip has a large contact area and the measured coordinates drift a
//! dozen-odd px, so testing against the body alone would miss often. There is no other
//! control inside the panel, so widening it cannot hit anything else by mistake.
//!
//! y widens the slider body by BL_SLIDER_GRAB_DY above and below:
//!
//!   slider body  100 .. 368
//!   hit area      76 .. 392
//!   panel         56 .. 412
//!
//! The widening is half the capsule's corner radius rather than the whole radius:
//! the whole radius would make the hit area 52 to 416, consuming the panel's entire
//! height and turning its top and bottom margins into "tap to jump the brightness".
//! A 20 px dead zone leaves room to catch the panel edge without changing the brightness.
//!
//! @param ui16X Press x
//! @param ui16Y Press y
//!
//! @return true when this press should be treated as a slider drag.
//
//*****************************************************************************
#define BL_SLIDER_GRAB_DY   (BL_SLIDER_R / 2)   // 24

static bool
bl_point_in_slider(uint16_t ui16X, uint16_t ui16Y)
{
    int32_t i32X = (int32_t)ui16X;
    int32_t i32Y = (int32_t)ui16Y;

    return ( (i32X >= BL_PANEL_X) && (i32X < (BL_PANEL_X + BL_PANEL_W)) &&
             (i32Y >= (BL_SLIDER_Y - BL_SLIDER_GRAB_DY)) &&
             (i32Y < (BL_SLIDER_BOTTOM + BL_SLIDER_GRAB_DY)) );
}

//*****************************************************************************
//
//! @brief Set the brightness from the y of a press.
//!
//! @details The slider fills from the bottom up: the higher the finger, the brighter.
//! That matches an upright brightness slider on a phone and matches "the filled
//! height is the brightness" visually.
//!
//! A y outside the slider's range is clamped to the ends -- sliding off the slider
//! leaves the brightness at maximum or minimum rather than jumping back to the middle.
//!
//! @param ui16Y Press y
//
//*****************************************************************************
static void
bl_set_level_from_y(uint16_t ui16Y)
{
    int32_t i32Fill = BL_SLIDER_BOTTOM - (int32_t)ui16Y;
    int32_t i32Span = (int32_t)BACKLIGHT_LEVEL_MAX - (int32_t)BACKLIGHT_LEVEL_MIN;
    int32_t i32Level;

    if ( i32Fill < 0 )
    {
        i32Fill = 0;
    }
    else if ( i32Fill > BL_SLIDER_H )
    {
        i32Fill = BL_SLIDER_H;
    }

    i32Level = (int32_t)BACKLIGHT_LEVEL_MIN +
               ((i32Fill * i32Span) / BL_SLIDER_H);

    backlight_set_level((uint8_t)i32Level);
}

//*****************************************************************************
//
//! @brief Decide which control centre cell a tap landed on.
//!
//! @details The hit area is the cell's bounding rectangle with no rounded corners
//! subtracted. The corner radius is 24 px, so what is wrongly included is four
//! triangles of about 150 square px each, while the cells are 12 px apart and a tap
//! there should do nothing anyway -- indistinguishable in use, in exchange for not
//! computing four corner distances on every tap.
//!
//! @param ui16X Tap x
//! @param ui16Y Tap y
//!
//! @return The cc_action_e hit, or -1 when no cell was hit.
//
//*****************************************************************************
static int32_t
cc_hit_test(uint16_t ui16X, uint16_t ui16Y)
{
    static const struct
    {
        uint8_t ui8Col;
        uint8_t ui8Row;
    } sTileGrid[CC_ACT_COUNT] =
    {
        { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 }
    };

    int32_t i32X = (int32_t)ui16X;
    int32_t i32Y = (int32_t)ui16Y;

    for ( uint32_t ui32Idx = 0; ui32Idx < CC_ACT_COUNT; ui32Idx++ )
    {
        int32_t i32TileX = CC_TILE_X(sTileGrid[ui32Idx].ui8Col);
        int32_t i32TileY = CC_TILE_Y(sTileGrid[ui32Idx].ui8Row);

        if ( (i32X >= i32TileX) && (i32X < (i32TileX + CC_TILE_W)) &&
             (i32Y >= i32TileY) && (i32Y < (i32TileY + CC_TILE_H)) )
        {
            return (int32_t)ui32Idx;
        }
    }

    return -1;
}

//*****************************************************************************
//
//! @brief Test whether a point falls inside the home page quick page menu.
//
//*****************************************************************************
static bool
page_menu_point_in_panel(uint16_t ui16X, uint16_t ui16Y)
{
    int32_t i32X = (int32_t)ui16X;
    int32_t i32Y = (int32_t)ui16Y;

    return ( (i32X >= PAGE_MENU_PANEL_X) &&
             (i32X < (PAGE_MENU_PANEL_X + PAGE_MENU_PANEL_W)) &&
             (i32Y >= PAGE_MENU_PANEL_Y) &&
             (i32Y < (PAGE_MENU_PANEL_Y + PAGE_MENU_PANEL_H)) );
}

//*****************************************************************************
//
//! @brief Decide which entry of the quick page menu a tap landed on.
//!
//! @return The page_menu_action_e hit, or -1 when nothing was hit.
//
//*****************************************************************************
static int32_t
page_menu_hit_test(uint16_t ui16X, uint16_t ui16Y)
{
    int32_t i32X = (int32_t)ui16X;
    int32_t i32Y = (int32_t)ui16Y;

    for ( uint32_t ui32Idx = 0; ui32Idx < PAGE_MENU_ACT_COUNT; ui32Idx++ )
    {
        int32_t i32TileY = PAGE_MENU_TILE_Y(ui32Idx);

        if ( (i32X >= PAGE_MENU_TILE_X) &&
             (i32X < (PAGE_MENU_TILE_X + PAGE_MENU_TILE_W)) &&
             (i32Y >= i32TileY) &&
             (i32Y < (i32TileY + PAGE_MENU_TILE_H)) )
        {
            return (int32_t)ui32Idx;
        }
    }

    return -1;
}

//*****************************************************************************
//
// The map navigation page.
//
// The base map is three 468x468 tiles living in PSRAM (see map_task.h for why not
// SSRAM). This page does only three things: blit the visible tiles, draw the vehicle
// in the right place, and overlay the navigation information. Map movement, tile
// loading and eviction, and the navigation simulation all live on the MapTask side;
// this reads one snapshot per frame.
//
//*****************************************************************************

//*****************************************************************************
//
//! @brief Geometry of the two HUD panels.
//!
//! @details The panel width and its y are decided together: the screen is a circle
//! 468 across, and the nearer the edge a row is, the shorter its chord. What decides
//! the values is the corner furthest from the centre -- the top panel's (104, 44) and
//! the bottom panel's (104, 424) are both sqrt(130^2 + 190^2) = 230.2 from the centre,
//! inside the 234 radius but with only 4 px to spare: any wider or any closer to the
//! edge and the corners would be clipped.
//!
//! Why push so close to the edge: the band left between the panels (114..354) is the
//! only area where the vehicle is never covered, and the route's y spans 105 to 340.
//! The wider the band, the less vertical push the camera needs to keep the vehicle
//! inside it, and the less often the screen edge shows beyond the map (see
//! MAP_CAM_Y_MIN/MAX in map_task.c).
//
//*****************************************************************************
#define MAP_HUD_W           (260)
#define MAP_HUD_H           (70)
#define MAP_HUD_X           ((FB_RESX - MAP_HUD_W) / 2)     // 104
#define MAP_HUD_TOP_Y       (44)
#define MAP_HUD_R           (18)

//*****************************************************************************
//
//! @brief The progress bar.
//!
//! @details The lower half used to carry a symmetric panel reading "distance
//! remaining - estimated minutes". Both ends of that line were clipped on a round
//! screen, and the top panel already says the same thing more immediately as "how far
//! to the next turn", so the whole block was removed and only this bar remains.
//!
//! Without the panel it is drawn straight onto the map. That is fine -- the trough is
//! an opaque dark grey visible over any base map, and the completed part is the brightest accent.
//!
//! y = 406 keeps the position it had inside the panel: that row's half-chord is
//! sqrt(234^2 - 172^2) = 158, and the bar's 212 width (half-width 106) still has room.
//
//*****************************************************************************
#define MAP_BAR_W           (MAP_HUD_W - 48)
#define MAP_BAR_H           (6)
#define MAP_BAR_X           ((FB_RESX - MAP_BAR_W) / 2)
#define MAP_BAR_Y           (406)

//*****************************************************************************
//
//! @brief The map page's palette.
//!
//! @details The panels use the same deep blue-grey as the control centre, only more
//! transparent (0xc8 against 0xdc) -- what is underneath is a map rather than a static
//! image, and letting the route and the blocks show through actually helps read the position.
//!
//! The accent is taken from the colour of the route on the base map (#01C1FE); the
//! vehicle, the progress bar and the "how far to go" figure all use it, so only one
//! blue on screen says one thing.
//
//*****************************************************************************
#define MAP_HUD_COLOR       CC_ABGR(0xc8, 0x10, 0x18, 0x20)
#define MAP_ACCENT_COLOR    CC_ABGR(0xff, 0x01, 0xc1, 0xfe)
#define MAP_TEXT_COLOR      CC_ABGR(0xff, 0xf0, 0xf4, 0xf8)
#define MAP_DIM_COLOR       CC_ABGR(0xff, 0x8a, 0x96, 0xa2)
#define MAP_BAR_BG_COLOR    CC_ABGR(0xff, 0x2a, 0x33, 0x3d)

//
// The vehicle deliberately does not use the accent colour.
//
// The usual navigation app draws a blue arrow with a white outline, because the route
// underneath is much narrower than the arrow. Here the route is 11 px wide and happens
// to be the very same blue, so a blue vehicle would melt into it -- on a straight
// stretch only the white outline would give it away. Hence the reverse: a white arrow
// with a dark ring, which has the strongest contrast on the blue route and stays
// legible off the route too (dragged elsewhere while browsing freely).
//
#define MAP_CAR_BODY_COLOR  CC_ABGR(0xff, 0xff, 0xff, 0xff)
#define MAP_CAR_EDGE_COLOR  CC_ABGR(0xff, 0x0d, 0x14, 0x1c)

//*****************************************************************************
//
//! @brief Size of the vehicle.
//!
//! @details The arrow's circumscribed radius is 13 px, with the white ring 3 px larger.
//! Making it smaller than a usual icon is deliberate -- it sits on the map, and any
//! larger it would hide the very junction it is standing on.
//
//*****************************************************************************
#define MAP_CAR_R           (13.0f)
#define MAP_CAR_RING_R      (16.0f)
#define MAP_CAR_RING_W      (2.5f)

//*****************************************************************************
//
//! @brief Convert world coordinates to screen coordinates.
//!
//! @details The whole map page has only this one conversion:
//!
//!     screen = (world - cam) * zoom
//!
//! Route coordinates, tile indices and the cache all live in world coordinates, and
//! the conversion happens only when something is finally blitted or drawn. The
//! reverse (screen -> world) is needed only for the zoom anchor and lives in MapTaskZoom().
//!
//! Rounding uses +0.5f rather than truncation: truncation would make the seam
//! between adjacent tiles off by 1 px at some zoom factors, showing a line of background.
//!
//! @param world  World coordinate.
//! @param cam    The camera's world coordinate on that axis.
//! @param zoom   Zoom factor.
//
//*****************************************************************************
#define MAP_W2S(world, cam, zoom)   \
    ((int32_t)((((float)(world) - (float)(cam)) * (zoom)) + 0.5f))

//*****************************************************************************
//
//! @brief viewBox conversion for the turn icons.
//!
//! @details The same approach as the control centre icons: the vertices are written
//! in a 24x24 viewBox and converted to pixels at compile time. The target edge here is 40 px.
//
//*****************************************************************************
#define MAP_TURN_ICON_SIZE  (40.0f)
#define MAP_TURN_S(v)       ((v) * (MAP_TURN_ICON_SIZE / 24.0f))
#define MAP_TURN_STROKE_W   MAP_TURN_S(2.4f)

//
// Left turn: up from below, left at the junction, plus an arrowhead. All three are
// stroked, with round caps to soften the corner. The right turn is its mirror about x = 12.
//
static const uint8_t g_pui8TurnLeftSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // (12,21) from below
    NEMA_VG_PRIM_LINE,      // (12,10) the junction
    NEMA_VG_PRIM_LINE,      // ( 4,10) turning left
    NEMA_VG_PRIM_MOVE,      // ( 8, 6) upper wing of the arrowhead
    NEMA_VG_PRIM_LINE,      // ( 4,10) tip of the arrowhead
    NEMA_VG_PRIM_LINE       // ( 8,14) lower wing of the arrowhead
};

static const nema_vg_float_t g_pfTurnLeftData[] =
{
    MAP_TURN_S(12.0f), MAP_TURN_S(21.0f),
    MAP_TURN_S(12.0f), MAP_TURN_S(10.0f),
    MAP_TURN_S( 4.0f), MAP_TURN_S(10.0f),
    MAP_TURN_S( 8.0f), MAP_TURN_S( 6.0f),
    MAP_TURN_S( 4.0f), MAP_TURN_S(10.0f),
    MAP_TURN_S( 8.0f), MAP_TURN_S(14.0f)
};

static const uint8_t g_pui8TurnRightSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // (12,21)
    NEMA_VG_PRIM_LINE,      // (12,10)
    NEMA_VG_PRIM_LINE,      // (20,10)
    NEMA_VG_PRIM_MOVE,      // (16, 6)
    NEMA_VG_PRIM_LINE,      // (20,10)
    NEMA_VG_PRIM_LINE       // (16,14)
};

static const nema_vg_float_t g_pfTurnRightData[] =
{
    MAP_TURN_S(12.0f), MAP_TURN_S(21.0f),
    MAP_TURN_S(12.0f), MAP_TURN_S(10.0f),
    MAP_TURN_S(20.0f), MAP_TURN_S(10.0f),
    MAP_TURN_S(16.0f), MAP_TURN_S( 6.0f),
    MAP_TURN_S(20.0f), MAP_TURN_S(10.0f),
    MAP_TURN_S(16.0f), MAP_TURN_S(14.0f)
};

//
// Straight on: a single upward arrow. Arrival uses it too -- the destination is
// directly ahead, and a different icon would only need explaining.
//
static const uint8_t g_pui8TurnUpSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // (12,21)
    NEMA_VG_PRIM_LINE,      // (12, 5)
    NEMA_VG_PRIM_MOVE,      // ( 7,10)
    NEMA_VG_PRIM_LINE,      // (12, 5)
    NEMA_VG_PRIM_LINE       // (17,10)
};

static const nema_vg_float_t g_pfTurnUpData[] =
{
    MAP_TURN_S(12.0f), MAP_TURN_S(21.0f),
    MAP_TURN_S(12.0f), MAP_TURN_S( 5.0f),
    MAP_TURN_S( 7.0f), MAP_TURN_S(10.0f),
    MAP_TURN_S(12.0f), MAP_TURN_S( 5.0f),
    MAP_TURN_S(17.0f), MAP_TURN_S(10.0f)
};

//
// The vehicle: an arrow centred on the origin, pointing at +x (east) by default. The
// tail is notched inward so it reads as having a direction -- a plain isosceles
// triangle has no distinguishable front or back at this size.
// This is a filled shape rather than a stroke, so the fill rule has to go back to NON_ZERO before drawing.
//
static const uint8_t g_pui8CarSegs[] =
{
    NEMA_VG_PRIM_MOVE,      // nose
    NEMA_VG_PRIM_LINE,      // left rear
    NEMA_VG_PRIM_LINE,      // tail notch
    NEMA_VG_PRIM_LINE,      // right rear
    NEMA_VG_PRIM_CLOSE
};

static const nema_vg_float_t g_pfCarData[] =
{
     MAP_CAR_R,         0.0f,
    -MAP_CAR_R * 0.7f, -MAP_CAR_R * 0.7f,
    -MAP_CAR_R * 0.35f, 0.0f,
    -MAP_CAR_R * 0.7f,  MAP_CAR_R * 0.7f
};

static NEMA_VG_PATH_HANDLE g_hTurnLeftPath;
static NEMA_VG_PATH_HANDLE g_hTurnRightPath;
static NEMA_VG_PATH_HANDLE g_hTurnUpPath;
static NEMA_VG_PATH_HANDLE g_hCarPath;

//*****************************************************************************
//
//! @brief Remember the finger's position on the previous frame, to compute drag displacement.
//!
//! @details The gesture layer gives the coordinates "at press" and "at release",
//! plus this frame's live coordinates (see bPressed/ui16CurX in touch_task.h). A
//! drag needs the difference between frames, so the previous frame's value is kept here.
//!
//! g_bMapDragging is also the flag for "this press is already dragging the map",
//! used at release to decide whether to swallow the page-change gesture -- see ui_input_update().
//
//*****************************************************************************
static bool     g_bMapDragging;
static uint16_t g_ui16MapLastX;

//*****************************************************************************
//
//! @brief State of pinch zoom.
//!
//! @details The same pattern as dragging: the gesture layer only gives live
//! coordinates and the change between frames is computed here. Zoom follows the
//! ratio of the finger distance, so the previous frame's distance has to be kept.
//!
//! g_bMapPinching is also the flag for "this press is already zooming" -- single-finger
//! dragging has to be switched off entirely during a pinch, or the first finger's
//! movement would also count as a pan and the map would wander while it zooms.
//!
//! The ratio cannot be computed the instant the second finger lands: that frame has
//! no "previous distance" to compare with. So the first one only records the distance
//! and zooming starts from the second frame.
//
//*****************************************************************************
static bool  g_bMapPinching;
static float g_fMapLastPinchDist;

//*****************************************************************************
//
//! @brief A finger distance below this is not treated as a pinch.
//!
//! @details With the fingers close together the relative error in the distance is
//! large -- two or three pixels of coordinate jitter from the touch IC swing the
//! ratio by tens of percent and the screen appears to twitch. 40 px is roughly the
//! width of two fingertips side by side, and "two fingers" closer than that is not a
//! meaningful pinch to begin with.
//
//*****************************************************************************
#define MAP_PINCH_MIN_DIST  (40.0f)

//
// The page change function is defined after this section (it needs the map page's
// state), while the map page's gesture handling needs to leave this page, hence the forward declaration here.
//
static void ui_page_set(ui_page_e ePage);

//
// The watch face selector is defined in full after the page change function (it needs
// g_sFaces[] and parts of the drawing layer), while a page change has to close the
// selector, hence the forward declaration here as well.
//
static void fsel_close(bool bApply);

//
// The page overview (psel_*) is defined at the very end (it needs each page's
// draw_*()), while Button1's handling has to ask whether it is open, hence the forward declaration here too.
//
// Unlike fsel_close(), these two have to disappear with WATCHFACE_ENABLE_TOUCH: the
// overview is a purely touch feature and the whole section is left out when touch is
// off, where keeping the declarations would give "declared static but never defined" warnings.
//
#if WATCHFACE_ENABLE_TOUCH
static bool psel_active(void);
static void psel_close(void);
#endif

//
// How far this press has dragged in total. A displacement too short counts as a tap
// rather than a drag, and that tap is reserved for "restart after arrival".
//
static int32_t  g_i32MapDragTotal;

#define MAP_DRAG_MIN_PX     (8)

//*****************************************************************************
//
//! @brief Create the vector paths of the map page.
//!
//! @details Called together with cc_icons_init(). The vertex data is const and set
//! once; after that each frame only changes the matrix and the colour.
//!
//! @return None
//
//*****************************************************************************
static void
map_icons_init(void)
{
    g_hTurnLeftPath  = nema_vg_path_create();
    g_hTurnRightPath = nema_vg_path_create();
    g_hTurnUpPath    = nema_vg_path_create();
    g_hCarPath       = nema_vg_path_create();

    if ( (g_hTurnLeftPath == NULL) || (g_hTurnRightPath == NULL) ||
         (g_hTurnUpPath == NULL) || (g_hCarPath == NULL) )
    {
        am_util_debug_printf("[render] map path create failed\n");
        return;
    }

    nema_vg_path_set_shape(g_hTurnLeftPath,
                           sizeof(g_pui8TurnLeftSegs) / sizeof(uint8_t),
                           g_pui8TurnLeftSegs,
                           sizeof(g_pfTurnLeftData) / sizeof(nema_vg_float_t),
                           g_pfTurnLeftData);
    nema_vg_path_set_shape(g_hTurnRightPath,
                           sizeof(g_pui8TurnRightSegs) / sizeof(uint8_t),
                           g_pui8TurnRightSegs,
                           sizeof(g_pfTurnRightData) / sizeof(nema_vg_float_t),
                           g_pfTurnRightData);
    nema_vg_path_set_shape(g_hTurnUpPath,
                           sizeof(g_pui8TurnUpSegs) / sizeof(uint8_t),
                           g_pui8TurnUpSegs,
                           sizeof(g_pfTurnUpData) / sizeof(nema_vg_float_t),
                           g_pfTurnUpData);
    nema_vg_path_set_shape(g_hCarPath,
                           sizeof(g_pui8CarSegs) / sizeof(uint8_t),
                           g_pui8CarSegs,
                           sizeof(g_pfCarData) / sizeof(nema_vg_float_t),
                           g_pfCarData);
}

//*****************************************************************************
//
//! @brief Draw a line of text inside a given rectangle.
//!
//! @details All the text on the map page sits inside its own HUD panel and, unlike
//! the temperature page, needs no scaling to the circular boundary, so this is a
//! plain "centre inside this box" with none of draw_temp_line()'s chord arithmetic.
//!
//! The font's glyphs cover only 0x20 to 0x7E, and Chinese would need a separate
//! bitmap asset. Place names are therefore not on the HUD -- the base map already
//! carries them, and the HUD only handles figures such as distance and time.
//!
//! @param pcStr     The string (ASCII)
//! @param fSize     Font size
//! @param fX        Top-left x of the box
//! @param fY        Top-left y of the box
//! @param fW        Box width
//! @param fH        Box height
//! @param ui32Align Alignment (NEMA_VG_ALIGNX_* | NEMA_VG_ALIGNY_*)
//! @param ui32Color Colour (ABGR)
//
//*****************************************************************************
static void
map_text(const char *pcStr, float fSize, float fX, float fY,
         float fW, float fH, uint32_t ui32Align, uint32_t ui32Color)
{
    if ( g_hBtIconPaint == NULL )
    {
        return;
    }

    //
    // Glyphs are closed outlines meant to be filled, so the fill rule must be
    // NON_ZERO; keeping the STROKE set earlier for the turn icons would only outline them.
    //
    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);
    nema_vg_set_font_size(fSize);

    nema_vg_paint_set_paint_color(g_hBtIconPaint, ui32Color);
    nema_vg_print(g_hBtIconPaint, pcStr, fX, fY, fW, fH, ui32Align, NULL);
}

//*****************************************************************************
//
//! @brief Blit the map tiles the window covers.
//!
//! @details The window is as wide as a tile, so at most two are spanned. The
//! intersection with the window is computed per tile and only that part is blitted
//! with nema_blit_subrect() -- blitting the whole tile and clipping would give the
//! same picture, but the GPU would read a whole tile's worth of pixels, and those
//! pixels come from PSRAM, the most expensive bandwidth on this page.
//!
//! When a tile is not in the cache yet MapTaskGetTileAddr() returns 0 and that block
//! is skipped. The background has already been cleared to dark grey, so it looks
//! like a part of the map not loaded yet rather than random content.
//!
//! There is only one tile vertically (the world is as tall as a tile), but the camera
//! still pushes up and down a little to keep the vehicle clear of the HUD (see
//! MAP_CAM_Y_MIN/MAX in map_task.c), so y is clipped the same way. The strip pushed
//! past the edge keeps the nema_clear() background.
//!
//! @param fCamX The camera's world x
//! @param fCamY The camera's world y, which may be negative
//
//*****************************************************************************
static void
map_draw_tiles(float fCamX, float fCamY, float fZoom)
{
    //
    // The window's size in world coordinates. Larger than the screen when zoomed out (a wider view), smaller when zoomed in.
    //
    int32_t i32ViewW = (int32_t)((float)FB_RESX / fZoom);
    int32_t i32ViewH = (int32_t)((float)FB_RESY / fZoom);

    int32_t i32CamX = (int32_t)fCamX;
    int32_t i32CamY = (int32_t)fCamY;
    int32_t i32First;
    int32_t i32Last;
    int32_t i32Idx;
    int32_t i32SrcY0;
    int32_t i32SrcY1;
    int32_t i32DstY0;
    int32_t i32DstY1;

    //
    // Vertical clipping, shared by all three tiles -- their y ranges are identical.
    //
    // The intersection of tile and window is computed in **world coordinates** first
    // and then converted to screen coordinates. Before zoom was added the two were the
    // same thing (1:1); now they must be kept apart: the source (src) is always world
    // or tile pixels, the destination (dst) is always screen pixels.
    //
    i32SrcY0 = (i32CamY > 0) ? i32CamY : 0;
    i32SrcY1 = i32CamY + i32ViewH;
    if ( i32SrcY1 > MAP_TILE_H )
    {
        i32SrcY1 = MAP_TILE_H;
    }

    if ( i32SrcY1 <= i32SrcY0 )
    {
        return;
    }

    //
    // Both endpoints are converted separately and the destination height is their difference, rather than the source height times the factor.
    //
    // The difference is at the boundary: if two adjacent blocks each did "convert the
    // start + convert the width", floating-point error would leave their edges
    // misaligned and a line of background showing at the seam. Converting the same
    // world coordinate to the same screen coordinate makes one block's right edge
    // necessarily equal to the next one's left. There is only one block vertically
    // here, but the horizontal direction uses the same method, and writing both the
    // same way makes it less likely that only one gets changed later.
    //
    i32DstY0 = MAP_W2S(i32SrcY0, i32CamY, fZoom);
    i32DstY1 = MAP_W2S(i32SrcY1, i32CamY, fZoom);

    if ( i32DstY1 <= i32DstY0 )
    {
        return;
    }

    //
    // Which tile each of the window's left and right edges falls on. The camera has
    // already been clamped by MapTask, and clamping again here avoids depending on
    // that guarantee -- an out-of-range index reads someone else's memory, which is
    // out of all proportion to two extra comparisons.
    //
    i32First = i32CamX / MAP_TILE_W;
    i32Last  = (i32CamX + i32ViewW - 1) / MAP_TILE_W;

    if ( i32First < 0 )
    {
        i32First = 0;
    }
    if ( i32Last > (MAP_TILE_COUNT - 1) )
    {
        i32Last = MAP_TILE_COUNT - 1;
    }

    for ( i32Idx = i32First; i32Idx <= i32Last; i32Idx++ )
    {
        //
        // This tile's intersection with the window, in world coordinates.
        //
        int32_t i32TileWorldX = i32Idx * MAP_TILE_W;
        int32_t i32WorldX0    = i32TileWorldX;
        int32_t i32WorldX1    = i32TileWorldX + MAP_TILE_W;
        int32_t i32DstX0;
        int32_t i32DstX1;
        uint32_t ui32Addr;

        if ( i32WorldX0 < i32CamX )
        {
            i32WorldX0 = i32CamX;
        }
        if ( i32WorldX1 > (i32CamX + i32ViewW) )
        {
            i32WorldX1 = i32CamX + i32ViewW;
        }

        if ( i32WorldX1 <= i32WorldX0 )
        {
            continue;
        }

        i32DstX0 = MAP_W2S(i32WorldX0, i32CamX, fZoom);
        i32DstX1 = MAP_W2S(i32WorldX1, i32CamX, fZoom);

        if ( i32DstX1 <= i32DstX0 )
        {
            continue;
        }

        ui32Addr = MapTaskGetTileAddr((uint32_t)i32Idx);
        if ( ui32Addr == 0 )
        {
            continue;
        }

        //
        // The PSRAM address is handed straight to the GPU. The tile need not be copied
        // into SSRAM first -- the GPU can sample the MSPI XIP address space (unlike
        // MRAM, see psram.h).
        //
        // The sampling mode follows the zoom: at 1:1 point sampling is sharpest, but
        // once scaled it must be bilinear, or zooming out drops thin lines (the route,
        // block edges) one here and one there, and zooming in is pure mosaic.
        //
        nema_bind_src_tex(ui32Addr,
                          MAP_TILE_W, MAP_TILE_H,
                          NEMA_RGB565, -1,
                          (fZoom == 1.0f) ? NEMA_FILTER_PS : NEMA_FILTER_BL);

        nema_set_blend_blit(NEMA_BL_SRC);

        //
        // subrect_fit takes a destination width and height on top of subrect, and the
        // source block is scaled to that size. At 1:1 the two give the same result.
        //
        nema_blit_subrect_fit(i32DstX0, i32DstY0,
                              i32DstX1 - i32DstX0, i32DstY1 - i32DstY0,
                              i32WorldX0 - i32TileWorldX, i32SrcY0,
                              i32WorldX1 - i32WorldX0, i32SrcY1 - i32SrcY0);
    }
}

//*****************************************************************************
//
//! @brief Draw the current position.
//!
//! @details An arrow pointing along the heading, inside a white ring that separates it
//! from the blue route on the map -- the route and the vehicle are the same blue, and
//! without the ring the vehicle would melt into it.
//!
//! Nothing is drawn when the vehicle is off screen. That happens when browsing freely
//! and dragging far enough, and no vehicle on screen is then correct: it really is not
//! in the part you are looking at.
//!
//! @param psView This frame's map state
//
//*****************************************************************************
static void
map_draw_car(const map_view_t *psView)
{
    nema_matrix3x3_t matrix;

    //
    // The vehicle's position follows the zoom but its size does not -- as in navigation
    // apps: zooming in enlarges the junction, and the arrow that means "you" does not
    // grow into a blob.
    //
    float fX = (psView->fCarX - psView->fCamX) * psView->fZoom;
    float fY = (psView->fCarY - psView->fCamY) * psView->fZoom;

    if ( (g_hCarPath == NULL) || (g_hCcIconPaint == NULL) )
    {
        return;
    }

    if ( (fX < -MAP_CAR_RING_R) || (fX > ((float)FB_RESX + MAP_CAR_RING_R)) )
    {
        return;
    }

    //
    // The ring. Drawn before the arrow, so the arrow sits on top of it.
    //
    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_STROKE);
    nema_vg_stroke_set_width(MAP_CAR_RING_W);
    nema_vg_paint_set_paint_color(g_hCcIconPaint, MAP_CAR_EDGE_COLOR);
    nema_vg_draw_circle(fX, fY, MAP_CAR_RING_R, NULL, g_hCcIconPaint);

    //
    // The arrow itself: rotate to the heading first, then translate to the vehicle's
    // position. The order cannot be reversed -- translating first would make the
    // rotation turn about the screen origin and fling the vehicle off screen.
    //
    nema_mat3x3_load_identity(matrix);
    nema_mat3x3_rotate(matrix, psView->fHeading);
    nema_mat3x3_translate(matrix, fX, fY);
    nema_vg_path_set_matrix(g_hCarPath, matrix);

    nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);
    nema_vg_paint_set_paint_color(g_hCcIconPaint, MAP_CAR_BODY_COLOR);
    nema_vg_draw_path(g_hCarPath, g_hCcIconPaint);
}

//*****************************************************************************
//
//! @brief Format a distance as a human-readable string.
//!
//! @details Below 1 km it uses metres rounded to 10 m, above that kilometres with one
//! decimal -- as navigation apps do. The rounding is because the simulated position
//! changes every frame, and single metres ticking over is only distracting.
//!
//! @param pcBuf     Output buffer
//! @param ui32Size  Buffer size
//! @param ui32M     Distance in metres
//
//*****************************************************************************
//
//! @brief Why every string on this page zeroes its buffer before formatting.
//!
//! @details am_util_stdio_snprintf() is not the standard snprintf. Its implementation
//! (am_util_stdio_vsnprintf in utils/am_util_stdio.c) ends like this:
//!
//!     for (i = 0; i < ui32NumChars; i++) { pcBuf[i] = g_prfbuf[i]; }
//!     return ui32NumChars;
//!
//! It returns once the characters are copied and **writes no NUL terminator**. Unless
//! the caller has arranged for a terminator, the string trails whatever garbage is on the stack.
//!
//! Hit twice in practice: the HUD distance showing as "4.9 kmC -" (that C is garbage),
//! and, worse, a map tile's file name becoming "MAPTILE0.BIN" plus a run of noise, so
//! f_open() returned FR_NO_FILE and it took retries until the stack happened to hold a
//! 0 -- which is why the map took several seconds to appear.
//!
//! Everywhere on this page that uses am_util_stdio_snprintf() memsets first.
//
//*****************************************************************************
static void
map_fmt_dist(char *pcBuf, uint32_t ui32Size, uint32_t ui32M)
{
    //
    // Zero it first, for the reason above -- am_util_stdio_snprintf() writes no NUL
    // terminator, and without this the string trails garbage from the stack.
    //
    memset(pcBuf, 0, ui32Size);

    if ( ui32M >= 1000 )
    {
        am_util_stdio_snprintf(pcBuf, ui32Size, "%u.%u km",
                               ui32M / 1000, (ui32M % 1000) / 100);
    }
    else
    {
        am_util_stdio_snprintf(pcBuf, ui32Size, "%u m", (ui32M / 10) * 10);
    }
}

//*****************************************************************************
//
//! @brief Draw the two HUD panels.
//!
//! @details The top one is "what to do next" (turn icon + how far), the bottom one is
//! "how much of the trip is left" (distance, estimated time, progress bar). They are
//! two blocks rather than one because they differ in update rate and importance: the
//! turn information is to be acted on, the remaining distance is only for reference.
//!
//! @param psView This frame's map state
//
//*****************************************************************************
static void
map_draw_hud(const map_view_t *psView)
{
    char cBuf[24];
    NEMA_VG_PATH_HANDLE hTurn;
    nema_matrix3x3_t matrix;
    int32_t i32BarW;

    //
    // The panel background. Only the top one -- the lower block has been removed (see MAP_BAR_Y).
    //
    nema_set_blend_fill(NEMA_BL_SIMPLE);
    nema_fill_rounded_rect(MAP_HUD_X, MAP_HUD_TOP_Y, MAP_HUD_W, MAP_HUD_H,
                           MAP_HUD_R, MAP_HUD_COLOR);

    //
    // The progress bar. The trough is filled first, then the completed fraction is covered in the accent colour.
    //
    nema_fill_rounded_rect(MAP_BAR_X, MAP_BAR_Y, MAP_BAR_W, MAP_BAR_H,
                           MAP_BAR_H / 2, MAP_BAR_BG_COLOR);

    i32BarW = (int32_t)((MAP_BAR_W * psView->ui32Progress) / 100);
    if ( i32BarW > 0 )
    {
        nema_fill_rounded_rect(MAP_BAR_X, MAP_BAR_Y, i32BarW, MAP_BAR_H,
                               MAP_BAR_H / 2, MAP_ACCENT_COLOR);
    }

    //
    // The turn icon in the top panel.
    //
    switch ( psView->eNextTurn )
    {
        case MAP_TURN_LEFT:
            hTurn = g_hTurnLeftPath;
            break;

        case MAP_TURN_RIGHT:
            hTurn = g_hTurnRightPath;
            break;

        default:
            hTurn = g_hTurnUpPath;
            break;
    }

    if ( (hTurn != NULL) && (g_hCcIconPaint != NULL) )
    {
        nema_vg_set_blend(NEMA_BL_SIMPLE);
        nema_vg_set_fill_rule(NEMA_VG_STROKE);
        nema_vg_stroke_set_width(MAP_TURN_STROKE_W);
        nema_vg_stroke_set_cap_style(NEMA_VG_CAP_ROUND, NEMA_VG_CAP_ROUND);
        nema_vg_stroke_set_join_style(NEMA_VG_JOIN_ROUND);

        nema_mat3x3_load_identity(matrix);
        nema_mat3x3_translate(matrix,
                              (float)(MAP_HUD_X + 26),
                              (float)(MAP_HUD_TOP_Y + 15));
        nema_vg_path_set_matrix(hTurn, matrix);

        //
        // The icon dims while stopped: that direction is still the next action, but we
        // are not currently heading there. One colour change makes "stopped" versus
        // "running" obvious at a glance.
        //
        nema_vg_paint_set_paint_color(g_hCcIconPaint,
                                      psView->bPaused ? MAP_DIM_COLOR
                                                      : MAP_TEXT_COLOR);
        nema_vg_draw_path(hTurn, g_hCcIconPaint);
    }

    //
    // The text in the top panel. The three states each say their own thing: after
    // arrival no distance is reported, while stopped the user should know they stopped
    // it themselves (rather than the program hanging), and otherwise it is "how far to
    // the next turn".
    //
    if ( psView->bArrived )
    {
        map_text("ARRIVED", 26.0f,
                 (float)(MAP_HUD_X + 76), (float)MAP_HUD_TOP_Y,
                 (float)(MAP_HUD_W - 86), (float)MAP_HUD_H,
                 NEMA_VG_ALIGNX_LEFT | NEMA_VG_ALIGNY_CENTER,
                 MAP_ACCENT_COLOR);
    }
    else if ( psView->bPaused )
    {
        map_text("PAUSED", 26.0f,
                 (float)(MAP_HUD_X + 76), (float)MAP_HUD_TOP_Y,
                 (float)(MAP_HUD_W - 86), (float)MAP_HUD_H,
                 NEMA_VG_ALIGNX_LEFT | NEMA_VG_ALIGNY_CENTER,
                 MAP_ACCENT_COLOR);
    }
    else
    {
        map_fmt_dist(cBuf, sizeof(cBuf), psView->ui32NextTurnM);
        map_text(cBuf, 32.0f,
                 (float)(MAP_HUD_X + 76), (float)MAP_HUD_TOP_Y,
                 (float)(MAP_HUD_W - 86), (float)MAP_HUD_H,
                 NEMA_VG_ALIGNX_LEFT | NEMA_VG_ALIGNY_CENTER,
                 MAP_TEXT_COLOR);
    }

}

//*****************************************************************************
//
//! @brief The screen shown when the map is unavailable.
//!
//! @details Drawing the map needs both PSRAM (where the tiles go) and eMMC (where they
//! come from). Saying which one failed rather than leaving a black screen -- black
//! looks like a crash, while this page failing affects no other page at all.
//!
//! The second line must state the real cause. It used to be hard-coded as "PSRAM INIT
//! FAILED", so a failed eMMC mount still accused PSRAM, and it took a round of
//! investigation to find the problem was somewhere else entirely.
//
//*****************************************************************************
static void
map_draw_unavailable(void)
{
    const char *pcReason;
    const char *pcStage = "";
    int         i32Code = AssetStoreGetFailure(&pcStage);
    char        cDetail[32];

    if ( !psram_is_ready() && !AssetStoreIsReady() )
    {
        pcReason = "PSRAM + eMMC FAILED";
    }
    else if ( !psram_is_ready() )
    {
        pcReason = "PSRAM INIT FAILED";
    }
    else if ( !AssetStoreIsReady() )
    {
        pcReason = "eMMC MOUNT FAILED";
    }
    else
    {
        //
        // Both are fine yet we got here: the tile files themselves are wrong (missing or the wrong length).
        //
        pcReason = "MAP TILES MISSING";
    }

    map_text("MAP UNAVAILABLE", 28.0f,
             0.0f, (float)(FB_RESY / 2 - 52), (float)FB_RESX, 40.0f,
             NEMA_VG_ALIGNX_CENTER | NEMA_VG_ALIGNY_CENTER,
             MAP_TEXT_COLOR);

    map_text(pcReason, 20.0f,
             0.0f, (float)(FB_RESY / 2 - 4), (float)FB_RESX, 30.0f,
             NEMA_VG_ALIGNX_CENTER | NEMA_VG_ALIGNY_CENTER,
             MAP_DIM_COLOR);

    //
    // The third line gives FatFs's stage and error code.
    //
    // The boot-phase log is flooded out of the RTT buffer by the PSRAM timing scan and
    // disappears entirely, while the screen is not -- for diagnosing why eMMC will not
    // mount, these three words plus a number are more reliable than the log. Against
    // FRESULT in ff.h: 1=DISK_ERR 3=NOT_READY 13=NO_FILESYSTEM 14=MKFS_ABORTED
    // 19=INVALID_PARAMETER.
    //
    if ( pcStage[0] != '\0' )
    {
        memset(cDetail, 0, sizeof(cDetail));
        am_util_stdio_snprintf(cDetail, sizeof(cDetail), "%s ERR %d",
                               pcStage, i32Code);

        map_text(cDetail, 18.0f,
                 0.0f, (float)(FB_RESY / 2 + 30), (float)FB_RESX, 26.0f,
                 NEMA_VG_ALIGNX_CENTER | NEMA_VG_ALIGNY_CENTER,
                 MAP_DIM_COLOR);
    }
}

//*****************************************************************************
//
//! @brief Draw the map navigation page.
//!
//! @param fb The target frame buffer
//! @param cl The command list that records the drawing commands
//!
//! @return None
//
//*****************************************************************************
static void
draw_map(img_obj_t *fb, nema_cmdlist_t *cl)
{
    map_view_t sView;

    MapTaskGetView(&sView);

    nema_cl_rewind(cl);
    nema_cl_bind_circular(cl);

    nema_bind_dst_tex(fb->bo.base_phys, fb->w, fb->h, fb->format, fb->stride);
    nema_set_clip(0, 0, fb->w, fb->h);

    //
    // The background colour is taken from the map's own blocks (#223348, the mode of the top and bottom edges of the three tiles).
    //
    // It shows in two cases: the strip exposed at the screen edge when the camera
    // pushes vertically, and the gap where a tile is not in the cache yet. In the map's
    // own background colour the first looks like the map continuing and the second like
    // a block not finished drawing -- in black, both would be obvious breakage.
    //
    nema_clear(CC_ABGR(0xff, 0x22, 0x33, 0x48));

    if ( !sView.bReady )
    {
        map_draw_unavailable();

        //
        // The brightness panel is a global feature and must still open when the map is unavailable.
        //
        if ( g_bBlOpen )
        {
            bl_draw();
        }

        nema_cl_submit(cl);
        return;
    }

    map_draw_tiles(sView.fCamX, sView.fCamY, sView.fZoom);
    map_draw_car(&sView);
    map_draw_hud(&sView);

    if ( g_bBlOpen )
    {
        bl_draw();
    }

    //
    // Submit this frame's commands.
    //
    // Without this line the screen is still "almost" right, because
    // nema_cl_bind_circular() submits what has accumulated when the ring buffer fills
    // -- so most of the content still gets drawn, and only the last, not yet full
    // stretch of commands stays in the buffer and is cleared by nema_cl_rewind() on
    // the next frame.
    //
    // The symptom is therefore "the last few draws of each frame come and go": most
    // visible once the brightness panel is overlaid, since the whole of it is at the
    // tail of a frame, and the panel background and the sun icon flicker in and out.
    //
    nema_cl_submit(cl);
}

//*****************************************************************************
//
//! @brief Touch handling for the map page.
//!
//! @details The gesture semantics of this page differ from the others, hence a branch
//! of its own in ui_input_update(): a horizontal swipe here pans the map rather than changing pages.
//!
//! Page changes are not removed entirely but handed to the boundary -- with the map
//! already pushed fully left (or right), another swipe in the same direction counts as
//! a page change. This is the usual way drag-based interfaces finish: the user need
//! not learn a new gesture to get out, while a swipe mid-map never changes pages by accident.
//!
//! @param psGesture This frame's gesture and live press state
//!
//! @return true when the map page has consumed the gesture and the caller should not treat it as a page change.
//
//*****************************************************************************
static bool
map_input_update(const gesture_t *psGesture)
{
    map_view_t sView;

    MapTaskGetView(&sView);

    //
    // Two fingers down: zoom, and give way to single-finger dragging entirely.
    //
    // It comes before dragging because the two are exclusive and zoom's condition is
    // stricter: the moment the second finger lands it should take over, so the first
    // finger's movement is no longer read as a pan.
    //
    if ( psGesture->bPressed && (psGesture->ui8Fingers >= 2) )
    {
        float fDx   = (float)psGesture->ui16CurX2 - (float)psGesture->ui16CurX;
        float fDy   = (float)psGesture->ui16CurY2 - (float)psGesture->ui16CurY;
        float fDist = sqrtf((fDx * fDx) + (fDy * fDy));

        //
        // No single-finger dragging during a zoom. The accumulated displacement is
        // cleared at the same time, so the release is not misread as a swipe because
        // of how far two fingers moved.
        //
        g_bMapDragging    = false;
        g_i32MapDragTotal = 0;

        if ( fDist < MAP_PINCH_MIN_DIST )
        {
            //
            // Too close; this frame does not count. The previous distance is not updated
            // either -- when the fingers spread again the comparison should start from
            // a trustworthy distance, not from this noise.
            //
            return true;
        }

        if ( !g_bMapPinching )
        {
            //
            // The second finger has just landed: only record the distance, there is no previous frame to compare with.
            //
            g_bMapPinching      = true;
            g_fMapLastPinchDist = fDist;
        }
        else
        {
            //
            // The anchor is the midpoint between the fingers: the part of the map under them stays put between them.
            //
            MapTaskZoom(fDist / g_fMapLastPinchDist,
                        ((float)psGesture->ui16CurX + (float)psGesture->ui16CurX2) * 0.5f,
                        ((float)psGesture->ui16CurY + (float)psGesture->ui16CurY2) * 0.5f);

            g_fMapLastPinchDist = fDist;
        }

        return true;
    }

    //
    // Going from two fingers to one, or releasing entirely: end the zoom and swallow this frame completely.
    //
    // Two reasons for swallowing it:
    //
    //   - With one finger still down, do not let it immediately become the start of a
    //     drag. The remaining finger's coordinates may be either of the original two,
    //     and taking over would make the map jump; waiting for a fresh press is honest.
    //   - On the frame where everything lifts, the gesture layer classifies this press
    //     into a discrete gesture (the path of two moving fingers is usually read as a
    //     horizontal swipe). Without swallowing it, releasing after a pinch would change pages.
    //
    if ( g_bMapPinching )
    {
        g_bMapPinching    = false;
        g_bMapDragging    = false;
        g_i32MapDragTotal = 0;
        return true;
    }

    //
    // While held: hand this frame's displacement to the map.
    //
    if ( psGesture->bPressed )
    {
        if ( !g_bMapDragging )
        {
            //
            // The first frame of this press: only record the start, no displacement yet -- there is no previous frame to compare with.
            //
            g_bMapDragging    = true;
            g_ui16MapLastX    = psGesture->ui16CurX;
            g_i32MapDragTotal = 0;
        }
        else
        {
            int32_t i32Delta = (int32_t)psGesture->ui16CurX
                               - (int32_t)g_ui16MapLastX;

            if ( i32Delta != 0 )
            {
                g_i32MapDragTotal += (i32Delta > 0) ? i32Delta : -i32Delta;
                g_ui16MapLastX     = psGesture->ui16CurX;

                //
                // The map only really starts dragging once the accumulated horizontal displacement passes the threshold.
                //
                // This dead zone is necessary: a vertical gesture (pulling the brightness
                // panel up from the bottom, swiping down to leave this page) cannot be
                // traced as a straight line, and treating a few pixels of lateral jitter
                // as a drag would push the map a little every time the brightness panel
                // is pulled, and put it into free browse mode too -- an unwanted
                // FREE VIEW would appear on screen and only disappear three seconds later.
                //
                // The threshold shares a constant with the tap test, consistently: within
                // 8 px counts as not having moved. A genuine drag loses only those first
                // few pixels, which is imperceptible.
                //
                if ( g_i32MapDragTotal >= MAP_DRAG_MIN_PX )
                {
                    MapTaskDrag((float)i32Delta);
                }
            }
        }

        return false;
    }

    //
    // This is only reached after release.
    //
    if ( psGesture->eType == GESTURE_NONE )
    {
        g_bMapDragging = false;
        return false;
    }

    switch ( psGesture->eType )
    {
        case GESTURE_SWIPE_LEFT:
        case GESTURE_SWIPE_RIGHT:
        {
            bool bAtEdge;

            //
            // Decide whether this swipe's direction has already reached the edge of the
            // map. Swiping left pushes the map left and moves the camera right, so the
            // right boundary is what matters.
            //
            // 0.5 px of tolerance rather than strict equality: the camera is a float, and
            // the interpolation of follow mode leaves it extremely close to but not equal
            // to the boundary.
            //
            bAtEdge = (psGesture->eType == GESTURE_SWIPE_LEFT)
                      ? (sView.fCamX >= ((float)(MAP_WORLD_W - FB_RESX) - 0.5f))
                      : (sView.fCamX <= 0.5f);

            g_bMapDragging    = false;
            g_i32MapDragTotal = 0;

            //
            // At the edge the gesture is handed over for a page change; otherwise it is
            // swallowed -- the map has already moved with the finger, and changing pages
            // as well would be "drag the map and end up on another page".
            //
            return !bAtEdge;
        }

        case GESTURE_TAP:
            //
            // A tap anywhere on screen: stop while moving, resume while stopped; after
            // arrival it restarts the whole navigation.
            //
            // Having no dedicated button is deliberate -- this is a watch, a round panel
            // has little room for buttons to begin with, and "stop a moment to see the
            // road ahead" is the most frequent action while navigating, which earns it
            // the whole screen. The map can still be dragged while stopped, and
            // stopping is usually exactly for dragging over to look at a junction.
            //
            // An accidental tap during a drag does not count: a press that moved past
            // the threshold should trigger nothing at release, even if it is classified as a tap.
            //
            if ( g_i32MapDragTotal < MAP_DRAG_MIN_PX )
            {
                if ( sView.bArrived )
                {
                    MapTaskRestart();
                }
                else
                {
                    MapTaskTogglePause();
                }
            }

            g_bMapDragging    = false;
            g_i32MapDragTotal = 0;
            return true;

        case GESTURE_SWIPE_UP:
            //
            // The brightness panel cannot be pulled out while moving.
            //
            // Brightness is a global feature available on every other page, and this is
            // the only exception: the screen is changing continuously during navigation,
            // an upward swipe from the bottom is very likely "pull the road ahead into
            // view" rather than "adjust the brightness", and a panel covering half the
            // screen would interrupt what is being watched.
            //
            // Once stopped it behaves normally -- the user is no longer following the road. The same after arrival.
            //
            if ( (!sView.bPaused) && (!sView.bArrived) )
            {
                g_bMapDragging    = false;
                g_i32MapDragTotal = 0;
                return true;
            }
            break;

        case GESTURE_SWIPE_DOWN:
            //
            // Swipe down from the centre of the screen to leave this page.
            //
            // The "swipe at the map boundary to change pages" path still works, but it
            // requires dragging the map all the way first -- leaving from the middle of a
            // route would take five or six swipes. The vertical direction has no other
            // use on this page at all (the world is as tall as the screen), which makes
            // it the right thing to use for leaving.
            //
            // The pull-down-from-the-top variant does not count: that is the control
            // centre's gesture, and although the control centre exists only on the home
            // page, it should still not be read as something else here.
            //
            if ( psGesture->ui16MinY > CC_PULL_START_MAX_Y )
            {
                g_bMapDragging    = false;
                g_i32MapDragTotal = 0;
                ui_page_set(UI_PAGE_TREND);
                return true;
            }
            break;

        default:
            break;
    }

    g_bMapDragging    = false;
    g_i32MapDragTotal = 0;

    //
    // Gestures not picked up above are handed back to the shared handling.
    //
    // What reaches here is mainly "an upward swipe after stopping or arriving" -- the
    // brightness panel should open as usual then (the branch above only blocks it while
    // moving). The map itself needs no vertical swiping: the world is as tall as the
    // screen, so there is nothing to look at above or below.
    //
    return false;
}

//*****************************************************************************
//
//! @brief Change page and log it.
//!
//! @param ePage The target page
//
//*****************************************************************************
static void
ui_page_set(ui_page_e ePage)
{
    static const char * const pcPageNames[UI_PAGE_COUNT] =
    {
        "home", "watch", "temp", "trend", "map"
    };

    if ( (ePage >= UI_PAGE_COUNT) || (ePage == g_ePage) )
    {
        return;
    }

    g_ePage = ePage;

    //
    // Tell the map task whether to keep the navigation simulation running. It pauses on
    // leaving this page and resumes on return -- quietly finishing the whole route while
    // another page is showing would mean coming back to an already-arrived screen.
    //
    // The drag state is cleared at the same time: the finger has necessarily left the
    // screen on a page change, and keeping it would make the first press after
    // returning compute its displacement from the previous visit's coordinates and
    // jump the map.
    //
    MapTaskSetActive(ePage == UI_PAGE_MAP);
    g_bMapDragging    = false;
    g_bMapPinching    = false;
    g_i32MapDragTotal = 0;

    //
    // Entering the generated page pulls the animation's time base up to now.
    //
    // The generated side only updates the base when draw_design() calls tick_to(), and
    // it stops at the last frame's time while away from this page; without the reset,
    // the first frame back would see a delta as large as "how long we were away" and
    // the rotation animation in progress would jump to its end instantly.
    //
    // The generated page currently sits after UI_PAGE_COUNT and the range check above
    // already rejects it, so this is not reached in practice; it stays so that moving
    // it back into the carousel later needs no rewriting.
    //
    if ( ePage == UI_PAGE_DESIGN )
    {
        ui_designer_resync(TICKS_TO_MS(xTaskGetTickCount()));
    }

    //
    // The control centre and the quick page menu exist only on the home page. Both are
    // closed on leaving, so returning never shows an overlay left open from last time.
    //
    if ( ePage != UI_PAGE_HOME )
    {
        g_bMenuOpen     = false;
        g_bPageMenuOpen = false;
    }

    //
    // The brightness panel exists on every page but is still closed on a page change:
    // horizontal swipes are ignored while an overlay is open, and the navigation menus
    // are exclusive with it. Clearing it explicitly keeps the post-change screen
    // independent of the order of the gesture branches.
    //
    g_bBlOpen = false;
    g_bBlDrag = false;

    //
    // The watch face selector exists only on the face page and is always closed on a page change.
    //
    // Two paths reach here: Button1 (where the user explicitly wants to leave), and a
    // left or right swipe while the selector is closed. The second cannot happen with
    // the selector open -- the gesture would already have been consumed by
    // fsel_input(). So this line really only serves Button1, but it lives here rather
    // than with the button so that "the screen is clean after a page change" does not
    // depend on who runs first.
    //
    fsel_close(false);

    //
    // The temperature is sampled continuously by SensorTask and no forced read is needed
    // on a page change; entering the temperature page always shows a value at most one
    // sampling period old.
    //
    am_util_debug_printf("[render] page -> %s\n", pcPageNames[g_ePage]);
}

//*****************************************************************************
//
//! @brief Perform the action of one control centre cell.
//!
//! @param i32Action The cc_action_e returned by cc_hit_test()
//
//*****************************************************************************
static void
cc_action_run(int32_t i32Action)
{
    switch ( i32Action )
    {
        case CC_ACT_LED2:
            LedTaskToggle(LED_ID_LED2);
            am_util_debug_printf("[render] cc: LED2(GP11) -> %s\n",
                                 LedTaskGetState(LED_ID_LED2) ? "ON" : "OFF");
            break;

        case CC_ACT_LED4:
            LedTaskToggle(LED_ID_LED4);
            am_util_debug_printf("[render] cc: LED4(GP83) -> %s\n",
                                 LedTaskGetState(LED_ID_LED4) ? "ON" : "OFF");
            break;

        case CC_ACT_GO_WATCH:
            //
            // ui_page_set() closes the menu along the way, so nothing to clear here.
            //
            ui_page_set(UI_PAGE_WATCH);
            break;

        case CC_ACT_GO_TEMP:
            ui_page_set(UI_PAGE_TEMP);
            break;

        default:
            //
            // A tap in the gap between cells or on the panel edge: do nothing, and do
            // not close the menu either. Closing on a near miss would be infuriating
            // when going for the second LED.
            //
            break;
    }
}

//*****************************************************************************
//
//! @brief Perform an action of the home page quick page menu.
//
//*****************************************************************************
static void
page_menu_action_run(int32_t i32Action)
{
    switch ( i32Action )
    {
        case PAGE_MENU_ACT_TREND:
            ui_page_set(UI_PAGE_TREND);
            break;

        case PAGE_MENU_ACT_MAP:
            ui_page_set(UI_PAGE_MAP);
            break;

        default:
            // A tap in the gap between the two entries keeps the menu open.
            break;
    }
}

//*****************************************************************************
//
// The watch face selector.
//
// Interaction: swipe down on the face page and the face shrinks away to the left while
// a wheel menu swings in on the right; a swipe up or down (or left or right) turns it
// one notch; a tap applies and zooms back to full screen; Button1 cancels.
//
// Each notch of the wheel blits that face's dial asset itself rather than a
// pre-rendered thumbnail. So no separate thumbnail assets are needed, and no offscreen
// buffer is needed to "compose the face before rotating the whole thing" -- the cost
// is that the notches carry no hands, but the hands are on the running preview to the
// left, so no information is lost.
//
// Why not rotate the whole face (hands included): nema_blit_quad_fit() blits the
// currently bound texture, and a face is a dial plus three hands stacked up. Making
// them share one perspective would first require
// compositing them into an offscreen buffer and blitting that -- a whole block of
// PSRAM plus one composition per switch, which is not worth it for a few barely
// legible hands on the wheel.
//
//*****************************************************************************

//
// Animation durations. Shrinking and zooming take longer than turning a notch: that
// is "entering and leaving a mode" and deserves to be seen; turning a notch is a
// repeated action and has to be quick enough to keep up with continuous swiping.
//
#define FSEL_ANIM_MS        (240)
#define FSEL_WHEEL_MS       (200)

//
// Size and position of the preview once it retreats to the left.
//
// The values follow from the panel being round: the preview is round too, so as long
// as "distance from the preview's centre to the panel's centre plus the preview's
// radius" stays under the panel radius (234), nothing is clipped.
//
// Currently: the preview is about 272 px across (radius 136) with its centre at
// x=162, 72 from the panel's centre, and 72 + 136 = 208 < 234, leaving 26 px to
// spare. Any larger or any further left and it starts touching the edge.
//
// The other limit is the menu on the right: the preview's right edge is at 298 and the alignment frame's left edge at 321, 23 px apart.
//
#define FSEL_PREVIEW_SCALE  (0.58f)
#define FSEL_PREVIEW_CX     (162)

//
// The wheel. The notches are always upright squares, and the sense of rotation comes
// from three things: sliding along the vertical (PITCH), shrinking with distance from
// the centre (SHRINK), and drawing the two ends leftward into an arc (ARC).
//
// This used to be a real 3D drum (nema_mat4x4_* with nema_blit_quad_fit), and it was
// unusable on hardware: a 58 degree tilt left the top and bottom notches at 53% of
// their height with no horizontal shrink, squashing the round dials into wide flat
// ellipses that were hard to recognize. Reducing the perspective to where nothing
// distorted left almost no sense of depth -- both ends of that road were a bad deal,
// hence the present approach.
//
// SPAN is the visible range (how many notches from the centre still get drawn). 1.35
// is set by the round panel: any further and a notch's bottom edge crosses the
// panel's circle and gets a corner clipped.
//
#define FSEL_WHEEL_CX       (372)
#define FSEL_WHEEL_CELL     (92)
#define FSEL_WHEEL_PITCH    (96)
#define FSEL_WHEEL_ARC      (14.0f)
#define FSEL_WHEEL_SHRINK   (0.22f)
#define FSEL_WHEEL_SPAN     (1.35f)

//
// How many notches are drawn above and below the centre one. With two faces only three
// are ever drawn (+-116 degrees is already facing away); the value is for the day there are more.
//
#define FSEL_WHEEL_VISIBLE  (2)

//
// How far beyond the right edge of the screen the wheel slides in from (pixels).
//
#define FSEL_WHEEL_ENTER_DX (140.0f)

typedef enum
{
    FSEL_OFF = 0,   // closed; the face page draws as usual
    FSEL_ENTER,     // the shrink-and-retreat animation
    FSEL_BROWSE,    // browsing (the notch-turning animation counts as this state)
    FSEL_EXIT       // the zoom-back animation after applying
} fsel_state_e;

static fsel_state_e g_eFsel = FSEL_OFF;

//
// Start of the ENTER / EXIT animation.
//
static uint32_t g_ui32FselTick;

//
// The wheel's target position and the start of the notch animation.
//
// A "continuous index" rather than 0..WATCHFACE_COUNT-1: turning down from the last
// one has to continue in the same direction, and taking the modulo would spin the
// wheel back a full turn. Which face is actually shown is derived by fsel_index()
// taking the modulo.
//
static int32_t  g_i32FselTarget;
static float    g_fFselWheelFrom;
static uint32_t g_ui32FselWheelTick;

//*****************************************************************************
//
//! @brief Animation progress, 0 to 1, smoothed at both ends.
//!
//! @details smoothstep: slow to start and slow to finish, fast in the middle. A linear
//! version comes to an abrupt halt at the end, which is especially visible on a scaling animation.
//!
//! The ticks are subtracted before being converted to milliseconds, and the order
//! cannot be reversed -- ticks wrap to 0, and converting first would compute an
//! enormous elapsed time at the wrap and jump the animation straight to its end.
//
//*****************************************************************************
static float
fsel_ease(uint32_t ui32StartTick, uint32_t ui32DurationMs)
{
    uint32_t ui32Elapsed = TICKS_TO_MS(xTaskGetTickCount() - ui32StartTick);
    float    fT;

    if ( ui32Elapsed >= ui32DurationMs )
    {
        return 1.0f;
    }

    fT = (float)ui32Elapsed / (float)ui32DurationMs;

    return fT * fT * (3.0f - 2.0f * fT);
}

//*****************************************************************************
//
//! @brief Take the modulo of a continuous index to get a subscript into g_sFaces[].
//
//*****************************************************************************
static uint32_t
fsel_index(int32_t i32Pos)
{
    int32_t i32Count = (int32_t)WATCHFACE_COUNT;
    int32_t i32Mod   = i32Pos % i32Count;

    return (uint32_t)((i32Mod < 0) ? (i32Mod + i32Count) : i32Mod);
}

//*****************************************************************************
//
//! @brief Round to the nearest integer index.
//!
//! @details Not lroundf(): the wheel position may be negative, and all that is needed
//! here is a symmetric rounding that does not depend on libm.
//
//*****************************************************************************
static int32_t
fsel_round(float fValue)
{
    return (int32_t)((fValue >= 0.0f) ? (fValue + 0.5f) : (fValue - 0.5f));
}

//*****************************************************************************
//
//! @brief Where the wheel is currently turned to (continuous index).
//
//*****************************************************************************
static float
fsel_wheel_pos(void)
{
    float fT = fsel_ease(g_ui32FselWheelTick, FSEL_WHEEL_MS);

    return g_fFselWheelFrom
           + (((float)g_i32FselTarget - g_fFselWheelFrom) * fT);
}

//*****************************************************************************
//
//! @brief Whether the selector is open (including during the animations).
//
//*****************************************************************************
static bool
fsel_active(void)
{
    return (g_eFsel != FSEL_OFF);
}

//*****************************************************************************
//
//! @brief Open the selector.
//
//*****************************************************************************
static void
fsel_open(void)
{
    if ( g_eFsel != FSEL_OFF )
    {
        return;
    }

    g_eFsel             = FSEL_ENTER;
    g_ui32FselTick      = xTaskGetTickCount();
    g_i32FselTarget     = (int32_t)g_ui32Face;
    g_fFselWheelFrom    = (float)g_i32FselTarget;
    g_ui32FselWheelTick = g_ui32FselTick;

    //
    // All three overlays are closed: the selector is a full-screen modal layer, and with
    // another panel underneath, the release of a finger would be read by both sides.
    //
    g_bBlOpen       = false;
    g_bBlDrag       = false;
    g_bMenuOpen     = false;
    g_bPageMenuOpen = false;

    am_util_debug_printf("[render] face sel: open (%s)\n",
                         g_sFaces[g_ui32Face].pcName);
}

//*****************************************************************************
//
//! @brief Close the selector.
//!
//! @param bApply true applies the currently aligned face and plays the zoom-back animation;
//!               false simply closes.
//!
//! @details Cancelling plays no animation deliberately: the only cancel path is Button1,
//! and that button also switches to the home page, so a zoom animation would be covered
//! by the page change halfway through and look like a flicker.
//
//*****************************************************************************
static void
fsel_close(bool bApply)
{
    if ( g_eFsel == FSEL_OFF )
    {
        return;
    }

    if ( !bApply )
    {
        g_eFsel = FSEL_OFF;
        return;
    }

    g_ui32Face     = fsel_index(g_i32FselTarget);
    g_eFsel        = FSEL_EXIT;
    g_ui32FselTick = xTaskGetTickCount();

    am_util_stdio_printf("[render] face sel: apply %s\n",
                         g_sFaces[g_ui32Face].pcName);

    //
    // Diagnostic: on applying, print the assets and pivots of all three faces. See faces_dump().
    //
    faces_dump();
}

//*****************************************************************************
//
//! @brief Turn the wheel one notch.
//!
//! @param i32Delta +1 for the next face, -1 for the previous.
//!
//! @details The start is taken from "where the wheel actually is" rather than the
//! previous target: swiping again before the last turn has finished should continue
//! from where it is on screen, not jump back and start over.
//
//*****************************************************************************
static void
fsel_step(int32_t i32Delta)
{
    g_fFselWheelFrom    = fsel_wheel_pos();
    g_i32FselTarget    += i32Delta;
    g_ui32FselWheelTick = xTaskGetTickCount();
}

//*****************************************************************************
//
//! @brief Handle the selector's gestures.
//!
//! @return true when the selector has consumed this gesture and the caller should not handle it.
//!
//! @details The selector is a full-screen modal layer and intercepts every gesture while
//! open -- including those that would normally change pages or pull out the brightness
//! panel. Without that, swiping up one notch would also pull out the brightness panel.
//
//*****************************************************************************
static bool
fsel_input(const gesture_t *psGesture)
{
    if ( g_eFsel == FSEL_OFF )
    {
        return false;
    }

    //
    // During a hold and during the zoom animation, gestures are only swallowed: the
    // selector has nothing that tracks the finger, and with the apply animation already
    // running, taking another gesture would only switch to a different face halfway
    // through the zoom.
    //
    if ( psGesture->bPressed || (g_eFsel == FSEL_EXIT) )
    {
        return true;
    }

    switch ( psGesture->eType )
    {
        //
        // Swiping up turns the wheel to the next face -- as with a scrolling list (finger
        // up, content up). Left and right are accepted too: with only two faces, most
        // people's first instinct is to flick sideways.
        //
        case GESTURE_SWIPE_UP:
        case GESTURE_SWIPE_LEFT:
            fsel_step(1);
            break;

        case GESTURE_SWIPE_DOWN:
        case GESTURE_SWIPE_RIGHT:
            fsel_step(-1);
            break;

        case GESTURE_TAP:
            fsel_close(true);
            break;

        default:
            break;
    }

    return true;
}

//*****************************************************************************
//
//! @brief Draw notch i32Pos (continuous index) of the wheel, skipping those out of the visible range.
//!
//! @details The notches are always upright squares, with no rotation and no perspective
//! -- a dial is round, and any tilt squashes it into an ellipse, which makes it hard to
//! tell which face it is. The sense of rotation is carried jointly by three things:
//! sliding along the vertical, shrinking with distance from the centre, and drawing the
//! two ends leftward into an arc.
//!
//! @param i32Pos    Which notch to draw (continuous index, may fall outside 0..WATCHFACE_COUNT-1).
//! @param fWheelPos Where the wheel is currently turned to.
//! @param fDx       Horizontal offset of the whole wheel (pixels), for the entry animation.
//! @param i32CY     Vertical position of the wheel's centre.
//
//*****************************************************************************
static void
fsel_wheel_at(int32_t i32Pos, float fWheelPos, float fDx, int32_t i32CY)
{
    const watchface_t *psFace = &g_sFaces[fsel_index(i32Pos)];
    img_obj_t         *psDial = psFace->psDial;
    float              fDist  = (float)i32Pos - fWheelPos;
    float              fAbs   = (fDist < 0.0f) ? -fDist : fDist;
    float              fSize;
    float              fCX;
    float              fCY;

    if ( (fAbs > FSEL_WHEEL_SPAN) || (psDial->bo.base_virt == NULL) )
    {
        return;
    }

    //
    // Smaller the further from the centre. This is the only cue that a notch is "further
    // away"; without it all three are the same size and it reads as a row of icons side
    // by side rather than a wheel.
    //
    fSize = (float)FSEL_WHEEL_CELL * (1.0f - (fAbs * FSEL_WHEEL_SHRINK));

    //
    // Evenly spaced vertically; horizontally they draw further left the further from the centre, bending the two ends into an arc.
    //
    // Leftward rather than rightward: the panel is round, and the available width on the
    // right narrows towards the top and bottom, so the ends of a rightward arc would run
    // straight into the panel edge.
    //
    fCY = (float)i32CY + (fDist * (float)FSEL_WHEEL_PITCH);
    fCX = (float)FSEL_WHEEL_CX + fDx - (fAbs * FSEL_WHEEL_ARC);

    nema_bind_src_tex(psDial->bo.base_phys,
                      psDial->w,
                      psDial->h,
                      psDial->format,
                      psDial->stride,
                      NEMA_FILTER_BL);

    nema_set_blend_blit(NEMA_BL_SRC);
    nema_blit_rect_fit((int32_t)(fCX - (fSize * 0.5f) + 0.5f),
                       (int32_t)(fCY - (fSize * 0.5f) + 0.5f),
                       (int32_t)(fSize + 0.5f),
                       (int32_t)(fSize + 0.5f));
}

//*****************************************************************************
//
//! @brief Draw the whole selector screen.
//!
//! @details The animation state transitions happen here too (ENTER becomes BROWSE when
//! complete, EXIT becomes OFF). They live in the drawing rather than a separate tick
//! function because the progress is computed from the tick every frame anyway, and
//! splitting it would only create a chance of computing it twice in one frame.
//!
//! @param fb   The target frame buffer.
//! @param cl   The command list.
//! @param fSec The time the preview should show, from the same source as the face page.
//
//*****************************************************************************
static void
fsel_draw(nema_img_obj_t *fb, nema_cmdlist_t *cl, float fSec)
{
    const watchface_t *psPreview;
    float    fT;
    float    fWheelPos;
    float    fWheelDx;
    float    fPreviewCX;
    float    fScale;
    float    fRingR;
    int32_t  i32Base;
    int32_t  i32Step;
    uint32_t ui32Shown;

    nema_cl_rewind(cl);
    nema_cl_bind_circular(cl);

    nema_bind_dst_tex(fb->bo.base_phys,
                      fb->w,
                      fb->h,
                      fb->format,
                      fb->stride);

    nema_set_clip(0, 0, fb->w, fb->h);

    //
    // Animation progress: ENTER runs 0 to 1 (shrinking away), EXIT runs 1 to 0 (zooming back),
    // and BROWSE is always 1.
    //
    if ( g_eFsel == FSEL_ENTER )
    {
        fT = fsel_ease(g_ui32FselTick, FSEL_ANIM_MS);

        if ( fT >= 1.0f )
        {
            g_eFsel = FSEL_BROWSE;
        }
    }
    else if ( g_eFsel == FSEL_EXIT )
    {
        fT = 1.0f - fsel_ease(g_ui32FselTick, FSEL_ANIM_MS);

        if ( fT <= 0.0f )
        {
            g_eFsel = FSEL_OFF;
        }
    }
    else
    {
        fT = 1.0f;
    }

    fWheelPos = fsel_wheel_pos();

    //
    // The preview shows "whichever notch the wheel is closest to", so it changes only
    // once a turn is past halfway -- which reads as the same thing the wheel is doing.
    // During EXIT it stays on the face that was applied: the wheel is already leaving by
    // then, and following it would flicker halfway through the zoom.
    //
    ui32Shown = (g_eFsel == FSEL_EXIT) ? g_ui32Face
                                       : fsel_index(fsel_round(fWheelPos));
    psPreview = &g_sFaces[ui32Shown];

    //
    // The preview's size and position both interpolate from "full screen" to "small image
    // on the left". At fT = 0 it is exactly what draw_watch() produces, so there is no
    // jump at the moment of entry.
    //
    fScale = face_fit_scale(psPreview, fb->w)
             * (1.0f + ((FSEL_PREVIEW_SCALE - 1.0f) * fT));

    fPreviewCX = (float)(fb->w / 2)
                 + (((float)FSEL_PREVIEW_CX - (float)(fb->w / 2)) * fT);

    nema_clear(0xff000000);

    face_render(psPreview, fsel_round(fPreviewCX), fb->h / 2, fScale, fSec);

    //
    // The wheel slides in from beyond the right edge on entry and back out on exit. The
    // unit is pixels -- the wheel goes through no 3D projection, and every number in the
    // layout is a screen coordinate directly.
    //
    fWheelDx = (1.0f - fT) * FSEL_WHEEL_ENTER_DX;

    i32Base = fsel_round(fWheelPos);

    //
    // Drawn outside in: adjacent notches overlap slightly on screen and the centre one
    // should be on top. There is no depth test, so the order is the only occlusion rule.
    //
    for ( i32Step = FSEL_WHEEL_VISIBLE; i32Step > 0; i32Step-- )
    {
        fsel_wheel_at(i32Base - i32Step, fWheelPos, fWheelDx, fb->h / 2);
        fsel_wheel_at(i32Base + i32Step, fWheelPos, fWheelDx, fb->h / 2);
    }

    fsel_wheel_at(i32Base, fWheelPos, fWheelDx, fb->h / 2);

    //
    // The alignment frame. It does not turn with the wheel, like the window in front of a
    // physical dial -- the dial turns, the frame is the fixed reference, and without it
    // there is no telling which notch counts as selected.
    //
    // A round frame rather than a square one: both dials are round, and a square frame would leave the four corners empty.
    //
    // The alpha fades in with fT: drawing it fully partway through the entry would catch
    // the eye before the wheel itself, as if the frame arrived first and the dial followed.
    //
    fRingR = ((float)FSEL_WHEEL_CELL * 0.5f) + 5.0f;

    nema_set_blend_fill(NEMA_BL_SIMPLE);
    nema_draw_circle_aa((float)FSEL_WHEEL_CX + fWheelDx,
                        (float)(fb->h / 2),
                        fRingR,
                        2.0f,
                        ((uint32_t)(fT * 200.0f) << 24) | 0x00ffffffU);

    nema_cl_submit(cl);
}

//*****************************************************************************
//
//! @brief Draw the given page into the framebuffer.
//!
//! @details Extracted from the main loop's if/else chain, because the page overview also
//! has to draw pages: its thumbnail is "draw a whole page as usual, then capture and
//! shrink it", so the drawing has to be shared by two call sites. Before extraction the
//! main loop and the overview each had their own dispatch, and adding a page meant
//! changing both.
//!
//! The time is passed in by the caller rather than computed in each branch: the face page
//! and the trend page both need it, and computing it twice would let the hands and the
//! digits of one frame land on different ticks.
//!
//! @param ePage The page to draw.
//! @param fb    The target framebuffer.
//! @param cl    The command list. The function rewinds and submits itself, but does **not** wait for completion.
//! @param fSec  This frame's time (seconds since boot plus the start offset).
//!
//! @return None
//
//*****************************************************************************
static void
draw_page(ui_page_e ePage, nema_img_obj_t *fb, nema_cmdlist_t *cl, float fSec)
{
    if ( ePage == UI_PAGE_WATCH )
    {
        //
        // Draw the watch face. While the face selector is open it takes over the whole
        // screen -- the preview's time comes from the same source as here, so the hands
        // do not jump at the instant it shrinks away.
        //
#if WATCHFACE_ENABLE_TOUCH
        if ( fsel_active() )
        {
            fsel_draw(fb, cl, fSec);
        }
        else
#endif
        {
            draw_watch(fb, cl, fSec);
        }
    }
    else if ( ePage == UI_PAGE_TEMP )
    {
        draw_temp(fb, cl);
    }
    else if ( ePage == UI_PAGE_TREND )
    {
        //
        // The time comes from the same source as the face page, so the two never disagree when switching.
        //
        draw_trend(fb, cl, fSec);
    }
    else if ( ePage == UI_PAGE_MAP )
    {
        //
        // The tiles come from PSRAM; the position and navigation information from MapTask.
        //
        draw_map(fb, cl);
    }
    else if ( ePage == UI_PAGE_DESIGN )
    {
        //
        // The generated page has left the carousel (see ui_page_e) and this branch is
        // currently unreachable; it stays so draw_design() still has a call site and is
        // not treated as an unused function.
        //
        draw_design(fb, cl);
    }
    else
    {
        draw_home(fb, cl);
    }
}

//
// This #if is nested -- from the map section onwards this file is already wrapped
// entirely in #if WATCHFACE_ENABLE_TOUCH, so it is not currently load-bearing. It stays
// because "the overview is a purely touch feature" belongs with the overview itself
// rather than resting on an outer block so large that its beginning is not visible from
// here. If that outer block is ever narrowed, nothing need be added here.
//
#if WATCHFACE_ENABLE_TOUCH

//*****************************************************************************
//
// The page overview (Button0 / SW1).
//
// Interaction: BTN0 on any page shrinks the current page into a small image at the
// centre with a row of page dots below; left and right swipes change pages quickly; a
// tap or another BTN0 zooms back to full screen.
//
// Its relationship with the watch face selector (fsel_*): the interaction semantics are
// deliberately identical -- ENTER / BROWSE / EXIT, the same easing (fsel_ease() is
// borrowed directly), and *_input() swallowing gestures the same way. But the two cannot
// share an implementation, and the difference is in what gets shrunk:
//
//   face selector  shrinks one watch face. face_render(cx, cy, scale, sec) already takes
//                  a centre and a factor and can draw at any size on demand, needing no extra memory.
//   page overview  shrinks a whole page. draw_home() / draw_temp() and the rest have
//                  coordinates hard-coded against the framebuffer (FB_RESX / 2 and so on), with no factor to pass.
//
// So this uses "draw then capture":
//
//   pass 1  draw_page()      draw the whole page into the framebuffer as usual (full size)
//   pass 2  psel_capture()   the framebuffer as source, shrunk into g_sPselThumb
//   pass 3  psel_compose()   clear the screen, blit the thumbnail back per the animation, then draw the dots
//
// Each pass submits and waits for completion. In one command list, pass 2's source would
// be pass 1's target and pass 3's target would be pass 2's source -- the GPU does execute
// a list in order, but that is a write-after-read and a read-after-write landing on the
// same tiles, and there is no need to gamble on it to save two round trips.
//
// Why not change every draw_*() to take (cx, cy, scale): that would be hundreds of
// coordinates across seven thousand lines, each needing the layout reworked. The capture
// approach costs one fixed buffer and two blits, independent of the number of pages or their complexity.
//
// Cost one: there is only one thumbnail, so a page change cannot show "the old page
// sliding out while the new slides in" -- the animation is the new page sliding in from
// the side (see psel_step()). Two pages on screen at once would need another buffer of
// the same size (about 136 KB), and the GPU pool has no such headroom (see the VMEM_SIZE
// note in gcc/Makefile).
//
// Cost two: the first part of the ENTER / EXIT animation scales a 264 px thumbnail up to
// nearly 468 px, softer than drawing full size. It lasts only 240 ms while the screen is
// moving and is invisible on hardware; "draw full size during the animation and use the
// thumbnail only in BROWSE" would instead jump on the frame where they hand over.
//
//*****************************************************************************

//
// Animation durations. The same values as the watch face selector: the two are the same
// kind of operation (entering and leaving a mode / moving one notch) and have no reason to feel different.
//
#define PSEL_ANIM_MS        (240)
#define PSEL_SLIDE_MS       (200)

//
// The shrunk edge factor and the centre height.
//
// The values follow from the panel being round while the thumbnail is square -- a
// square's corners reach the circle before its edges do, so it is the corners that have
// to be computed:
//
//   edge          468 x 0.56 = 262, half-edge 131
//   corner to thumbnail centre  131 x 1.414 = 185
//   thumbnail centre raised     234 - 206 = 28
//   corner to panel centre      185 + 28 = 213 < 234 (the radius), 21 px to spare
//
// Raising it 28 px leaves room below for the dots. Any larger or any higher and the corners start to be clipped.
//
#define PSEL_PREVIEW_SCALE  (0.56f)
#define PSEL_PREVIEW_CY     (206)

//
// How far from the side a new page slides in on a page change (pixels).
//
// 210 is about 80% of the thumbnail's edge: enough to read as "a different one", without
// sliding so far off screen that 200 ms feels rushed.
//
#define PSEL_SLIDE_DX       (210.0f)

//
// The page dots.
//
// y = 388 is 154 from the panel centre, where the half-chord is
// sqrt(234^2 - 154^2) = 176; five dots 26 apart span only 52 of that half-width, far from
// the edge. The thumbnail's bottom edge is at 337, leaving 51 px in between.
//
#define PSEL_DOT_CY         (388)
#define PSEL_DOT_PITCH      (26)
#define PSEL_DOT_R          (4.0f)
#define PSEL_DOT_R_ON       (6.5f)

typedef enum
{
    PSEL_OFF = 0,   // closed; pages draw full screen as usual
    PSEL_ENTER,     // the shrink-and-retreat animation
    PSEL_BROWSE,    // browsing (the slide-in of a page change counts as this state)
    PSEL_EXIT       // the zoom-back animation
} psel_state_e;

static psel_state_e g_ePsel = PSEL_OFF;

//
// Start of the ENTER / EXIT animation.
//
static uint32_t g_ui32PselTick;

//
// Start of the page-change slide-in and its initial horizontal offset (pixels, signed).
//
// Pixels rather than an index: there is only one thumbnail, only ever one image moving on
// screen, and no such thing as a "continuous index" (that is what the face selector's wheel needs).
//
static uint32_t g_ui32PselSlideTick;
static float    g_fPselSlideFrom;

//*****************************************************************************
//
//! @brief Whether the page overview is open (including the transition animations).
//
//*****************************************************************************
static bool
psel_active(void)
{
    return (g_ePsel != PSEL_OFF);
}

//*****************************************************************************
//
//! @brief Open the page overview.
//
//*****************************************************************************
static void
psel_open(void)
{
    if ( g_ePsel != PSEL_OFF )
    {
        return;
    }

    //
    // No thumbnail buffer means no overview to look at. This is not an error path but a
    // degraded one: with the allocation failed everything else works as usual and only
    // this button does nothing, so it is said out loud rather than failing silently.
    //
    if ( g_sPselThumb.bo.base_virt == NULL )
    {
        am_util_debug_printf("[render] overview unavailable (thumb not allocated)\n");
        return;
    }

    //
    // The overview is a full-screen modal layer, so the other overlays and the face
    // selector are all closed. It does not rely on "they were not open anyway" -- BTN0
    // can be pressed at any time, including halfway through pulling out the brightness panel.
    //
    fsel_close(false);
    g_bMenuOpen     = false;
    g_bPageMenuOpen = false;
    g_bBlOpen       = false;
    g_bBlDrag       = false;

    g_ePsel             = PSEL_ENTER;
    g_ui32PselTick      = xTaskGetTickCount();
    g_fPselSlideFrom    = 0.0f;
    g_ui32PselSlideTick = g_ui32PselTick;

    am_util_debug_printf("[render] overview: opened\n");
}

//*****************************************************************************
//
//! @brief Close the page overview and zoom back to full screen.
//!
//! @details There is no "cancel": page changes made in the overview take effect
//! immediately (see psel_step()), and closing only zooms the screen back in on whichever
//! page you are on. That is deliberate -- a physical button press should have exactly one
//! outcome, and "press BTN0, swipe twice, press BTN0 again and end up back on the original
//! page" would feel like a mistake.
//
//*****************************************************************************
static void
psel_close(void)
{
    if ( (g_ePsel == PSEL_OFF) || (g_ePsel == PSEL_EXIT) )
    {
        return;
    }

    //
    // When cancelled partway through ENTER (BTN0 is a toggle, so a quick double press does
    // this), the EXIT start is moved forward so the zoom continues from the current shrink
    // progress rather than jumping to fully shrunk first.
    //
    // This inverts linearly rather than using the easing's true inverse: smoothstep has
    // zero slope at both ends and an exact inversion would need a cube root. The error only
    // shows as the first few frames of the zoom being slightly fast or slow, invisible on a
    // 240 ms animation and not worth pulling in a root for.
    //
    if ( g_ePsel == PSEL_ENTER )
    {
        float fT = fsel_ease(g_ui32PselTick, PSEL_ANIM_MS);

        g_ui32PselTick = xTaskGetTickCount()
                         - (uint32_t)((1.0f - fT) * (float)PSEL_ANIM_MS);
    }
    else
    {
        g_ui32PselTick = xTaskGetTickCount();
    }

    g_ePsel = PSEL_EXIT;

    am_util_debug_printf("[render] overview: closed\n");
}

//*****************************************************************************
//
//! @brief BTN0 pressed: open when closed, close when open.
//
//*****************************************************************************
static void
psel_toggle(void)
{
    if ( g_ePsel == PSEL_OFF )
    {
        psel_open();
    }
    else
    {
        psel_close();
    }
}

//*****************************************************************************
//
//! @brief Change page inside the overview.
//!
//! @details It calls ui_page_set() and really switches, rather than recording a "target
//! index" and applying it later the way the face selector does. The difference is in the
//! correctness of the preview:
//!
//! The face selector's preview is drawn live from the assets and is exactly right without
//! switching. A page is not -- the thumbnail comes from "drawing that page into the
//! framebuffer", and what the map page draws depends on whether MapTask is active
//! (`MapTaskSetActive()` lives in ui_page_set()). Without really switching, swiping to the
//! map page would show an image frozen at the last time it was left.
//!
//! The cost is that every swipe in the overview runs all of ui_page_set()'s side effects
//! (overlays reset, map drag state reset, one log line). Those are what a page change
//! should do anyway, and doing them repeatedly is idempotent.
//!
//! Cyclic: swiping right from the last page returns to the first. Unlike swipes outside the
//! overview (which stop at the ends), because this is the overview -- all five dots are on
//! screen, wrapping round loses nobody, and being blocked at a boundary while switching
//! quickly is the more annoying option. The face selector's wheel is cyclic too.
//!
//! @param i32Delta +1 for the next page, -1 for the previous.
//!
//! @return None
//
//*****************************************************************************
static void
psel_step(int32_t i32Delta)
{
    int32_t i32Next;

    //
    // Page changes are accepted only while browsing. Changing during a transition would swap
    // the thumbnail's content mid-zoom and look like a flicker.
    //
    if ( g_ePsel != PSEL_BROWSE )
    {
        return;
    }

    i32Next = (int32_t)g_ePage + i32Delta;

    if ( i32Next < 0 )
    {
        i32Next = (int32_t)UI_PAGE_COUNT - 1;
    }
    else if ( i32Next >= (int32_t)UI_PAGE_COUNT )
    {
        i32Next = 0;
    }

    ui_page_set((ui_page_e)i32Next);

    //
    // The new page slides in from the opposite side to the finger: swiping left (to see the next page) brings the new page in from the right.
    //
    g_fPselSlideFrom    = (i32Delta > 0) ? PSEL_SLIDE_DX : -PSEL_SLIDE_DX;
    g_ui32PselSlideTick = xTaskGetTickCount();
}

//*****************************************************************************
//
//! @brief Intercept every gesture while the overview is open.
//!
//! @details Returning true means the overview has handled this gesture and the caller
//! should go no further. Even gestures with no corresponding action are swallowed --
//! without that, an upward swipe in the overview would also pull the brightness panel out
//! over the thumbnail. Same reason and same shape as fsel_input().
//!
//! @param psGesture The gesture classified by TouchTask.
//!
//! @return Always true while the overview is open; false while closed.
//
//*****************************************************************************
static bool
psel_input(const gesture_t *psGesture)
{
    if ( g_ePsel == PSEL_OFF )
    {
        return false;
    }

    //
    // Discrete gestures are not handled while a finger is still down (they are only
    // produced at release), and no input is accepted during EXIT either -- the zoom back is
    // already running, and a page change then would only make the animation look broken.
    //
    if ( psGesture->bPressed || (g_ePsel == PSEL_EXIT) )
    {
        return true;
    }

    switch ( psGesture->eType )
    {
        case GESTURE_SWIPE_LEFT:
            psel_step(1);
            break;

        case GESTURE_SWIPE_RIGHT:
            psel_step(-1);
            break;

        case GESTURE_TAP:
            psel_close();
            break;

        default:
            //
            // Vertical swipes have no meaning in the overview, but still count as handled.
            //
            break;
    }

    return true;
}

//*****************************************************************************
//
//! @brief pass 2: shrink the fully drawn page into the thumbnail buffer.
//!
//! @param fb The framebuffer with a whole page already drawn (the source).
//! @param cl The command list. This function rewinds and submits but does not wait for completion.
//!
//! @return None
//
//*****************************************************************************
static void
psel_capture(nema_img_obj_t *fb, nema_cmdlist_t *cl)
{
    nema_cl_rewind(cl);
    nema_cl_bind_circular(cl);

    nema_bind_dst_tex(g_sPselThumb.bo.base_phys,
                      g_sPselThumb.w,
                      g_sPselThumb.h,
                      g_sPselThumb.format,
                      g_sPselThumb.stride);

    nema_set_clip(0, 0, g_sPselThumb.w, g_sPselThumb.h);

    //
    // Shrinking must use bilinear: 468 -> 264 is not an integer ratio, and point sampling
    // would simply drop the pixels between sample points, breaking up fine text and the
    // hands. This is the other half of the same reason draw_home() picks NEMA_FILTER_BL
    // when scaling the home image up.
    //
    nema_bind_src_tex(fb->bo.base_phys,
                      fb->w,
                      fb->h,
                      fb->format,
                      fb->stride,
                      NEMA_FILTER_BL);

    //
    // NEMA_BL_SRC rather than NEMA_BL_SIMPLE: this overwrites the whole block rather than
    // compositing, the thumbnail's previous frame need not be preserved, and it saves a read.
    //
    nema_set_blend_blit(NEMA_BL_SRC);
    nema_blit_rect_fit(0, 0, g_sPselThumb.w, g_sPselThumb.h);

    nema_cl_submit(cl);
}

//*****************************************************************************
//
//! @brief pass 3: clear the screen, blit the thumbnail back per the animation, then draw the page dots.
//!
//! @details Animation progress fT: ENTER runs 0 to 1 (shrinking), EXIT runs 1 to 0
//! (zooming), BROWSE is always 1. At fT = 0 the blit's position and size are exactly full
//! screen, so there is no jump at the moment of transition. The state machine advances here
//! too (ENTER -> BROWSE, EXIT -> OFF), on the same pattern as fsel_draw(): only the drawing
//! side can see the animation's time base.
//!
//! @param fb The target framebuffer.
//! @param cl The command list. This function rewinds and submits but does not wait for completion.
//!
//! @return None
//
//*****************************************************************************
static void
psel_compose(nema_img_obj_t *fb, nema_cmdlist_t *cl)
{
    float    fT;
    float    fSide;
    float    fCX;
    float    fCY;
    float    fDotX0;
    uint32_t ui32Page;

    if ( g_ePsel == PSEL_ENTER )
    {
        fT = fsel_ease(g_ui32PselTick, PSEL_ANIM_MS);

        if ( fT >= 1.0f )
        {
            g_ePsel = PSEL_BROWSE;
        }
    }
    else if ( g_ePsel == PSEL_EXIT )
    {
        fT = 1.0f - fsel_ease(g_ui32PselTick, PSEL_ANIM_MS);

        if ( fT <= 0.0f )
        {
            g_ePsel = PSEL_OFF;
        }
    }
    else
    {
        fT = 1.0f;
    }

    //
    // The edge interpolates from full screen to the shrunk size; the centre from the screen centre to PSEL_PREVIEW_CY.
    //
    fSide = (float)fb->w * (1.0f + ((PSEL_PREVIEW_SCALE - 1.0f) * fT));
    fCY   = (float)(fb->h / 2)
            + (((float)PSEL_PREVIEW_CY - (float)(fb->h / 2)) * fT);

    //
    // The slide-in offset of a page change. Once it finishes (or if it was never running), fsel_ease() returns 1 and the offset is zero.
    //
    fCX = (float)(fb->w / 2)
          + (g_fPselSlideFrom
             * (1.0f - fsel_ease(g_ui32PselSlideTick, PSEL_SLIDE_MS)));

    nema_cl_rewind(cl);
    nema_cl_bind_circular(cl);

    nema_bind_dst_tex(fb->bo.base_phys,
                      fb->w,
                      fb->h,
                      fb->format,
                      fb->stride);

    nema_set_clip(0, 0, fb->w, fb->h);

    nema_clear(0xff000000);

    nema_bind_src_tex(g_sPselThumb.bo.base_phys,
                      g_sPselThumb.w,
                      g_sPselThumb.h,
                      g_sPselThumb.format,
                      g_sPselThumb.stride,
                      NEMA_FILTER_BL);

    nema_set_blend_blit(NEMA_BL_SRC);
    nema_blit_rect_fit((int)(fCX - (fSide * 0.5f)),
                       (int)(fCY - (fSide * 0.5f)),
                       (int)fSide,
                       (int)fSide);

    //
    // The page dots. The alpha fades in with fT, for the same reason as the face selector's
    // alignment frame: drawing it fully partway through the entry would catch the eye before the thumbnail.
    //
    // Drawn only for the UI_PAGE_COUNT pages -- the generated page sits after COUNT and is not in the carousel.
    //
    fDotX0 = (float)(fb->w / 2)
             - (((float)UI_PAGE_COUNT - 1.0f) * 0.5f * (float)PSEL_DOT_PITCH);

    nema_set_blend_fill(NEMA_BL_SIMPLE);

    for ( ui32Page = 0; ui32Page < (uint32_t)UI_PAGE_COUNT; ui32Page++ )
    {
        bool     bOn    = (ui32Page == (uint32_t)g_ePage);
        uint32_t ui32A  = (uint32_t)(fT * (bOn ? 235.0f : 110.0f));

        nema_fill_circle_aa(fDotX0 + ((float)ui32Page * (float)PSEL_DOT_PITCH),
                            (float)PSEL_DOT_CY,
                            bOn ? PSEL_DOT_R_ON : PSEL_DOT_R,
                            (ui32A << 24) | 0x00ffffffU);
    }

    nema_cl_submit(cl);
}

#endif // WATCHFACE_ENABLE_TOUCH

//*****************************************************************************
//
//! @brief Update the UI state machine from a gesture.
//!
//! @details Four modes; the open state of the three overlays decides what a gesture means:
//!
//!   Brightness panel open (possible on every page)
//!       hold the slider -> adjust the brightness continuously (updated every frame during the hold, not at release)
//!       swipe down      -> close the panel
//!       tap outside     -> close the panel
//!       anything else   -> ignored
//!
//!   Control centre open (home page only)
//!       tap a cell      -> perform that cell's action
//!       tap outside     -> close the menu
//!       swipe up        -> close the menu
//!       anything else   -> ignored. Horizontal swipes deliberately do not change pages:
//!                          with the menu covering the screen the change would be invisible,
//!                          and one way out of the menu is easier to understand.
//!
//!   Quick page menu open (home page only)
//!       tap an entry    -> go to UI_PAGE_TREND or UI_PAGE_MAP
//!       tap outside     -> close the menu
//!       swipe left      -> close the menu
//!       anything else   -> ignored
//!
//!   All three closed
//!       swipe left      -> next page
//!       swipe right     -> open the quick page menu on the home page; go back a page elsewhere
//!       swipe down      -> open the control centre, only on the home page and only when the swipe started at the top edge
//!       swipe up        -> open the brightness panel when the swipe started at the bottom edge (on every page)
//!
//! The brightness panel is tested first, because it is the only overlay that has to react
//! during a hold; the control centre and page changes only act at release.
//!
//! The upward swipe is used by two things at once: with the control centre open it means
//! "close the menu", and with everything closed it means "open the brightness panel". They
//! cannot conflict -- the former only holds on the home page with the menu open, and that
//! state never reaches the latter's branch. The cost is that the brightness panel cannot be
//! summoned directly while the home menu is open and the menu has to be closed first; that
//! is easier to understand than one gesture doing two things.
//!
//! @param psGesture The gesture reported by TouchTaskGetGesture()
//
//*****************************************************************************
static void
ui_input_update(const gesture_t *psGesture)
{
    //
    // The face selector comes before everyone: it is a full-screen modal layer, and while
    // it is open not even the brightness panel should be reachable. While closed this
    // function does nothing and returns false immediately.
    //
    //
    // The page overview comes first of all: it is further out than the face selector --
    // with the overview open the face selector has already been closed by psel_open(), so
    // the two can never hold at once. While closed it returns false immediately.
    //
    if ( psel_input(psGesture) )
    {
        return;
    }

    if ( fsel_input(psGesture) )
    {
        return;
    }

    if ( g_bBlOpen )
    {
        //
        // During a hold: as long as this press started on the slider, the brightness keeps following the finger.
        //
        // The test uses g_bBlDrag || so that "already dragging" sticks: sliding up along the
        // slider past the hit area still counts as a drag (the brightness is clamped to the
        // maximum by bl_set_level_from_y()) rather than turning into a SWIPE_UP partway.
        //
        if ( psGesture->bPressed )
        {
            if ( g_bPageMenuOpen ||
                 bl_point_in_slider(psGesture->ui16CurX, psGesture->ui16CurY) )
            {
                g_bBlDrag = true;
                bl_set_level_from_y(psGesture->ui16CurY);
            }

            return;
        }

        //
        // The finger has lifted. If this frame has just ended a drag, the gesture is
        // swallowed: a drag's displacement is usually well past SWIPE_MIN_DY, and without
        // blocking it one downward drag of the brightness would close the panel as well.
        //
        if ( g_bBlDrag )
        {
            g_bBlDrag = false;
            am_util_debug_printf("[render] bl: level -> 0x%02x\n",
                                 backlight_get_level());
            return;
        }

        switch ( psGesture->eType )
        {
            case GESTURE_SWIPE_DOWN:
                g_bBlOpen = false;
                am_util_debug_printf("[render] bl: closed\n");
                break;

            case GESTURE_TAP:
                //
                // A tap outside the panel means close, as with the control centre. A tap
                // inside the panel but not on the slider (the margins above and below) does
                // nothing -- a near miss should not close the panel.
                //
                if ( !bl_point_in_panel(psGesture->ui16X, psGesture->ui16Y) )
                {
                    g_bBlOpen = false;
                    am_util_debug_printf("[render] bl: closed (tap outside)\n");
                }
                break;

            default:
                break;
        }

        return;
    }

    if ( g_bMenuOpen )
    {
        switch ( psGesture->eType )
        {
            case GESTURE_TAP:
                if ( cc_point_in_panel(psGesture->ui16X, psGesture->ui16Y) )
                {
                    cc_action_run(cc_hit_test(psGesture->ui16X,
                                              psGesture->ui16Y));
                }
                else
                {
                    //
                    // A tap outside the panel: as on a phone, this means close the menu.
                    // It cannot wrongly catch the gaps between cells, since those points
                    // are still inside the panel and are left to
                    // cc_action_run(), which treats them as no action.
                    //
                    g_bMenuOpen = false;
                    am_util_debug_printf("[render] cc: closed (tap outside)\n");
                }
                break;

            case GESTURE_SWIPE_UP:
                g_bMenuOpen = false;
                am_util_debug_printf("[render] cc: closed\n");
                break;

            default:
                break;
        }

        return;
    }

    if ( g_bPageMenuOpen )
    {
        switch ( psGesture->eType )
        {
            case GESTURE_TAP:
                if ( page_menu_point_in_panel(psGesture->ui16X,
                                              psGesture->ui16Y) )
                {
                    page_menu_action_run(
                        page_menu_hit_test(psGesture->ui16X,
                                           psGesture->ui16Y));
                }
                else
                {
                    g_bPageMenuOpen = false;
                    am_util_debug_printf("[render] page menu: closed (tap outside)\n");
                }
                break;

            case GESTURE_SWIPE_LEFT:
                g_bPageMenuOpen = false;
                am_util_debug_printf("[render] page menu: closed\n");
                break;

            default:
                break;
        }

        return;
    }

    //
    // The map page's gesture semantics differ from the others (a horizontal swipe pans
    // the map rather than changing pages), so it picks out what it wants before the shared handling.
    //
    // It comes after the overlay branches: while any overlay is open the gesture belongs to that overlay and never reaches here.
    //
    if ( g_ePage == UI_PAGE_MAP )
    {
        if ( map_input_update(psGesture) )
        {
            return;
        }
    }

    switch ( psGesture->eType )
    {
        case GESTURE_TAP:
            //
            // Only the generated page needs taps: with the overlays closed, the other three pages have nothing to tap.
            //
            // The release position (ui16X/ui16Y) is passed, consistent with the control
            // centre's hit test, and needs no conversion: the Designer's canvas is 454x454
            // (the circle the watch actually shows), smaller than the FB, but code
            // generation already centres the whole canvas on the framebuffer (see
            // UI_DESIGNER_ORIGIN_X/Y = 7 in the generated file), so both drawing and hit
            // testing produce framebuffer coordinates. To change the canvas size, change
            // the Designer project settings; do not add an offset here -- that would
            // apply it twice.
            //
            // There is no need to check whether the generated side initialized
            // successfully -- uninitialized, it always returns false and triggers no callback.
            //
            // The generated page has left the carousel (see ui_page_e) and this branch is currently unreachable.
            //
            if ( g_ePage == UI_PAGE_DESIGN )
            {
                (void)ui_designer_touch(UI_SCENE_DEFAULT,
                                        (int32_t)psGesture->ui16X,
                                        (int32_t)psGesture->ui16Y);
            }
            break;

        case GESTURE_SWIPE_LEFT:
            if ( g_ePage < (UI_PAGE_COUNT - 1) )
            {
                ui_page_set((ui_page_e)(g_ePage + 1));
            }
            break;

        case GESTURE_SWIPE_RIGHT:
            if ( g_ePage == UI_PAGE_HOME )
            {
                g_bPageMenuOpen = true;
                g_bMenuOpen     = false;
                g_bBlOpen       = false;
                g_bBlDrag       = false;
                am_util_debug_printf("[render] page menu: opened\n");
            }
            else
            {
                ui_page_set((ui_page_e)(g_ePage - 1));
            }
            break;

        case GESTURE_SWIPE_DOWN:
            //
            // Swipe down on the face page: open the watch face selector.
            //
            // This gesture had no behaviour at all on the face page, so it needs no
            // coordination with anyone. Unlike the control centre, it does not require the
            // swipe to start at the top edge -- the control centre is "a drawer pulled down
            // from above", whereas a downward swipe here means "push the face aside and see
            // what else there is", and it makes no difference where the push starts.
            //
            if ( g_ePage == UI_PAGE_WATCH )
            {
                fsel_open();
            }
            else if ( (g_ePage == UI_PAGE_HOME) &&
                      (psGesture->ui16MinY <= CC_PULL_START_MAX_Y) )
            {
                g_bMenuOpen     = true;
                g_bPageMenuOpen = false;
                g_bBlOpen       = false;
                g_bBlDrag       = false;
                am_util_debug_printf("[render] cc: opened\n");
            }
            break;

        case GESTURE_SWIPE_UP:
            //
            // Drag up from the bottom edge to open the brightness panel. On every page --
            // brightness is a global setting and should be reachable everywhere.
            //
            //
            // The test uses the trace's lowest point (ui16MaxY) rather than the start: it is
            // enough that this swipe reached the bottom band at some point. It used to use
            // ui16StartY, and that start was the first point sampled rather than where the
            // finger really went down, so a fast swipe drifted out of the edge band and
            // failed (see the header of touch_task.h).
            //
            if ( psGesture->ui16MaxY >= BL_PULL_START_MIN_Y )
            {
                g_bBlOpen = true;

                //
                // The three overlays are exclusive. The home page's two menus are closed
                // explicitly here, rather than relying on the order of the branches above
                // to keep the state right.
                //
                g_bMenuOpen     = false;
                g_bPageMenuOpen = false;
                g_bBlDrag       = false;

                am_util_debug_printf("[render] bl: opened (level 0x%02x)\n",
                                     backlight_get_level());
            }
            break;

        default:
            break;
    }
}
#endif // WATCHFACE_ENABLE_TOUCH

//*****************************************************************************
//
// Render task
//
//*****************************************************************************
void
RenderTask(void *pvParameters)
{
    //
    // Wake the GPU for initialization.
    //
    nemagfx_power_control(AM_HAL_SYSCTRL_WAKE, false);

    //
    // Initialize the NemaSDK
    //
    nema_init();
    nema_vg_init(FB_RESX, FB_RESY);

    //
    // The Bluetooth icon's vector path, created once and reused.
    //
    bt_icon_init();

    //
    // The control centre's icon paths, likewise created once and reused (the brightness panel's sun icon is in there too).
    //
    cc_icons_init();

    //
    // The map page's vehicle and turn icons.
    //
    map_icons_init();

    //
    // The brightness state. Only the initial value this module tracks is set and no command
    // is sent -- DC/DSI is still asleep at this point, and panel init has already set the
    // brightness to 0xff (see backlight.h).
    //
    backlight_init();

    //
    // The UI Designer generated page (page 1, shown by default at boot).
    //
    // Must come after nema_init() and nema_vg_init(): it creates VG paints and copies image
    // assets into SSRAM with nema_buffer_create_pool(NEMA_MEM_POOL_ASSETS, ...) -- the same
    // pool render_assets_load() uses. Its assets currently come to 52 KB.
    //
    // A failure does not abort RenderTask and needs no flag: the other three pages have
    // nothing to do with the generated one and should not freeze the whole screen because it
    // could not get memory; and uninitialized, the generated side fills the canvas with the
    // background colour and always returns false for touch, so page 1 is merely a flat
    // colour rather than a leftover image or a source of stray taps. This log line is the
    // only clue -- do not remove it.
    //
    if ( !ui_designer_init() )
    {
        am_util_debug_printf("[render] ui_designer_init failed\n");
    }

    //
    // Initialization complete; put the GPU into its low-power state.
    //
    nemagfx_power_control(AM_HAL_SYSCTRL_DEEPSLEEP, true);

#if WATCHFACE_ENABLE_TOUCH
    //
    // Touch initialization has moved to TouchTask (see touch_task.h): once the GP98
    // interrupt is enabled it can arrive immediately, and the callback needs a task handle
    // that is already valid, so that task has to do it itself. Nothing is needed here any more.
    //
#endif

    //
    // The TMP119 is initialized and sampled by SensorTask (see sensor_task.c); the render
    // side only reads the cache and never touches IOM1.
    //

    //
    // Prepare the command list.
    //
    // Raised from 4 KB to 32 KB for the temperature page: everything there goes through
    // NemaVG, and the gauge's 20 arc segments, the 12 ticks and every glyph of the vector
    // font each expand into their own run of GPU commands, well past 4 KB in one frame.
    //
    // A circular CL never fills up or overflows -- when full it implicitly submits and
    // overwrites from the start (see nema_cl_bind_circular()), so 4 KB would still "work",
    // only forcing the frame to stop and wait for the GPU to catch up several times, eating
    // frame time for nothing. 32 KB holds a whole frame, at the cost of 28 KB more from the
    // graphics pool (VMEM totals 2 MB).
    //
    nema_cmdlist_t cl = nema_cl_create_sized(32 * 1024);

    //
    // Allocate the two blocks the UI cannot do without first, then load the optional assets.
    //
    // The order is itself part of the design, and was the root cause of two failures on
    // hardware. render_assets_load() used to come first, so a purely additive change like
    // "one more watch face" could eat the pool down past the necessities, with symptoms
    // that looked nothing like the cause:
    //
    //   first   assets exhaust the pool -> framebuffer allocation fails -> the GPU is bound
    //           to address 0 -> fault -> the screen and BLE advertising stop together, looking like a dead board
    //   second  assets (with the larger Ambiq dial) take the extra 122 KB -> the overview
    //           thumbnail allocation fails -> BTN0 silently stops working while everything else is fine
    //
    // What the two share is "the optional comes before the necessary". Reversed, the
    // consequence of a short pool lands where it belongs: some face's assets do not load and
    // that face is black (face_render() skips it anyway), while the framebuffer and the
    // overview thumbnail already have theirs.
    //
    // This does not add memory; it puts the failure in the right place.
    //
    //
    // A failed framebuffer allocation means going no further.
    //
    // This used to print one line and continue, while nothing in the drawing checked
    // base_virt -- nema_bind_dst_tex() would be bound to address 0, the GPU would fault on
    // the first write, and even BLE advertising would stop. A black screen with no blinking
    // LED looks like a dead board, while the real cause (a short pool) sat in that one log line.
    //
    // Now the task exits instead: there is no UI at all, but BLE, sensing and buttons carry
    // on as usual, and the log says exactly why. This matches what TouchTask does when
    // touch_init() fails -- "letting it pretend to work" is far harder to diagnose than exiting.
    //
    // Why not check every frame: the framebuffer is allocated once at boot and does not
    // disappear afterwards. Checking an unchanging condition every frame only spreads the
    // cost over runtime and buys no additional safety.
    //
    if ( framebuffer_create(&g_sFrameBuffer) != 0 )
    {
        am_util_debug_printf("[render] framebuffer alloc FAILED (%u B) -- "
                             "graphics pool exhausted, UI disabled\n",
                             (unsigned)nema_texture_size(g_sFrameBuffer.format,
                                                         0,
                                                         g_sFrameBuffer.w,
                                                         g_sFrameBuffer.h));
        am_util_debug_printf("[render] raise VMEM_SIZE in gcc/Makefile or "
                             "drop a texture; task exiting\n");

        vTaskDelete(NULL);
    }

#if WATCHFACE_ENABLE_TOUCH
    //
    // The page overview's thumbnail buffer. From the same pool as the framebuffer (NEMA_MEM_POOL_FB).
    //
    // A failed allocation is not fatal: psel_open() checks base_virt and refuses to open,
    // BTN0 does nothing, and everything else works as usual. The size is printed alongside,
    // because what has to be judged on a failure is "how much pool is left" rather than
    // "where the program went wrong".
    //
    if ( framebuffer_create(&g_sPselThumb) != 0 )
    {
        am_util_debug_printf("[render] overview thumb alloc failed (%ux%u), BTN0 disabled\n",
                             (unsigned)g_sPselThumb.w,
                             (unsigned)g_sPselThumb.h);
    }
#endif

    //
    // Load the textures
    //
    render_assets_load();

    //
    // The asset load results are not printed here: the PSRAM timing scan emits hundreds of
    // lines and floods the RTT up buffer (NO_BLOCK_SKIP, full means dropped), so anything
    // behind it never gets out -- the boot log was measured breaking off in the middle of
    // the PSRAM section.
    //
    // To see what each face actually uses, switch faces once: faces_dump() hangs off the
    // moment of applying, by which time RTT is idle.
    //


#if WATCHFACE_SHOW_WEATHER_ICON
    //
    // Create the timer that rotates the weather icon
    //
    // Both return values have to be checked, because in this project the timer command queue
    // is a "shared and crowded" resource:
    //
    //   - configTIMER_QUEUE_LENGTH is only 5 (the SDK example's value).
    //   - Cordio's WSF porting layer posts two xTimerChangePeriod() calls per
    //     wsfOsDispatcher() invocation (one on entry and one on exit, see wsf_os.c and
    //     WsfTimerUpdateTicks() in wsf_timer.c), and RadioTask runs at the highest priority, 15.
    //   - The timer service task runs at priority 3, below every task in this project.
    //
    // By the time this function reaches here, render_assets_load() and framebuffer_create()
    // have just finished, during which RenderTask (priority 14) held the CPU and was only
    // preempted by RadioTask; the timer service task never got scheduled and nobody drained
    // the commands RadioTask pushed into the queue. So the original xTimerStart(icon_timer, 0)
    // (no waiting, no return check) easily met a full queue and returned pdFAIL -- the timer
    // was created but never started, the weather icon stayed on blowing forever, and there
    // was no sign of an error anywhere.
    //
    // It is now willing to wait (the second argument to xTimerStart) and says so on failure.
    //
    TimerHandle_t icon_timer = xTimerCreate("IconTimer",
                                            pdMS_TO_TICKS(1000),
                                            pdTRUE,
                                            NULL,
                                            icon_change);
    if ( icon_timer == NULL )
    {
        am_util_debug_printf("[render] icon timer create failed (heap %u B free)\n",
                             (unsigned)xPortGetFreeHeapSize());
    }
    else if ( xTimerStart(icon_timer, pdMS_TO_TICKS(200)) != pdPASS )
    {
        am_util_debug_printf("[render] icon timer start failed (timer queue full)\n");
    }
#endif // WATCHFACE_SHOW_WEATHER_ICON

    //
    // Measure the frame rate and report it over the debug channel (RTT or ITM/SWO).
    //
    uint32_t ui32FrameCount  = 0;
    uint32_t ui32LastReport  = xTaskGetTickCount();

    am_util_debug_printf("[render] task started, %ux%u\n", FB_RESX, FB_RESY);

    while (1)
    {
        xSemaphoreTake(g_semDisplayEnd, portMAX_DELAY);

        //
        // Check once per frame (about 18 ms) whether the EM9305 INT is stuck high.
        //
        // GPIO117 accepts rising edges only, and once that transition is missed the hardware
        // gives no second chance; recovery used to take the 10 s hciCmdTimeout() (the source
        // of the 10 / 20 second boot delay). The render loop is borrowed here as a
        // fixed-period check point. See WATCHFACE_BLE_INT_KICK in ble_diag.h.
        //
        ble_diag_int_kick();

#if WATCHFACE_ENABLE_TOUCH
        //
        // Take the gesture TouchTask classified and update the page state machine.
        //
        // This no longer touches I2C and never blocks -- sampling and classification are both
        // done by TouchTask on the GP98 interrupt (see touch_task.h). Before this revision a
        // blocking I2C read happened right here, so the touch sampling rate equalled the
        // frame rate and short edge gestures were often missed entirely.
        //
        // It still comes before waking the GPU: the page state and the overlays' open or
        // closed state have to be settled before this frame starts drawing, or a gesture's
        // effect would not be visible until the next frame.
        //
        {
            gesture_t sGesture;

            TouchTaskGetGesture(&sGesture);
            ui_input_update(&sGesture);
        }

        //
        // Button1 (SW3) requests a return to the home page.
        //
        // The button's interrupt context only raises a flag (see RenderTaskGoHome()); the
        // actual page change happens here: ui_page_set() touches the overlays, the map task's
        // active state and a pile of gesture variables, and the other writer of all those is
        // this task itself -- changing them in an ISR would mean both sides writing them with
        // no synchronization at all.
        //
        // It comes after the gesture handling: when a gesture in the same frame also wants to
        // change pages, the button wins -- the button is the user explicitly saying "I want
        // out", while a map page gesture is usually a side effect of dragging to the boundary.
        //
        if ( g_bGoHomeReq )
        {
            g_bGoHomeReq = false;

            //
            // With the overview open, BTN1 means "go straight to the home page", so the
            // overview is closed after the page change: otherwise it would stop on a shrunken
            // home page and the user would have to press again to get out.
            //
            if ( psel_active() )
            {
                am_util_debug_printf("[render] BTN1 -> home (overview closed)\n");
                ui_page_set(UI_PAGE_HOME);
                psel_close();
            }
            else if ( g_ePage != UI_PAGE_HOME )
            {
                am_util_debug_printf("[render] BTN1 -> home\n");
                ui_page_set(UI_PAGE_HOME);
            }
        }

        //
        // Button0 (SW1) toggles the page overview.
        //
        // It comes after BTN1: with both flags set in one frame, BTN1 has already closed the
        // overview and returned home, and toggling here would open it again. That is a
        // reasonable result for one press of each button, but the other order (toggle first,
        // then closed by BTN1) would make the BTN0 press look unresponsive.
        //
        if ( g_bPageOverviewReq )
        {
            g_bPageOverviewReq = false;
            psel_toggle();
        }
#endif

        //
        // The temperature trend sampling. It runs every frame on every page and decides for
        // itself whether a second has passed -- the curve is about the last minute, and
        // collecting only on that page would make it grow from nothing on every visit.
        //
        // It comes before waking the GPU: it only reads SensorTask's cache and has nothing to do with drawing.
        //
        trend_sample_update();

        //
        // Wake the GPU for drawing.
        //
        nemagfx_power_control(AM_HAL_SYSCTRL_WAKE, true);

        //
        // This frame's time: seconds since boot plus the start offset. The face page and the
        // trend page share this one value, so switching between them never disagrees.
        //
        {
            float fSec = (float)xTaskGetTickCount() / configTICK_RATE_HZ
                         + WATCH_START_SEC;

            //
            // pass 1: draw the current page into the framebuffer as usual (full size).
            //
            draw_page(g_ePage, &g_sFrameBuffer, &cl, fSec);

            //
            // Wait for the drawing to complete
            //
            nema_cl_wait(&cl);

#if WATCHFACE_ENABLE_TOUCH
            //
            // With the page overview open, two more passes follow: capture the page just drawn
            // into a thumbnail, then clear the screen and blit it back per the animation. Why
            // all three passes wait for completion separately is explained in the "page
            // overview" section.
            //
            if ( psel_active() )
            {
                psel_capture(&g_sFrameBuffer, &cl);
                nema_cl_wait(&cl);

                psel_compose(&g_sFrameBuffer, &cl);
                nema_cl_wait(&cl);
            }
#endif
        }

        //
        // Drawing finished; power the GPU down.
        //
        nemagfx_power_control(AM_HAL_SYSCTRL_DEEPSLEEP, true);

        //
        // Release the semaphore
        //
        xSemaphoreGive(g_semDisplayStart);

        //
        // Report the measured frame rate once a second. In NO_BLOCK_TRIM mode RTT is only a
        // memcpy and never blocks the drawing, even with no host attached.
        //
        ui32FrameCount++;
        uint32_t ui32Now = xTaskGetTickCount();
        if ((ui32Now - ui32LastReport) >= configTICK_RATE_HZ)
        {
            am_util_debug_printf("[render] %u fps\n", ui32FrameCount);
            ui32FrameCount = 0;
            ui32LastReport = ui32Now;
        }
    }
}

//*****************************************************************************
//
//! @brief The `test` button of the UI Designer generated page has been pressed.
//!
//! @details Overrides the weak definition in ui_designer_generated.c. The button's own
//! visual feedback (the central image rotating 90 degrees) is already handled by the
//! generated code; only this project's own response goes here.
//!
//! Called synchronously by RenderTask inside ui_input_update(), so this must not block --
//! anything time-consuming should be handed to the matching task, as the control centre
//! cells do with LedTaskToggle().
//!
//! The symbol name comes from that button's "click event" field in the Designer (currently
//! button_clicked), not from the button's name. After changing that field this function is
//! no longer an override but a function nobody calls -- the link does not complain and the
//! button simply does nothing. Generate C lists added and removed symbols in its warnings,
//! and seeing one means changing this too.
//!
//! @return None
//
//*****************************************************************************
void
ui_event_button_clicked(void)
{
    am_util_debug_printf("[render] design: button clicked\n");
}
