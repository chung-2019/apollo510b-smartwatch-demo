//*****************************************************************************
//
//! @file touch_task.h
//!
//! @brief Touch task: interrupt-driven CHSC5816 sampling and gesture classification.
//!
//! This module lifts touch out of the render loop. Before this revision
//! touch_poll() was called once per frame by RenderTask, and render and display
//! form a fully serialized pipeline:
//!
//!   draw_*() -> nema_cl_wait() -> g_semDisplayStart -> DSI transfer
//!            -> wait_transfer_done() -> g_semDisplayEnd -> only then touch_poll()
//!
//! So "touch sampling rate = frame rate", with the sampling instant pinned to the
//! same phase of every frame. Three problems follow, all of which users actually
//! hit (and all most visible on the short "pull the brightness panel up from the
//! bottom edge" gesture):
//!
//!   1. The start of a gesture is not where the finger really went down but the
//!      first point sampled. A finger that goes down between two samples has
//!      already moved by then, so the start drifts out of the edge band. At a
//!      30 ms frame period and 1500 px/s of upward travel the start is off by
//!      45 px -- BL_PULL_START_MIN_Y is 318, so a press at y=350 is recorded as
//!      305 and fails outright.
//!   2. The end point likewise lags the real lift by up to a frame, so the
//!      displacement is systematically understated by 30 to 50 px: SWIPE_MIN_DY
//!      is nominally 70, but in practice 100 px or more was needed to be reliable.
//!   3. If the whole gesture falls inside one sampling interval (a quick flick),
//!      pressed is never observed as true, the release branch is never entered
//!      and the gesture vanishes entirely.
//!
//! On top of that, RadioTask (highest priority) stretches individual frames every
//! time wsfOsDispatcher() runs, so the sampling interval itself jitters and the
//! same gesture works one moment and not the next.
//!
//! After this revision, sampling is driven by the GP98 touch interrupt and is
//! independent of the frame rate:
//!   GP98 falling edge
//!         |
//!   am_gpio0_607f_isr()     the BSP's existing one (see "ISR ownership" below)
//!         |                 am_hal_gpio_interrupt_service() dispatches by pin
//!   touch_int_callback()    ISR context, nothing but vTaskNotifyGiveFromISR()
//!         |
//!   TouchTask               task context, I2C read + gesture classification
//!         |
//!   queue / shared live state   consumed by RenderTask once per frame
//!
//! The same shape as Button_callback() in led_task.c: the ISR stays extremely
//! short and does nothing that might block in interrupt context. The SDK's
//! nemagfx_earth_nasa/event_task.c starts the I2C transfer inside the ISR; that
//! approach is not followed here.
//!
//! @section why_not_pure_int Why not purely interrupt-driven, and why two timeouts remain
//!
//! GP98 is configured as edge-triggered (eIntDir of g_AM_BSP_GPIO_TOUCH_INT is
//! INTDIR_HI2LO) with no pull-up (ePullup is PULLUP_NONE). Once an edge is
//! missed, the hardware does not re-issue it.
//!
//! There is an exact precedent in this project: the EM9305's GPIO117 accepts
//! rising edges only, and missing that transition means waiting out the 10 s
//! hciCmdTimeout() before recovery -- which is why ble_diag.c needs the
//! WATCHFACE_BLE_INT_KICK per-frame remedy.
//!
//! A missed edge is worse for touch: the driver sets touch_released to true only
//! on the packet where event == 4 (TP_UP) (see chsc5816_pointinfo_parse() in
//! am_devices_chsc5816_ap5.c). If that packet's interrupt happens to arrive while
//! we are reading I2C and is missed, the state machine stays in "pressed"
//! forever, touch stops working entirely, and no timeout rescues it.
//!
//! The wait timeout therefore follows the state:
//!
//!   released (idle)    TOUCH_IDLE_POLL_MS  almost entirely interrupt-driven, a final safety net only
//!   pressed (held)     TOUCH_HOLD_POLL_MS  handle an interrupt at once, back it up if none arrives
//!
//! @section int_waveform The real INT waveform (measured, not assumed)
//!
//! Measured on this board with TOUCH_INT_RATE_DEBUG, one window of an unbroken hold gives:
//!
//!   1009 ms: edges=59 polls=118 timeouts=59 burst=1 lowIn=59 lowOut=59
//!
//! The complete behaviour follows from this, and all three points differ from the original assumptions:
//!
//!   1. The report rate is 59 Hz (17 ms), not the 10 ms an earlier comment claimed.
//!   2. The IC **does** keep issuing interrupts while a finger rests without
//!      moving, one falling edge per report. The earlier claim that "a stationary
//!      hold may not keep interrupting" is wrong, so that is not what the backup is for.
//!   3. INT is a **self-clearing low pulse**, not "held low until the host reads"
//!      -- lowOut=59 means the line is still low after the I2C read (reading does
//!      not clear it), yet the next edge still arrives, so the IC pulls it back
//!      high itself. The pulse width lies between one I2C read (about 0.4 ms,
//!      lowOut still low) and 15 ms (lowIn was already high on the timeout case).
//!
//! Point 3 is the real reason for the backup, and it enlarges the risk rather than
//! shrinking it: with "held low until read", a missed edge would leave the line low
//! and the next read would necessarily catch up; with a self-clearing pulse a miss
//! is really gone, and only the timeouts below can recover. The lift event is the
//! most dangerous of all -- a release produces exactly one edge (measured edges=1)
//! with no second chance.
//!
//! polls=118 is twice edges, a direct consequence of TOUCH_HOLD_POLL_MS (15 ms)
//! being shorter than the report period (17 ms): every report period first wakes
//! on the timeout for an empty read, then wakes again on the real edge for a
//! second read. See TOUCH_HOLD_POLL_MS.
//!
//! Why portMAX_DELAY cannot be used while idle -- with INCLUDE_vTaskSuspend set to
//! 1 in this project, that is a genuinely infinite wait: miss one edge and the task
//! never wakes again, with no timeout to rescue it. Worst of all, this can happen
//! at boot: am_devices_chsc5816_init() goes interrupt_clear -> register -> control
//! enable, so if the touch IC pulls INT low after reset because it has data
//! pending, that falling edge is cleared before the registration and the line stays
//! low -- no further falling edge ever follows and touch is dead from boot.
//!
//! A 500 ms final safety net leaves no state permanently stuck: the worst case of a
//! missed edge is half a second of latency rather than touch failing entirely. The
//! cost is one ~400 us I2C read every 500 ms, about 0.08% CPU.
//!
//! @section isr_owner ISR ownership
//!
//! This module deliberately does NOT define am_gpio0_607f_isr(). GP98 -> 98/32 = 3
//! -> GPIO0_607F_IRQn, a line shared with the EM9305 INT (GPIO117 -> 117/32 = 3),
//! and the BSP's AM_BSP_EM9305_RADIO_INT_ISR is already the same symbol (see the
//! note in radio_task.c). Defining another would collide at link time. Reusing it
//! is enough -- that one does exactly irq_status_get -> irq_clear ->
//! interrupt_service, and am_hal_gpio_interrupt_service() dispatches by the handler
//! registered for each pin.
//!
//! @section i2c_ownership I2C ownership
//!
//! Touch owns IOM2 (AM_BSP_TP_IOM_MODULE) exclusively and the TMP119 is on IOM1
//! (see tmp119.c), so moving touch into its own task needs no mutex at all. The
//! reason touch.c used to disable the GP98 interrupt was "an I2C read from the ISR
//! might collide with render's polled read"; with only TouchTask touching IOM2,
//! that reason no longer exists.
//
//*****************************************************************************

