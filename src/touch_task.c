//*****************************************************************************
//
//! @file touch_task.c
//!
//! @brief Touch task: interrupt-driven CHSC5816 sampling and gesture classification.
//!
//! The rationale, the purpose of each of the two timeouts and the ownership of
//! the ISR and the I2C are all in touch_task.h. This file is implementation only.
//
//*****************************************************************************

#include <stdint.h>
#include <stdbool.h>

#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "touch.h"
#include "touch_task.h"

//*****************************************************************************
//
// Touch task handle.
//
//*****************************************************************************
TaskHandle_t TouchTaskHandle;

//*****************************************************************************
//
// Queue of discrete gestures.
//
// Depth 4: a gesture is produced only when the finger lifts, one per swipe, so
// under normal use at most one sits in the queue. 4 covers the case of
// "RenderTask preempted by RadioTask for several frames while the user tapped a
// few times" -- a gesture is the user's intent, better handled a frame or two
// late than dropped silently.
//
//*****************************************************************************
#define TOUCH_GESTURE_QUEUE_LEN     (4)

static QueueHandle_t g_qTouchGesture = NULL;

//*****************************************************************************
//
// Live press state.
//
// Kept separate from the discrete gestures; see TouchTaskGetGesture() for why.
// The three fields are only meaningful read and written together (coordinates
// that do not match pressed would make the brightness slider take the previous
// press position), so access is always wrapped in a critical section rather than
// relying on volatile alone.
//
//*****************************************************************************
typedef struct
{
    bool     bPressed;
    uint16_t ui16X;
    uint16_t ui16Y;

    //
    // Second finger and the current finger count, for pinch zoom (see gesture_t).
    //
    uint8_t  ui8Fingers;
    uint16_t ui16X2;
    uint16_t ui16Y2;
} touch_live_t;

static touch_live_t g_sLive = { false, 0, 0, 0, 0, 0 };

#if TOUCH_INT_RATE_DEBUG
//*****************************************************************************
//
// Cumulative count of GP98 falling edges (diagnostic, see TOUCH_INT_RATE_DEBUG in touch_task.h).
//
// Incremented only in ISR context and read only in task context, so no critical
// section is needed -- unlike g_sLive, where three or more fields must agree.
// Here there is one aligned 32-bit variable, and a load or store of it is never
// split on Cortex-M. volatile is there to stop the compiler folding the task
// side's repeated reads into one.
//
// The statistics window works on differences (ui32WinEdgeBase below), so an
// overflow of this counter does not affect the result either: unsigned
// subtraction wraps in a well-defined way.
//
//*****************************************************************************
static volatile uint32_t g_ui32TouchIntCount = 0;
#endif

