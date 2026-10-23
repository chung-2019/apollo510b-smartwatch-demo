//*****************************************************************************
//
//! @file led_task.c
//!
//! @brief On-board LED task (driven by BTN0 / GP46 and the home control centre).
//!
//! Manages two LEDs: GP11 (silkscreen LED2) and GP83 (silkscreen LED4). Both are
//! wired the same way, open-drain and active low, so they share one state table
//! and one write path.
//!
//! Each button corresponds to one LED:
//!
//!   BTN0 / GP46 (SW2) -> LED2 / GP11
//!   BTN1 / GP29 (SW3) -> LED4 / GP83
//!
//! There are two sources of control. Both only set a flag and wake this task; neither touches GPIO:
//!
//!   Button_callback()   ISR context, one press toggles that button's LED
//!   LedTaskSetState()   task context, called when a control centre icon is tapped in the render task
//!
//! The data flow is deliberately in three stages, with the bare minimum in interrupt context:
//!
//!   am_gpio0_203f_isr()   the display driver's existing one (GP46 -> 46/32 = 1)
//!   am_gpio0_001f_isr()   defined by this module (GP29 -> 29/32 = 0)
//!         |               both read and clear the IRQ status and hand over to
//!         |               am_hal_gpio_interrupt_service() for per-pin dispatch
//!         v
//!   Button_callback()     debounce, toggle that button's LED flag, notify LedTask
//!         |
//!         v
//!   LedTask/led_handler() drive GP11 / GP83 from the flags
//!
//! Why it is split: "press to light, press again to extinguish" is only toggling
//! a bool, but keeping the GPIO write and the log output in task context keeps
//! the ISR clear of am_util_stdio_printf() (shared static buffer, no lock) and
//! keeps interrupt latency from disturbing RadioTask's BLE connection events or
//! render's cadence.
//
//*****************************************************************************

#include "led_task.h"

#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

//
// BTN1 doubles as the Home key (see Button_callback()), which needs RenderTaskGoHome().
//
// render_task.h has no includes of its own; the TaskHandle_t and img_obj_t it
// uses come from the common headers, so it must come after them -- the same
// order as in sensor_task.c and display_task.c.
//
#include "nemagfx_watchface.h"
#include "render_task.h"

//*****************************************************************************
//
// Hardware mapping.
//
// LED2 is AM_BSP_GPIO_LED2 = GP83 in the BSP (silkscreen LED4 on the board), a
// different pin from LED3 (AM_BSP_GPIO_LED1 = GP19), which ble_led.c uses to
// indicate BLE advertising and connection. BTN0 is AM_BSP_GPIO_BUTTON0 = GP46.
//
//*****************************************************************************


//
// Pin table for the controlled LEDs, indexed by led_id_e.
//
// The BSP macro names are off by one from the board silkscreen (AM_BSP_GPIO_LED0
// is printed LED2). Silkscreen numbering is used everywhere outside, and this
// table is the only place that has to understand the difference.
//
static const uint32_t g_pui32LedPins[LED_ID_COUNT] =
{
    AM_BSP_GPIO_LED0,   // LED_ID_LED2 -> GP11
    AM_BSP_GPIO_LED2    // LED_ID_LED4 -> GP83
};

//
// The matching BSP pin configurations. They are identical apart from uFuncSel,
// and are still listed separately rather than sharing one and patching
// uFuncSel, so a future BSP change to either pin is picked up automatically.
//
static const am_hal_gpio_pincfg_t * const g_ppsLedCfgs[LED_ID_COUNT] =
{
    &g_AM_BSP_GPIO_LED0,
    &g_AM_BSP_GPIO_LED2
};