#ifndef TOUCH_TASK_H
#define TOUCH_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"
#include "nemagfx_watchface.h"

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
// WATCHFACE_ENABLE_TOUCH
//
// Set to 0 to leave touch out; the UI then stays on the home page and never changes page.
// For early bring-up on boards where the touch panel is not yet soldered.
//
// Defined here rather than in render_task.c: with touch split into its own task,
// rtos.c also has to see this switch to decide whether to create TouchTask. Kept
// inside render_task.c it would be invisible to rtos.c, and the task would still
// be created with the switch set to 0.
//
//*****************************************************************************
#define WATCHFACE_ENABLE_TOUCH  1

//*****************************************************************************
//
// Touch task handle.
//
//*****************************************************************************
extern TaskHandle_t TouchTaskHandle;

//*****************************************************************************
//
//! @brief Maximum interval between two samples while held (ms).
//!
//! @details The backup period for a missed interrupt edge. See why_not_pure_int at the top of this file.
//!
//! The original 15 ms came from "clearly shorter than a frame (about 25 to 35 ms
//! on the home page), without getting close to the touch IC's report period
//! (believed to be 10 ms at the time)". Measurement showed that premise was wrong:
//! the report period is 17 ms (59 Hz, see int_waveform at the top), and 15 ms is
//! shorter still, so every report period first wakes on the timeout for an empty
//! read and then on the real edge for a second one -- twice the necessary number
//! of I2C reads while held (measured polls=118 against edges=59), with those extra
//! 59 reads of about 400 us each returning nothing new.
//!
//! Coordinate updates do not depend on this period: the IC still interrupts every
//! 17 ms while a finger rests, so the update rate while dragging the brightness
//! slider follows the report rate and not this constant.
//!
//! The value should therefore be slightly **longer** than the report period rather
//! than shorter: purely a missed-edge insurance that never expires in normal
//! operation. 25 ms allows 8 ms of margin (enough to absorb report-rate jitter), at
//! the cost of the worst case after a missed edge going from 15 ms to 25 ms --
//! well under one frame and imperceptible.
//
//*****************************************************************************
#define TOUCH_HOLD_POLL_MS      (15)