//*****************************************************************************
//
//! @brief Callback that am_gpio0_607f_isr() dispatches the GP98 touch interrupt to.
//!
//! @details This function is not an ISR written into the vector table; it is
//! passed to the CHSC5816 driver as touch_init(touch_int_callback) at TouchTask
//! startup, and the driver registers it as the GP98 callback through
//! am_hal_gpio_interrupt_register().
//!
//! GP98 belongs to the GPIO 96-127 interrupt group, so when the touch IC pulls
//! INT low the CPU first enters am_gpio0_607f_isr(). That ISR reads and clears
//! the group's pending status, then calls am_hal_gpio_interrupt_service(); the
//! GPIO HAL looks up the registration for the GP98 pending bit and finally calls
//! this function. The full chain is:
//!
//!   CHSC5816 INT falling edge (GP98)
//!       -> am_gpio0_607f_isr()
//!       -> am_hal_gpio_interrupt_service()
//!       -> touch_int_callback()
//!       -> vTaskNotifyGiveFromISR()
//!       -> TouchTask wakes and reads I2C in task context
//!
//! So although this is a callback, it still executes in ISR context. It only
//! notifies TouchTask and never performs the potentially blocking I2C read itself.
//!
//! Why calling a FromISR API is legal here: inside am_devices_chsc5816_init() the
//! driver calls NVIC_SetPriority(..., 0x4) on this IRQ, i.e.
//! AM_IRQ_PRIORITY_DEFAULT, which is below (numerically above)
//! configMAX_SYSCALL_INTERRUPT_PRIORITY. The same situation as Button_callback() in led_task.c.
//!
//! No debouncing: the touch IC already handles it, and dropping an interrupt
//! here costs far more than with a mechanical button (see touch_task.h on missed edges).
//!
//! @param pArg Unused.
//!
//! @return None.
//
//*****************************************************************************
static void
touch_int_callback(void *pArg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    (void)pArg;

#if TOUCH_INT_RATE_DEBUG
    //
    // Before the notify: the notification may let TouchTask preempt immediately
    // (its priority is above the task this ISR returns to), so incrementing
    // first keeps the task from reading a count that is one short.
    //
    g_ui32TouchIntCount++;
#endif

    vTaskNotifyGiveFromISR(TouchTaskHandle, &xHigherPriorityTaskWoken);

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

//*****************************************************************************
//
//! @brief Classify the trace of this press into a gesture.
//!
//! @details Called the moment the finger lifts. The order is "tap first, then
//! edge gestures, then horizontal, then vertical":
//!
//!   - The tap threshold (both axes < TAP_MAX_MOVE) is the strictest and has to
//!     come first, or the looser directional tests below would swallow it.
//!   - Edge gestures before horizontal is a necessity, not a preference. A thumb
//!     pushing up from the bottom of a round screen traces an arc, and 80 px of
//!     lateral movement is normal; the horizontal branch tests
//!     |dy| < 110 && |dx| >= 80, so "90 px up plus 85 px across" would count as a
//!     page change, the brightness panel could never be pulled out and the page
//!     would jump as well. Putting vertical gestures whose trace touched the edge
//!     band first makes it behave like a phone: edge gestures outrank ordinary swipes.
//!   - Horizontal before ordinary vertical preserves the page-change feel from
//!     before this revision: a slightly slanted horizontal swipe still changes pages.
//!
//! These branches are chained with if / else if, so the horizontal branch's
//! condition has to mean "genuinely a horizontal swipe": gating only on small
//! vertical movement would trap a pure vertical swipe in that branch, where it
//! fails, and the vertical branch would never be reached (this did happen, and
//! is why SWIPE_MIN_DY was effectively dead for a while).
//!
//! The magnitude of an edge gesture uses the trace extremes rather than the
//! endpoint displacement (ui16MinY / ui16MaxY, see gesture_t): it is enough that
//! the trace reached the edge band and the magnitude passed the threshold, with
//! no gamble on the press and release instants happening to be sampled.
//!
//! Beyond magnitude, the edge branch must also pass EDGE_PULL_AXIS_OK(), or it
//! erodes page changes: on a round screen the touchable area at y > 318 is still
//! wide, and an arcing horizontal page swipe there easily drifts 40 px or more in
//! y, turning a page change into the brightness panel. That macro requires the
//! vertical magnitude to be at least half the horizontal -- no effect at all on
//! a real upward pull (vertical far exceeding horizontal), while rejecting the
//! obviously lateral ones.
//!
//! @param psGesture Structure with coordinates and trace extremes already filled in; this function only sets eType.
//!
//! @return None.
//
//*****************************************************************************
static void
touch_gesture_classify(gesture_t *psGesture)// Classify the gesture when the finger lifts
{
    //
    // Endpoint displacement as "release coordinate - press coordinate". Screen X
    // increases to the right and Y increases downward, so dx < 0 means left and
    // dx > 0 right; dy < 0 means up and dy > 0 down.
    //
    int32_t i32Dx = (int32_t)psGesture->ui16X - (int32_t)psGesture->ui16StartX;
    int32_t i32Dy = (int32_t)psGesture->ui16Y - (int32_t)psGesture->ui16StartY;

    //
    // Direction needs the sign; the threshold tests care only about magnitude, so absolute values are computed separately.
    //
    int32_t i32AbsDx = (i32Dx < 0) ? -i32Dx : i32Dx;
    int32_t i32AbsDy = (i32Dy < 0) ? -i32Dy : i32Dy;

    //
    // An edge gesture cannot look at the release point alone, since the finger
    // may pull some distance and retreat slightly before lifting. ui16MinY is
    // the highest point of the whole trace, ui16MaxY the lowest:
    //   i32PullUp   = upward travel from the press point to the highest point
    //   i32PullDown = downward travel from the press point to the lowest point
    //
    int32_t i32PullUp   = (int32_t)psGesture->ui16StartY - (int32_t)psGesture->ui16MinY;
    int32_t i32PullDown = (int32_t)psGesture->ui16MaxY   - (int32_t)psGesture->ui16StartY;

    //
    // No appreciable displacement on either axis between press and release, so
    // this is a tap. The tap must be tested first, so small coordinate jitter
    // does not fall into the swipe tests below.
    //
    if ( (i32AbsDx < TAP_MAX_MOVE) && (i32AbsDy < TAP_MAX_MOVE) )
    {
        psGesture->eType = GESTURE_TAP;
    }
    //
    // Pull up from the bottom edge: the trace entered the bottom band, the upward
    // magnitude is sufficient, and the dominant direction is not clearly
    // horizontal. This pulls the brightness panel out from the bottom of the screen.
    //
    else if ( (psGesture->ui16MaxY >= BL_PULL_START_MIN_Y) &&
              (i32PullUp >= EDGE_PULL_MIN_DY) &&
              EDGE_PULL_AXIS_OK(i32PullUp, i32AbsDx) )
    {
        psGesture->eType = GESTURE_SWIPE_UP;
    }
    //
    // Pull down from the top edge: the trace entered the top band, the downward
    // magnitude is sufficient, and the dominant direction is not clearly
    // horizontal. This pulls the control centre down from the top of the screen.
    //
    else if ( (psGesture->ui16MinY <= CC_PULL_START_MAX_Y) &&
              (i32PullDown >= EDGE_PULL_MIN_DY) &&
              EDGE_PULL_AXIS_OK(i32PullDown, i32AbsDx) )
    {
        psGesture->eType = GESTURE_SWIPE_DOWN;
    }
    //
    // Ordinary horizontal swipe: the horizontal displacement must exceed the
    // minimum threshold while the vertical offset stays bounded, so a clear
    // vertical swipe is not misread as a page change. The sign of dx gives left or right.
    //
    else if ( (i32AbsDy < SWIPE_MAX_DY) && (i32AbsDx >= SWIPE_MIN_DX) )
    {
        psGesture->eType = (i32Dx <= -SWIPE_MIN_DX) ? GESTURE_SWIPE_LEFT
                                                    : GESTURE_SWIPE_RIGHT;
    }
    //
    // Ordinary vertical swipe: bound the horizontal offset first, then use the
    // sign of dy and the minimum threshold for direction. This branch comes after
    // the edge gestures, so it cannot steal the pull-out overlays.
    //
    else if ( i32AbsDx < SWIPE_MAX_DX )
    {
        if ( i32Dy >= SWIPE_MIN_DY )
        {
            psGesture->eType = GESTURE_SWIPE_DOWN;
        }
        else if ( i32Dy <= -SWIPE_MIN_DY )
        {
            psGesture->eType = GESTURE_SWIPE_UP;
        }
    }

    //
    // If nothing matched, keep the GESTURE_NONE the caller pre-set, meaning the
    // trace was too short or its direction unclear and no UI gesture is produced.
    //
}

//*****************************************************************************
//
//! @brief Take the pending gesture and the current live press state.
//
//*****************************************************************************
void
TouchTaskGetGesture(gesture_t *psGesture)
{
    touch_live_t sLive;

    if ( psGesture == NULL )
    {
        return;
    }

    //
    // The discrete gesture first. xQueueReceive() leaves psGesture untouched when
    // the queue is empty, so the whole structure is cleared here -- otherwise the
    // caller would read uninitialized stack contents.
    //
    //
    // Only eType is named; C's rules zero the rest. This used to be a long list
    // of positional initializers that had to be recounted every time gesture_t
    // gained a field -- the three pinch-zoom fields were very nearly missed that way.
    //
    gesture_t sEmpty = { .eType = GESTURE_NONE };
    *psGesture = sEmpty;

    if ( g_qTouchGesture != NULL )
    {
        (void)xQueueReceive(g_qTouchGesture, psGesture, 0);
    }

    //
    // The live state is always overwritten with the latest values, independent of
    // what came off the queue. The two are semantically separate: this frame may
    // both "have just completed a gesture" and "have a finger already down starting
    // the next one".
    //
    taskENTER_CRITICAL();
    sLive = g_sLive;
    taskEXIT_CRITICAL();

    psGesture->bPressed   = sLive.bPressed;
    psGesture->ui16CurX   = sLive.ui16X;
    psGesture->ui16CurY   = sLive.ui16Y;
    psGesture->ui8Fingers = sLive.ui8Fingers;
    psGesture->ui16CurX2  = sLive.ui16X2;
    psGesture->ui16CurY2  = sLive.ui16Y2;
}

//*****************************************************************************
//
// Touch task.
//
//*****************************************************************************
void
TouchTask(void *pvParameters)
{
    bool     bPrevPressed = false;
    uint16_t ui16StartX   = 0;
    uint16_t ui16StartY   = 0;
    uint16_t ui16LastX    = 0;
    uint16_t ui16LastY    = 0;
    uint16_t ui16MinY     = 0;
    uint16_t ui16MaxY     = 0;
    uint32_t ui32DownTick  = 0;
    uint8_t  ui8MaxFingers = 0;

#if TOUCH_INT_RATE_DEBUG
    //
    // Accumulators for the statistics window. All task locals -- only this task touches them, so no synchronization.
    //
    TickType_t xWinStart       = 0;
    uint32_t   ui32WinEdgeBase = 0;
    uint32_t   ui32Polls       = 0;
    uint32_t   ui32Timeouts    = 0;
    uint32_t   ui32MaxBurst    = 0;
    uint32_t   ui32LowIn       = 0;
    uint32_t   ui32LowOut      = 0;
#endif

    (void)pvParameters;

    //
    // The gesture queue must exist before interrupts are enabled: once they are on, the first touch may arrive immediately.
    //
    g_qTouchGesture = xQueueCreate(TOUCH_GESTURE_QUEUE_LEN, sizeof(gesture_t));
    if ( g_qTouchGesture == NULL )
    {
        am_util_debug_printf("[touch] gesture queue create failed (heap %u B free)\n",
                             (unsigned)xPortGetFreeHeapSize());

        //
        // With no queue there are no gestures to send, and continuing would only
        // burn CPU and battery. Let the task exit; the UI stays on the home page
        // -- easier to diagnose than pretending to work.
        //
        vTaskDelete(NULL);
    }

    //
    // Initialize the CHSC5816 and register the GP98 interrupt callback. Done
    // inside the task rather than in rtos.c; see touch_task.h: the callback needs
    // a task handle that is already valid.
    //
    if ( touch_init(touch_int_callback) != 0 )
    {
        am_util_debug_printf("[touch] init failed, touch task exiting\n");
        vTaskDelete(NULL);
    }

    //
    // Do one read up front to clear whatever data the touch IC may be holding.
    //
    // The purpose is to let the INT line return high: if the IC pulls INT low
    // right after reset because it has data, that falling edge was already
    // cleared by the interrupt_clear inside am_devices_chsc5816_init() (see
    // touch_task.h). The line is then low, and without draining the data there
    // will never be another falling edge. Reading once restores normal edges.
    //
    // It would still work without this step -- the TOUCH_IDLE_POLL_MS safety net
    // catches up after 500 ms -- but the first touch after boot would take half a
    // second to respond, and there is no reason to make the user meet that.
    //
    touch_poll();

    am_util_debug_printf("[touch] task started (int-driven, %u/%u ms hold/idle fallback)\n",
                         (unsigned)TOUCH_HOLD_POLL_MS,
                         (unsigned)TOUCH_IDLE_POLL_MS);

#if TOUCH_INT_RATE_DEBUG
    //
    // The first window starts here, with the baseline taken after the boot-time
    // touch_poll() -- edges during init (if any) should not count towards the
    // statistics of user presses.
    //
    xWinStart       = xTaskGetTickCount();
    ui32WinEdgeBase = g_ui32TouchIntCount;

    am_util_debug_printf("[touch] int-rate debug on, window %u ms\n",
                         (unsigned)TOUCH_INT_RATE_WINDOW_MS);
#endif

    while (1)
    {
        touch_state_t sState;

        //
        // A short timeout backs up the hold, and when idle only one long-period
        // final safety net remains. This is the core of this revision; see
        // why_not_pure_int in touch_task.h -- in particular why portMAX_DELAY
        // cannot be used while idle.
        //
        TickType_t xWait = bPrevPressed ? pdMS_TO_TICKS(TOUCH_HOLD_POLL_MS)
                                        : pdMS_TO_TICKS(TOUCH_IDLE_POLL_MS);

        //
        // The return value is "the notification count before clearing", i.e. how
        // many edges accumulated during this sleep: 0 means a timeout wake-up,
        // > 1 means edges arrived while we were busy and were coalesced by the
        // notification count. Nobody uses it with diagnostics off, hence the (void)
        // to avoid -Wunused.
        //
        uint32_t ui32Notified = ulTaskNotifyTake(pdTRUE, xWait);
        (void)ui32Notified;

#if TOUCH_INT_RATE_DEBUG
        //
        // Sample the INT level before reading I2C. It has to be taken before
        // touch_poll(), because if the CHSC5816 holds the line low until the host
        // reads, the read itself releases it.
        //
        uint32_t ui32LevelIn  = 1;
        uint32_t ui32LevelOut = 1;

        am_hal_gpio_state_read(AM_BSP_GPIO_TOUCH_INT,
                               AM_HAL_GPIO_INPUT_READ,
                               &ui32LevelIn);
#endif

        //
        // Read the coordinates whether woken by an interrupt or by a timeout. A
        // timeout wake-up must read too, because the missed edge may have been the
        // lift event (the driver only sets touch_released to true on the packet
        // where event == 4).
        //
        touch_poll();
        touch_get_info(&sState);

#if TOUCH_INT_RATE_DEBUG
        am_hal_gpio_state_read(AM_BSP_GPIO_TOUCH_INT,
                               AM_HAL_GPIO_INPUT_READ,
                               &ui32LevelOut);

        ui32Polls++;

        if ( ui32Notified == 0 )
        {
            ui32Timeouts++;
        }
        if ( ui32Notified > ui32MaxBurst )
        {
            ui32MaxBurst = ui32Notified;
        }
        if ( ui32LevelIn == 0 )
        {
            ui32LowIn++;
        }
        if ( ui32LevelOut == 0 )
        {
            ui32LowOut++;
        }

        //
        // The window is only checked on wake-up, so its real length is slightly
        // above the configured value (during a hold the 17 ms report rate drives
        // it and the error is tiny; while idle the timeout is 500 ms, so it can
        // run to 1.5x). Printing the measured elapsed time rather than the
        // configured one keeps the per-second rate correct.
        //
        if ( (xTaskGetTickCount() - xWinStart) >= pdMS_TO_TICKS(TOUCH_INT_RATE_WINDOW_MS) )
        {
            TickType_t xNow    = xTaskGetTickCount();
            uint32_t   ui32Ms  = (uint32_t)((xNow - xWinStart) * portTICK_PERIOD_MS);
            uint32_t   ui32Edges = g_ui32TouchIntCount - ui32WinEdgeBase;

            //
        // Print nothing while idle (no edges and nothing pressed), or one line a
        // second would drown the log and bury the numbers from the few seconds
        // that were actually pressed.
            //
            if ( (ui32Edges != 0) || sState.pressed )
            {
                am_util_debug_printf("[touch] int-rate %u ms: edges=%u polls=%u "
                                     "timeouts=%u burst=%u lowIn=%u lowOut=%u pressed=%u\n",
                                     (unsigned)ui32Ms,
                                     (unsigned)ui32Edges,
                                     (unsigned)ui32Polls,
                                     (unsigned)ui32Timeouts,
                                     (unsigned)ui32MaxBurst,
                                     (unsigned)ui32LowIn,
                                     (unsigned)ui32LowOut,
                                     (unsigned)sState.pressed);
            }

            xWinStart       = xNow;
            ui32WinEdgeBase = g_ui32TouchIntCount;
            ui32Polls       = 0;
            ui32Timeouts    = 0;
            ui32MaxBurst    = 0;
            ui32LowIn       = 0;
            ui32LowOut      = 0;
        }
#endif

        //
        // The live state is updated first, independent of gesture classification.
        // The brightness slider relies on it to track the finger during a hold
        // (see gesture_t).
        //
        taskENTER_CRITICAL();
        g_sLive.bPressed   = sState.pressed;
        g_sLive.ui16X      = sState.x;
        g_sLive.ui16Y      = sState.y;
        g_sLive.ui8Fingers = sState.ui8Fingers;
        g_sLive.ui16X2     = sState.x2;
        g_sLive.ui16Y2     = sState.y2;
        taskEXIT_CRITICAL();

        if ( sState.pressed )
        {
            if ( !bPrevPressed )
            {
                //
                // Finger down: latch the start point and seed the trace extremes from it.
                //
                ui16StartX = sState.x;
                ui16StartY = sState.y;
                ui16MinY   = sState.y;
                ui16MaxY   = sState.y;

                ui32DownTick  = (uint32_t)xTaskGetTickCount();
                ui8MaxFingers = 0;
            }

            //
            // Keep recording the latest position and the y extremes. The touch IC
            // stops updating coordinates once the finger lifts, so the decision on
            // release has to rely on what was recorded here during the hold.
            //
            ui16LastX = sState.x;
            ui16LastY = sState.y;

            if ( sState.y < ui16MinY )
            {
                ui16MinY = sState.y;
            }
            if ( sState.y > ui16MaxY )
            {
                ui16MaxY = sState.y;
            }

            //
            // The most fingers held at once over the whole press. Looking only at
            // the moment of release is not enough: after a pinch the two fingers
            // usually lift one after the other, so only one is left at the end.
            //
            if ( sState.ui8Fingers > ui8MaxFingers )
            {
                ui8MaxFingers = sState.ui8Fingers;
            }
        }
        else if ( bPrevPressed )// The instant of release, where sState.pressed = 0
        {
            //
            // Finger released: classify the gesture and push it onto the queue.
            //
            gesture_t sGesture = { .eType = GESTURE_NONE };

            sGesture.ui16X      = ui16LastX;
            sGesture.ui16Y      = ui16LastY;
            sGesture.ui16StartX = ui16StartX;
            sGesture.ui16StartY = ui16StartY;
            sGesture.ui16MinY   = ui16MinY;
            sGesture.ui16MaxY   = ui16MaxY;

            sGesture.ui32DownTick  = ui32DownTick;
            sGesture.ui32UpTick    = (uint32_t)xTaskGetTickCount();
            sGesture.ui8MaxFingers = ui8MaxFingers;

            touch_gesture_classify(&sGesture);

            if ( sGesture.eType != GESTURE_NONE )
            {
                //
                // A full queue means dropping it and saying so. Waiting is not an
                // option: TouchTask outranks RenderTask (its only consumer), so
                // blocking here would deadlock the two.
                //
                if ( xQueueSend(g_qTouchGesture, &sGesture, 0) != pdPASS )
                {
                    am_util_debug_printf("[touch] gesture queue full, dropped %d\n",
                                         (int)sGesture.eType);
                }
            }
        }

        bPrevPressed = sState.pressed;// Update the previous state
    }
}