//
// LED2, like LED3, is open-drain and active low: CLEAR pulls the pin low and
// lights it, SET releases it and lets it go out. Same reason as ble_led.c --
// am_devices_led is not used here either, because am_bsp_psLEDs[] omits
// AM_DEVICES_LED_POL_OPEN_DRAIN and would be switched to tristate, diverging
// from how the board is actually wired.
//
#define LED_TASK_LED_ON_STATE   AM_HAL_GPIO_OUTPUT_CLEAR
#define LED_TASK_LED_OFF_STATE  AM_HAL_GPIO_OUTPUT_SET

//*****************************************************************************
//
// Button mapping table.
//
// The two buttons fall in different GPIO IRQ groups, which decides whether an ISR has to be written here:
//
//   GP46 -> 46/32 = 1 -> GPIO0_203F_IRQn
//        This one is shared with the display panel's TE (GPIO33); the ISR body
//        is the display driver's, and defining another function of the same name
//        gives a multiple definition link error.
//
//   GP29 -> 29/32 = 0 -> GPIO0_001F_IRQn
//        This one has no other user in this project, and startup_gcc.c only has
//        a weak alias pointing to am_default_isr. Without a definition here, the
//        button interrupt would land in the default handler: no response to a
//        press and no error message either. Defined below in this file.
//
// The touch INT (GPIO98) and the EM9305 INT (GPIO117) are on GPIO0_607F_IRQn and
// have nothing to do with this.
//
// The debounce state (tick and count) lives in each row's own fields, so the two
// buttons are entirely independent -- sharing one set would let a press of BTN0
// immediately followed by BTN1 swallow the second as bounce from the first.
//
//*****************************************************************************
typedef struct
{
    uint32_t                            ui32Pin;
    IRQn_Type                           eIrq;
    led_id_e                            eLed;
    const am_hal_gpio_pincfg_t         *psCfg;
    const char                         *pcName;
    volatile uint32_t                   ui32LastTick;
    volatile uint32_t                   ui32PressCount;
} button_map_t;

static button_map_t g_psButtons[] =
{
    {
        AM_BSP_GPIO_BUTTON0, GPIO0_203F_IRQn, LED_ID_LED2,
        &g_AM_BSP_GPIO_BUTTON0, "BTN0(GP46)", 0, 0
    },
    {
        AM_BSP_GPIO_BUTTON1, GPIO0_001F_IRQn, LED_ID_LED4,
        &g_AM_BSP_GPIO_BUTTON1, "BTN1(GP29)", 0, 0
    }
};

#define LED_TASK_BUTTON_COUNT   (sizeof(g_psButtons) / sizeof(g_psButtons[0]))

//*****************************************************************************
//
// Globals.
//
//*****************************************************************************
TaskHandle_t LedTaskHandle = NULL;

//
// Target state of each LED. Written by Button_callback() (ISR context) and
// LedTaskSetState() (task context) and read by led_handler() (task context),
// hence volatile.
//
// The array elements are independent, each written by a single source at a time,
// and a bool load or store is atomic on Cortex-M, so no critical section is
// needed: the ISR only touches LED_ID_LED4, the control centre may touch both,
// but the two never write the same element at once -- and if the UI set LED4 at
// the very instant the ISR toggled it, one of them wins and both outcomes are
// legal states.
//
static volatile bool g_pbLedOnFlags[LED_ID_COUNT] = { false, false };