//*****************************************************************************
//
//! @brief Maximum interval between two samples while idle (ms).
//!
//! @details The final insurance against a missed interrupt edge; see
//! why_not_pure_int at the top of this file. It is not a polling period -- in
//! normal operation a finger touching the screen wakes the task by interrupt and
//! this timeout never expires.
//! How 500 ms was chosen: short enough that the user does not conclude "touch is
//! broken" (a second tap within half a second is certain to be seen), long enough
//! that the idle power cost is negligible.
//
//*****************************************************************************
#define TOUCH_IDLE_POLL_MS      (500)

//*****************************************************************************
//
//! @brief Touch interrupt rate measurement (diagnostic; set back to 0 when done).
//!
//! @details With this set to 1, TouchTask counts GP98 falling edges once every
//! TOUCH_INT_RATE_WINDOW_MS and prints them. The question to answer is "does the
//! CHSC5816 INT give one short pulse per report, or stay low until the host reads"
//! -- that decides how risky a missed edge is, and with no CHSC5816 datasheet in
//! the SDK it cannot be settled from the code, only by measurement.
//!
//! Output fields:
//!
//!   edges     how many times the ISR callback ran, i.e. falling edges that really reached the CPU
//!   polls     how many touch_poll() calls (= I2C reads)
//!   timeouts  how many of those polls were woken by the timeout rather than an interrupt
//!   burst     the largest notification count returned by one ulTaskNotifyTake().
//!             > 1 means edges arrived while we were reading I2C and were coalesced
//!             by the FreeRTOS notification count -- nothing was missed, they were
//!             merged into one pass
//!   lowIn     how often INT was still low on waking, before the I2C read
//!   lowOut    how often INT was still low after the I2C read
//!
//! lowIn / lowOut are what separate the two waveforms; edges alone is not enough
//! (both waveforms give about 100 edges per second at a 100 Hz report rate):
//!
//!   short pulse        lowIn near 0 -- the pulse was long over by the time we got there
//!   held low until read  lowIn near polls and lowOut near 0 -- the read itself cleared it
//!
//! The latter would mean a natural handshake between interrupt and read: a missed
//! edge still leaves the line low, the next backup poll is certain to see it and
//! touch_released cannot get stuck. It is the former that genuinely needs the
//! TOUCH_HOLD_POLL_MS backup.
//!
//! Nothing is printed unless edges is non-zero or a finger is down, so an idle device does not spam the log.
//!
//! Interference from the measurement itself: one extra RTT printf per window (once
//! a second while held), which delays that one poll slightly, plus two extra GPIO
//! reads per poll -- register accesses, negligible.
//
//*****************************************************************************
#define TOUCH_INT_RATE_DEBUG        (0)
#define TOUCH_INT_RATE_WINDOW_MS    (1000)
//*****************************************************************************
//
// Gesture thresholds.
//
// A gesture is always classified when the finger lifts, so one swipe triggers at
// most one action. The three classes are separated by the dominant axis and the
// magnitude of the displacement:
//
//   page change (left/right)   horizontal >= SWIPE_MIN_DX and vertical < SWIPE_MAX_DY
//   pull out overlay (up/down) vertical >= SWIPE_MIN_DY and horizontal < SWIPE_MAX_DX
//   tap                        both axes < TAP_MAX_MOVE
//
// TAP_MAX_MOVE is 24 px rather than something smaller: the panel is 468 px wide
// and a fingertip has a large contact area, so a "stationary" tap still drifts a
// dozen-odd pixels in practice.
//
//*****************************************************************************
#define SWIPE_MIN_DX        (80)
#define SWIPE_MAX_DY        (110)
#define SWIPE_MIN_DY        (70)
#define SWIPE_MAX_DX        (110)
#define TAP_MAX_MOVE        (24)

//
// To pull an overlay inward from a screen edge, the trace must reach a band of
// this width at that edge, with the same meaning as pulling the control centre
// down from the top of a phone or the controls up from the bottom. Without that
// restriction, any vertical swipe in the middle of the screen would summon an overlay.
//
// The top edge (control centre) and the bottom edge (brightness panel) share one
// width, hence the single constant: the two gestures are symmetric actions and
// there is no reason for one to be harder to trigger. Changing this affects both.
//
#define UI_EDGE_PULL_BAND   (150)

//
// The individual tests follow from it, top to bottom.
//
//   top edge     highest point of the trace <= 150        (0 .. 150)
//   bottom edge  lowest point of the trace  >= 468 - 150  (318 .. 467)
//
#define CC_PULL_START_MAX_Y (UI_EDGE_PULL_BAND)
#define BL_PULL_START_MIN_Y (FB_RESY - UI_EDGE_PULL_BAND)

//*****************************************************************************
//
//! @brief Minimum displacement to pull an overlay from an edge.
//!
//! @details Looser than SWIPE_MIN_DY, because "the trace must reach the edge band"
//! is already strict enough on its own and cannot be triggered by an ordinary swipe
//! in the middle of the screen; and pulling inward from a screen edge is a short
//! movement by nature, so keeping 70 px would make it harder than a page change.
//
//*****************************************************************************
#define EDGE_PULL_MIN_DY    (40)

//*****************************************************************************
//
//! @brief Dominant-axis requirement for an edge gesture: vertical magnitude at least half the horizontal.
//!
//! @details Edge gestures are classified before page changes (see
//! touch_gesture_classify()), and ignoring lateral movement entirely would erode
//! page changes -- on a round screen the touchable area at y > 318 is still wide,
//! and an arcing horizontal page swipe there easily drifts 40 px or more in y.
//!
//! The 1/2 factor is deliberately much looser (the page-change branch, by contrast,
//! requires |dx| >= 80 and |dy| < 110): pulling inward from a screen edge naturally
//! traces an arc, since the thumb pivots at the wrist, and lateral and vertical
//! travel of the same order is the norm. In practice, raise the denominator if an
//! upward pull still gets stolen by a page change (making an upward pull easier to
//! detect), and lower it if page changes keep summoning the overlay.
//!
//! @param dyPull  The vertical magnitude in that direction (i32PullUp or i32PullDown), non-negative.
//! @param absDx   Absolute horizontal displacement.
//
//*****************************************************************************
#define EDGE_PULL_AXIS_OK(dyPull, absDx)    (((dyPull) * 2) >= (absDx))

//*****************************************************************************
//
//! @brief A touch gesture.
//!
//! @details Originally there were only the two page-change swipes; with the control
//! centre and the brightness panel it also has to express "pull down from the top
//! edge", "push back up" and "tap a given cell", hence a structure carrying
//! coordinates -- a tap needs its position for hit testing, and an edge gesture
//! needs the trace to tell whether it started at a screen edge.
//
//*****************************************************************************
typedef enum
{
    GESTURE_NONE = 0,
    GESTURE_SWIPE_LEFT,
    GESTURE_SWIPE_RIGHT,
    GESTURE_SWIPE_DOWN,
    GESTURE_SWIPE_UP,
    GESTURE_TAP
} gesture_type_e;