//*****************************************************************************
//
//! @brief Button interrupt callback, shared by both buttons.
//!
//! @details Dispatched here by am_hal_gpio_interrupt_service() per pin, running
//! in ISR context. It does only three things: debounce, toggle the on flag and
//! notify LedTask; the actual GPIO work is left to led_handler().
//!
//! Both buttons share one function and are distinguished by pArg rather than
//! having one each: the debounce logic has a number of subtleties (unsigned
//! wraparound, the first-press exception), and the risk of later fixing only one
//! of two copies far outweighs one extra pointer dereference.
//!
//! @param pArg The button_map_t* passed at registration, identifying the button.
//
//*****************************************************************************
static void
Button_callback(void *pArg)
{
    BaseType_t    xHigherPriorityTaskWoken = pdFALSE;
    button_map_t *psBtn = (button_map_t *)pArg;
    uint32_t      ui32Now;

    //
    // Registration always passes pArg, but it is checked anyway: dereferencing
    // NULL would land near the vector table, the hardest kind of crash to trace.
    //
    if ( psBtn == NULL )
    {
        return;
    }

    //
    // An interrupt before the task exists (which cannot happen in theory, since
    // interrupts are only enabled inside LedTask) -- ignore it, as notify must
    // not be called on a NULL handle.
    //
    if ( LedTaskHandle == NULL )
    {
        return;
    }

    //
    // Take the current time for the debounce test below.
    //
    // What this returns is the current value of the FreeRTOS software tick
    // counter xTickCount, meaning "which tick since boot it is now", and not a
    // hardware timestamp of this falling edge -- Cortex-M GPIO has no
    // edge-capture register, and the raw STIMER count is not read here either.
    // There are two sources of error against "when the edge really happened":
    //
    //   * Interrupt latency plus the few lines between entering the ISR and this
    //     one (disp_te_isr()'s status_get / clear / dispatch, and the NULL check
    //     above). Microseconds.
    //   * Resolution of only 1 ms (configTICK_RATE_HZ = 1000). And the GPIO IRQ
    //     has priority 4, which preempts the kernel's STIMER tick ISR (priority
    //     15), so one tick may not be counted yet, leaving ui32Now up to one tick
    //     behind real time.
    //
    // Both are negligible against a 50 ms debounce window, so ticks are enough
    // and there is no need to read STIMER for finer time.
    //
    // Why the FromISR version: it reads the same xTickCount (tasks.c) as
    // xTaskGetTickCount(), so the time source is identical. The only differences
    // are the critical section mechanism
    // (portTICK_TYPE_SET_INTERRUPT_MASK_FROM_ISR() versus
    // portTICK_TYPE_ENTER_CRITICAL()) and one extra
    // portASSERT_IF_INTERRUPT_PRIORITY_INVALID() check. On Cortex-M, TickType_t
    // is 32-bit and the read is atomic in itself, so both return the same value;
    // FromISR is used to obey the API rule that calling a non-FromISR version in
    // interrupt context is illegal.
    //
    // Can the tick fail to be corrected when a button wakes the device from deepsleep? No.
    // The order in vPortSuppressTicksAndSleep() in port_stimer.c is:
    //
    //     cpsid i    -> PRIMASK set: interrupts can still wake WFI, but handlers do not run
    //     wfi        -> the button interrupt wakes the CPU here, before this function is called
    //     vTaskStepTick(ulElapsedTick)  -> STIMER gives how many ticks were slept, and corrects
    //     cpsie i    -> only here does the pending GPIO ISR actually fire
    //
    // Because cpsid i only masks execution and does not affect WFI wake-up,
    // xTickCount is already correct before this function runs. So even after the
    // device has slept for minutes, the difference computed below is the real
    // elapsed time and this press is not mistaken for bounce and dropped.
    //
    ui32Now = (uint32_t)xTaskGetTickCountFromISR();

    //
    // Debounce test. If it holds, this edge is dropped. The two conditions each have a purpose:
    //
    // The second half (the time window) is the actual debounce: an edge less than
    // 50 ms after "the last accepted edge" is mechanical bounce from the same
    // press. The metal contacts bounce several times at the moment of contact,
    // each one a falling edge, and taking them all would toggle that LED's flag
    // several times per press -- an even number of toggles looks like the LED not
    // responding at all.
    //
    // Unsigned subtraction is used rather than
    // (ui32Now < ui32LastTick + BUTTON_DEBOUNCE_TICKS) so that it stays correct
    // across tick overflow (2^32 ms is about 49.7 days): the subtraction wraps
    // naturally in uint32_t and the difference is right, whereas the addition
    // overflows first and misjudges around the wrap point.
    //
    // The first half (the press count) is not a redundant safeguard but a necessary exception for the first press.
    // ui32LastTick starts at 0, but that 0 means "no edge yet" rather than "an
    // edge happened at tick 0". Without this test it is misread: press the button
    // within the first 50 ms after boot (holding it while resetting the board,
    // say) with ui32Now at, for instance, 30, and 30 - 0 = 30 < 50 holds, so the
    // first press is swallowed, the LED stays dark and the button looks broken.
    // With the count test the first edge is always accepted (there is no "last
    // time" to compare against), and afterwards the count is never zero and this
    // term has no further effect, at the cost of one integer comparison.
    //
    // An equivalent alternative is to initialize ui32LastTick to
    // (uint32_t)(0 - BUTTON_DEBOUNCE_TICKS) so the first difference is naturally
    // >= 50, and drop the count test. The behaviour is the same, but reading it
    // requires working through unsigned wraparound first, so the plainer version
    // was chosen; ui32PressCount has another use anyway -- led_handler()'s log
    // prints press #N, which makes it easy to confirm against the real number of
    // presses that bounce really is being filtered out.
    //
    if ( (psBtn->ui32PressCount != 0) &&
         ((ui32Now - psBtn->ui32LastTick) < BUTTON_DEBOUNCE_TICKS) )
    {
        return;
    }

    psBtn->ui32LastTick = ui32Now;
    psBtn->ui32PressCount++;

    //
    // Press to light, press again to extinguish. Each button owns only its own LED; the other is unaffected.
    //
    g_pbLedOnFlags[psBtn->eLed] = !g_pbLedOnFlags[psBtn->eLed];

    //
    // BTN1 (SW3) also doubles as the Home key: whichever page is showing, pressing it returns home.
    //
    // This button originally only toggled LED4, and that behaviour is kept -- it
    // is the fastest way to confirm the button really reaches the interrupt;
    // without it, a day when "home" stops working gives no way to tell a button
    // that did not fire from a UI that did not handle it.
    //
    // Why a physical key is needed: horizontal swipes on the map page pan the map
    // rather than change pages, and the only way out is dragging the map to the
    // edge of the world, which takes five or six swipes from mid-route.
    //
    // This only raises a flag; the actual page change happens on RenderTask's next frame (see RenderTaskGoHome()).
    // Tested by pin rather than by eLed: which LED a button lights and which
    // button it is are two different things, and rewiring later should not drag
    // the identity of the Home key along with it.
    //
    if ( psBtn->ui32Pin == AM_BSP_GPIO_BUTTON1 )
    {
        RenderTaskGoHome();
    }

    //
    // BTN0 (SW1) also doubles as the page overview key: one press shrinks the
    // current page into the overview, another zooms back. Swiping left and right
    // in the overview jumps between pages (see RenderTaskTogglePageOverview()).
    //
    // Exactly like BTN1: the LED2 toggle is kept and this only adds one more
    // flag, with the actual open and close on RenderTask's next frame -- the
    // thumbnail buffer and the animation state belong to that task.
    //
    // Tested by pin rather than by eLed as well; see the note above.
    //
    if ( psBtn->ui32Pin == AM_BSP_GPIO_BUTTON0 )
    {
        RenderTaskTogglePageOverview();
    }

    //
    // Send the event back to LedTask. The GPIO IRQ priority is
    // AM_IRQ_PRIORITY_DEFAULT (4); being numerically above
    // configMAX_SYSCALL_INTERRUPT_PRIORITY (3) means lower priority, which makes
    // calling the FreeRTOS FromISR API legal.
    //
    vTaskNotifyGiveFromISR(LedTaskHandle, &xHigherPriorityTaskWoken);

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

//*****************************************************************************
//
// This module deliberately does NOT define am_gpio0_203f_isr().
//
// The display driver already has one and it expands to the same symbol, so
// writing another here gives a multiple definition link error:
//
//   disp_te_isr  = am_gpio0_203f_isr
//                  (Display TE = GPIO33 -> 33/32 = 1 -> GPIO0_203F_IRQn)
//   this module  = am_gpio0_203f_isr
//                  (BTN0       = GPIO46 -> 46/32 = 1 -> GPIO0_203F_IRQn)
//
// This is exactly the same situation as GPIO0_607F_IRQn in radio_task.c (where
// the EM9305's GPIO117 collides with touch's GPIO98), and it is handled the same
// way: reuse the existing one.
//
// Reuse works because disp_te_isr() in am_devices_display_generic.c does exactly
// irq_status_get -> irq_clear -> interrupt_service, and
// am_hal_gpio_interrupt_service() dispatches by the handler registered for each
// pin. It reads the status of the whole group (GPIO 32-63), so the GP46 bit is
// covered too and Button_callback() is still called; the TE handler is unaffected.
//
// A side effect: even when g_sDispCfg.eTEType is not DISP_TE_GPIO (in which case
// the display driver does not touch this IRQ line's NVIC), led_task_hw_init()
// below still enables it, so the buttons do not depend on the TE configuration.
//
//*****************************************************************************

//*****************************************************************************
//
//! @brief Interrupt handler for GPIO0 pins 0-31 (used by BTN1 / GP29).
//!
//! @details The opposite of the GP46 case: this IRQ line has no other user in
//! this project, and startup_gcc.c only makes am_gpio0_001f_isr a weak alias to
//! am_default_isr, so one has to be defined here or GP29's interrupt is consumed
//! by the default handler -- no response to a press, and no error message or
//! crash either, the kind that costs the most time to find.
//!
//! The body is Ambiq's standard three steps: read the status, clear it, dispatch
//! by pin. Clearing must precede dispatch, or a new edge arriving while a
//! callback runs would be cleared along with the old status.
//
//*****************************************************************************
void
am_gpio0_001f_isr(void)
{
    uint32_t ui32IntStatus;

    am_hal_gpio_interrupt_irq_status_get(GPIO0_001F_IRQn, true, &ui32IntStatus);
    am_hal_gpio_interrupt_irq_clear(GPIO0_001F_IRQn, ui32IntStatus);
    am_hal_gpio_interrupt_service(GPIO0_001F_IRQn, ui32IntStatus);
}

//*****************************************************************************
//
//! @brief Look up the cumulative press count of the button mapped to one LED.
//!
//! @details For the log only. A lookup is used deliberately instead of indexing
//! g_psButtons by the LED index: the two tables happen to be in the same order
//! today (BTN0->LED2, BTN1->LED4), but that is coincidence, and inserting a row
//! in the middle later would misalign them -- a misalignment that only shows as
//! wrong numbers in the log and is very hard to notice.
//!
//! @param eLed Target LED
//!
//! @return The press count of the mapped button; 0 when that LED has no button.
//
//*****************************************************************************
static uint32_t
button_press_count(led_id_e eLed)
{
    for ( uint32_t ui32Idx = 0; ui32Idx < LED_TASK_BUTTON_COUNT; ui32Idx++ )
    {
        if ( g_psButtons[ui32Idx].eLed == eLed )
        {
            return g_psButtons[ui32Idx].ui32PressCount;
        }
    }

    return 0;
}

//*****************************************************************************
//
//! @brief Drive both LEDs from the current flags.
//!
//! @details Runs in LedTask context. GPIO is written and a log line emitted only
//! when the state actually changes, so the number of log lines equals the number
//! of presses and can be matched against the press numbers directly.
//
//*****************************************************************************
static void
led_handler(void)
{
    //
    // The state last applied to the hardware. The initial value must match what
    // led_task_hw_init() wrote (all off), or the first call would decide "no
    // change" and skip everything.
    //
    static bool pbLedApplied[LED_ID_COUNT] = { false, false };

    static const char * const pcLedNames[LED_ID_COUNT] = { "LED2", "LED4" };

    for ( uint32_t ui32Idx = 0; ui32Idx < LED_ID_COUNT; ui32Idx++ )
    {
        bool bWanted = g_pbLedOnFlags[ui32Idx];

        if ( bWanted == pbLedApplied[ui32Idx] )
        {
            continue;
        }

        am_hal_gpio_state_write(g_pui32LedPins[ui32Idx],
                                bWanted ? LED_TASK_LED_ON_STATE
                                        : LED_TASK_LED_OFF_STATE);
        pbLedApplied[ui32Idx] = bWanted;

        am_util_debug_printf("[led_task] %s(GP%u) %s (press #%u)\n",
                             pcLedNames[ui32Idx],
                             (unsigned)g_pui32LedPins[ui32Idx],
                             bWanted ? "ON" : "OFF",
                             (unsigned)button_press_count((led_id_e)ui32Idx));
    }
}

//*****************************************************************************
//
//! @brief Configure LED2 and BTN0, and register the button interrupt.
//!
//! @details Deliberately called inside LedTask() rather than in setup_task: an
//! interrupt can arrive the moment it is enabled, and Button_callback() needs a
//! valid LedTaskHandle. Enabling only once the task really runs leaves no gap.
//
//*****************************************************************************
static void
led_task_hw_init(void)
{

    //
    // Both LEDs: apply their respective BSP open-drain configuration and make
    // sure they start off, so there is no flash at boot. The order matches the
    // initial values of g_pbLedOnFlags / pbLedApplied (all off).
    //
    for ( uint32_t ui32Idx = 0; ui32Idx < LED_ID_COUNT; ui32Idx++ )
    {
        am_hal_gpio_pinconfig(g_pui32LedPins[ui32Idx], *g_ppsLedCfgs[ui32Idx]);
        am_hal_gpio_state_write(g_pui32LedPins[ui32Idx], LED_TASK_LED_OFF_STATE);
    }

    //
    // Configure the two buttons one by one.
    //
    for ( uint32_t ui32Idx = 0; ui32Idx < LED_TASK_BUTTON_COUNT; ui32Idx++ )
    {
        button_map_t        *psBtn      = &g_psButtons[ui32Idx];
        am_hal_gpio_pincfg_t sButtonCfg = *psBtn->psCfg;
        uint32_t             ui32IntPin = psBtn->ui32Pin;

        //
        // The BSP already configures both buttons as input + 100K pull-up +
        // INTDIR_HI2LO. eIntDir is still written explicitly here, because
        // "falling edge triggered" is a requirement of this feature and should
        // not merely rely on the BSP happening to agree -- the button conducts to
        // ground and the level falls from the pull-up's high to low, which is HI2LO.
        //
        sButtonCfg.GP.cfg_b.eIntDir = AM_HAL_GPIO_PIN_INTDIR_HI2LO;
        am_hal_gpio_pinconfig(psBtn->ui32Pin, sButtonCfg);

        //
        // Event registration: hook Button_callback() to channel 0 of this pin,
        // with pArg carrying that button's own row, so the callback knows which
        // LED to toggle and which debounce state to use.
        //
        am_hal_gpio_interrupt_register(AM_HAL_GPIO_INT_CHANNEL_0,
                                       psBtn->ui32Pin,
                                       (am_hal_gpio_handler_t)Button_callback,
                                       psBtn);

        //
        // Clear any stray flag left by configuring the pin before enabling it, or
        // a spurious interrupt would arrive before the first real press.
        //
        am_hal_gpio_interrupt_irq_clear(psBtn->eIrq,
                                        (1UL << (psBtn->ui32Pin % 32)));

        am_hal_gpio_interrupt_control(AM_HAL_GPIO_INT_CHANNEL_0,
                                      AM_HAL_GPIO_INT_CTRL_INDV_ENABLE,
                                      (void *)&ui32IntPin);

        //
        // Enable only this IRQ line, leaving other existing GPIO interrupts alone.
        //
        // The GP46 line is shared with the display's TE, and when
        // eTEType == DISP_TE_GPIO the display driver sets 0x4
        // (= AM_IRQ_PRIORITY_DEFAULT) as well, so setting the same value again
        // does not change TE behaviour; conversely, if TE does not use GPIO, these
        // two lines are the only place it is enabled. The GP29 line has no other
        // user in this project, and this is its only configuration point.
        //
        NVIC_SetPriority(psBtn->eIrq, AM_IRQ_PRIORITY_DEFAULT);
        NVIC_EnableIRQ(psBtn->eIrq);

        am_util_debug_printf("[led_task] %s falling-edge int on -> %s(GP%u)\n",
                             psBtn->pcName,
                             (psBtn->eLed == LED_ID_LED2) ? "LED2" : "LED4",
                             (unsigned)g_pui32LedPins[psBtn->eLed]);
    }
}

//*****************************************************************************
//
//! @brief Set the target state of one LED (task context).
//!
//! @details Only writes the flag and wakes LedTask; GPIO is written centrally by
//! led_handler(). The render task therefore need not know about open-drain
//! polarity and performs no extra hardware access inside the drawing loop.
//!
//! xTaskNotifyGive() rather than the FromISR version: the caller here is the
//! render task. LedTask has the lowest priority in the system, so this notify
//! does not preempt the caller immediately and the LED lights only once the
//! caller yields -- imperceptible as button feedback.
//
//*****************************************************************************
void
LedTaskSetState(led_id_e eLed, bool bOn)
{
    if ( eLed >= LED_ID_COUNT )
    {
        return;
    }

    g_pbLedOnFlags[eLed] = bOn;

    //
    // Before the task exists, only the flag is updated; the first led_handler()
    // after LedTask starts applies the current flags to the hardware, so this
    // setting is not lost.
    //
    if ( LedTaskHandle != NULL )
    {
        xTaskNotifyGive(LedTaskHandle);
    }
}

//*****************************************************************************
//
//! @brief Toggle the target state of one LED (task context).
//
//*****************************************************************************
void
LedTaskToggle(led_id_e eLed)
{
    if ( eLed >= LED_ID_COUNT )
    {
        return;
    }

    LedTaskSetState(eLed, !g_pbLedOnFlags[eLed]);
}

//*****************************************************************************
//
//! @brief Query the current target state of one LED.
//
//*****************************************************************************
bool
LedTaskGetState(led_id_e eLed)
{
    if ( eLed >= LED_ID_COUNT )
    {
        return false;
    }

    return g_pbLedOnFlags[eLed];
}

//*****************************************************************************
//
//! @brief Query the on flag of LED4 (GP83). Legacy interface.
//
//*****************************************************************************
bool
LedTaskGetFlag(void)
{
    return g_pbLedOnFlags[LED_ID_LED4];
}

//*****************************************************************************
//
//! @brief Button LED task.
//
//*****************************************************************************
void
LedTask(void *pvParameters)
{
    (void)pvParameters;

    led_task_hw_init();

    //
    // Apply the flags' initial state to the LEDs at boot (equivalent to off), so
    // led_handler()'s internal state matches the hardware.
    //
    led_handler();

    while (1)
    {
        //
        // Block waiting for a notification, which may come from
        // Button_callback() (ISR) or LedTaskSetState() (the render task's control
        // centre). pdTRUE clears the count on take, so several notifications in a
        // row produce just one led_handler() call -- which is what is wanted: the
        // flags are state, not a queue, and led_handler() scans every LED and
        // brings them all up to date at once.
        //
        // portMAX_DELAY blocks completely, so with no button presses this task
        // uses no CPU and IDLE can still enter deepsleep; the GPIO interrupt is
        // itself the wake-up source.
        //
        if ( ulTaskNotifyTake(pdTRUE, portMAX_DELAY) > 0 )
        {
            led_handler();
        }
    }
}