typedef struct
{
    gesture_type_e eType;
    uint16_t       ui16X;       // Position at release (GESTURE_TAP uses it for hit testing)
    uint16_t       ui16Y;
    uint16_t       ui16StartX;  // Position at press
    uint16_t       ui16StartY;

    //
    // Extremes of y over the whole press.
    //
    // Edge gestures are classified from these two rather than from ui16StartY: it
    // is enough that the trace reached the edge band at any point, with no gamble
    // on the press instant happening to be sampled. Before this revision the start
    // was "the first point sampled", and a fast swipe drifted out of the edge band
    // and failed (see the file header); even with sampling now at 15 ms, extremes
    // remain more reliable than endpoints, at the cost of two comparisons per hold.
    //
    uint16_t       ui16MinY;
    uint16_t       ui16MaxY;

    //
    // When this press happened and how many fingers it used, for double-tap
    // detection (see dtap_tap() in render_task.c).
    //
    // The times are recorded here rather than by RenderTask looking at the clock
    // when it receives the gesture: RenderTask takes one gesture from the queue
    // per frame, by which point the real release is one to several frames old
    // (18-35 ms per frame, longer when busy) -- far too coarse for measuring a
    // gap of about 300 ms. Here the error is a single report period (17 ms).
    //
    // ui8MaxFingers is the most fingers held at once over the whole press. When a
    // pinch ends and the two fingers lift one after the other, the last one can
    // start and end close together and be classified as a tap; this field is what
    // identifies it as the tail of a two-finger gesture rather than a tap.
    //
    uint32_t       ui32DownTick;    // tick of the first sample with the finger down
    uint32_t       ui32UpTick;      // tick of the first sample with the finger up
    uint8_t        ui8MaxFingers;

    //
    // Below is the "live state of this frame", as opposed to the fields above, which only mean something at release.
    //
    // The brightness slider needs it: gestures are classified when the finger lifts
    // (one swipe, one action -- that design stands), but dragging a slider has to
    // update the brightness continuously during the hold, or changing it only on
    // release feels nothing like a slider. So the live coordinates are carried out
    // alongside, letting both meanings coexist.
    //
    bool           bPressed;    // Whether a finger is down this frame
    uint16_t       ui16CurX;    // Live coordinate while bPressed is true
    uint16_t       ui16CurY;

    //
    // Live state for pinch zoom.
    //
    // It takes the live channel rather than a discrete gesture for the same reason
    // as the brightness slider: zoom has to follow the fingers as they move, and
    // zooming only on release feels nothing like a pinch. The map page therefore
    // computes the change in finger distance itself each frame, and this only
    // delivers the raw coordinates.
    // ui16CurX2/ui16CurY2 are meaningful only while ui8Fingers is 2. It has already
    // been filtered through touch_released in touch.c, so it is necessarily 0 after
    // release (the driver's own finger_number does not reset; see touch.h).
    //
    uint8_t        ui8Fingers;
    uint16_t       ui16CurX2;
    uint16_t       ui16CurY2;
} gesture_t;

//*****************************************************************************
//
//! @brief The touch task itself.
//!
//! @details Touch initialization (IOM2, GP98 interrupt registration) is
//! deliberately left inside the task, for the same reason as led_task.c: an
//! interrupt can arrive the moment it is enabled, and the callback needs a task
//! handle that is already valid.
//!
//! @param pvParameters Unused.
//!
//! @return Does not return.
//
//*****************************************************************************
extern void TouchTask(void *pvParameters);

//*****************************************************************************
//
//! @brief Take the pending gesture and the current live press state.
//!
//! @details Called once per frame by RenderTask. The two kinds of data are obtained differently, because they mean different things:
//!
//!   - The discrete gesture (the one classified at release) comes through a queue
//!     and cannot be read as "the latest value", or a quick double tap would be
//!     overwritten by the later one. eType is GESTURE_NONE when the queue is empty.
//!   - The live press state is always filled with the current value. Its meaning is
//!     "where the finger is now", and old values are worth nothing.
//!
//! The caller is never blocked (the queue wait time is 0).
//!
//! @param psGesture Structure receiving the result, must not be NULL.
//!
//! @return None.
//
//*****************************************************************************
extern void TouchTaskGetGesture(gesture_t *psGesture);

#ifdef __cplusplus
}
#endif

#endif // TOUCH_TASK_H
