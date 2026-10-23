# A missed GPIO117 edge stalls HCI initialization for 10 seconds

**Platform** apollo510b_evb (AP510BFA-CBR, EM9305 in-package) · AmbiqSuite 5.2.0
· Cordio BLE host · FreeRTOS
**Status** Root cause of the stall identified and worked around in the application;
the reason the edge is lost is still open.
**Scope** Everything described here lives in the example's own `src/` and
`gcc/Makefile`. **No file under `third_party/` or `devices/` was modified.**

Traditional Chinese version: [BLE_HCI_MISSED_INTERRUPT.zh-TW.md](BLE_HCI_MISSED_INTERRUPT.zh-TW.md)

---

## 1. Summary

Boot-to-advertising time on this board was not constant. It was either ~1.3 s,
or ~11.3 s, or ~21.3 s — a quantized ladder, 10 seconds per step.

Each step is one `HCI_CMD_TIMEOUT` (10 s, `hci_cmd.c`). The timeout fires
because the EM9305 raised `SPI_RDY` (GPIO117) with a Command Complete waiting,
and the host missed that **rising edge**. GPIO117 is configured
`AM_HAL_GPIO_PIN_INTDIR_LO2HI`, and Apollo5 GPIO has **no level-triggered
mode**, so once the transition is missed the hardware never offers a second
chance. Nothing in the driver polls the pin on its own, so the HCI command
sequence stops until Cordio's timeout tears the controller down and restarts
the whole initialization.

Our workaround detects the stalled state from the render loop and replays the
WSF event the lost interrupt would have posted. Worst-case recovery goes from
10 s to 20–40 ms, and the controller is not restarted — the stuck packet is read
and the sequence continues from where it stopped.

**What we would like Ambiq to consider:** `hci_drv_em9305.c` already has a
partial workaround that posts another event after four packets, with a comment
noting that an interrupt may be absent despite a waiting packet (see §3.4).
However, it has no ongoing check independent of the handler. If an edge is lost
while the handler is not scheduled and no transfer event is pending, recovery
may still depend on the 10 s timeout, which discards all HCI/DM state. The
detection is cheap (one GPIO read plus two comparisons) and would be better
placed in the driver than in each application.

---

## 2. The log that identified it

This is the evidence, captured over RTT with the diagnostics described in §6:

```
[irqdiag] em9305 irq=13 pin=1 nvic=1
[irqdiag] em9305 irq=13 pin=1 nvic=1
[irqdiag] em9305 irq=13 pin=1 nvic=1
[bledbg] t=11048 ms  *** HCI CMD TIMEOUT op=0x0C01 irq=13 pin=1 ***
```

The `[irqdiag]` line is printed once per second by SensorTask
(`src/sensor_task.c:324`). Three consecutive seconds, every field frozen:

| Field | Value | Meaning |
|---|---|---|
| `irq` | `13`, not moving | `g_ui32InterruptsSeen` in `hci_drv_em9305.c`. It is incremented **inside `HciDrvIntService()`**, so a frozen value means the ISR was never entered — not that it ran and did nothing. |
| `pin` | `1` | `am_hal_gpio_input_read(AM_BSP_GPIO_EM9305_INT)`. High means the EM9305 has data waiting for the host. It is still asserted. |
| `nvic` | `1` | `GPIO0_607F_IRQn` is still enabled in the NVIC. The interrupt was not masked off. |

Read together: **the controller is asserting "I have data", the NVIC is enabled,
and the ISR is not running.** That narrows the fault to the path between the pin
transition and `HciDrvIntService()`, and rules out WSF event dispatch and the
host stack above it.

The fourth line is from our `--wrap` hook on `hciCmdTimeout()`:

| Field | Value | Meaning |
|---|---|---|
| `t` | `11048 ms` | Time since boot. The command went out at t≈1048 ms, so this is exactly 10 s later. |
| `op` | `0x0C01` | `HCI_Set_Event_Mask` — the command whose Command Complete is stuck in the EM9305. |
| `irq` / `pin` | `13` / `1` | Unchanged at the moment of timeout, confirming the state persisted for the full 10 s. |

The relationship to the observed boot times is direct: 0 missed edges → 1.3 s,
1 missed edge → 11.3 s, 2 missed edges → 21.3 s.

> **Note on the 1.3 s baseline.** It is not a defect and should not be confused
> with this problem. About 1.04 s of it is the EM9305's own reset time after
> `HCI_Reset` (`tx cmd op=0x0C03` at t=2 ms, `cmd cmpl` at t=1040 ms).

---

## 3. Why one missed edge cannot recover

### 3.1 The pin is edge-triggered, and Apollo5 has no level mode

GPIO117 is configured by `am_devices_em9305_config_pins()`
(`am_devices_em9305.c:280`) with the generic `am_hal_gpio_pincfg_input`:

| Field | Value |
|---|---|
| `eGPInput` | `INPUT_ENABLE` |
| `eGPOutCfg` | `OUTCFG_DISABLE` |
| **`eIntDir`** | **`AM_HAL_GPIO_PIN_INTDIR_LO2HI`** ← rising edge only |
| `ePullup` | `PULLUP_NONE` |

`AM_HAL_GPIO_PIN_INTDIR_*` offers only `NONE` / `HI2LO` / `LO2HI` / `BOTH`
(`am_hal_gpio.h:320-323`). There is no level-triggered option, so "the pin is
currently high" cannot by itself generate an interrupt. Switching to `BOTH`
does not help either — it adds falling edges, which says nothing about a line
that is already high and staying high.

> Worth flagging separately: the BSP does define `g_AM_BSP_GPIO_EM9305_INT`
> (`am_bsp_pins.c:4292`) with `AM_HAL_GPIO_PIN_INTDIR_HI2LO`, the **opposite**
> polarity. That structure is not used by `am_devices_em9305_config_pins()`.
> It is dead configuration that points the wrong way, and it cost us time while
> establishing the real polarity.

### 3.2 The driver's own GPIO polling cannot run

`HciDrvHandler()` does read the pin:

```c
/* hci_drv_em9305.c:852 */
if ( am_hal_gpio_input_read(GPIO_INT) )
{
    am_devices_em9305_block_read(...);
    /* line 884: wait for it to go low again, so the same packet is not read twice */
}
```

But this is not background polling — it only runs when the handler is scheduled.
With `USE_NONBLOCKING_HCI = 0` (the `hciDrvReadCallback()` path compiled out),
the two main external sources that schedule transfer processing are:

| Source | Location |
|---|---|
| `HciDrvIntService()` → `WsfSetEvent()` | `hci_drv_em9305.c:570` |
| `hciDrvWrite()` → `WsfSetEvent()` | `hci_drv_em9305.c:519` |

The handler can also post its own transfer event when the stack has not consumed
all received data, when a transmit retry limit is reached, or after four packets
(see §3.4). These paths all require the handler to be running first; they are
not independent background checks.

In the stalled case discussed here, **neither external source fires and no
transfer event is pending**: the interrupt never fired, and Cordio will not send
the next command while waiting for the previous one's completion. The GPIO
check inside the handler therefore has no opportunity to run.

(The 10 s heartbeat also wakes the handler, but that branch sends
`HciReadLocalVerInfoCmd()` and returns before reaching the transfer loop.)

### 3.3 This single pin serves two different models

This is the heart of the design issue:

| Model | Used by | Purpose |
|---|---|---|
| **Level polling** | `am_devices_em9305_block_read/write()`, reset handshake, firmware download | Synchronous "is it ready yet", via `WHILE_TIMEOUT_MS` on the pin level |
| **Rising-edge interrupt** | `HciDrvIntService()` | Asynchronous "a packet arrived", wakes RadioTask |

The two complement each other in normal operation. But **if an edge is lost
and no transfer event is pending, the driver has no independent background
mechanism to re-check the level**. The code that reads the level must first be
reached through an interrupt, a transmit request, or a previously posted event.
That is the gap.

### 3.4 The existing vendor workaround: reschedule after four packets

The blocking path in `hci_drv_em9305.c` already contains this code:

```c
/* line 105 */
#define HCI_DRV_MAX_READ_PACKET          4   // max read in a row at a time

/* lines 931–938, inside HciDrvHandler() */
if (read_hci_packet_count >= HCI_DRV_MAX_READ_PACKET)
{
    // It looks like there's time that we won't get interrupt even though
    // there's packet waiting for host to grab.
    WsfSetEvent(g_HciDrvHandleID, BLE_TRANSFER_NEEDED_EVENT);

    break;
}
```

The original comment explicitly records the observation that a packet may be
waiting without an interrupt, showing that the vendor added a partial workaround
for this class of symptom. It does not establish the same root cause as this
report, or identify whether the cause lies in the EM9305, MCU GPIO interrupts,
or the timing between them.

`read_hci_packet_count` resets on every handler entry. It increments only after
a successful packet read whose data has been fully consumed by the stack
(line 913). Once four packets have been processed in this invocation, the code
posts another transfer event and uses `break` to leave the current transfer
loop. `WsfSetEvent()` sets a software event for a later WSF dispatch; it neither
calls the handler recursively nor generates another hardware interrupt. The next
handler invocation checks GPIO117 again, so it has an opportunity to read waiting
data even without another rising edge.

**This schedules a follow-up check before ending the current batch; it does not
detect every missed interrupt.** Its trigger is four fully processed packets in
the current invocation, not a frozen IRQ count or confirmation of another packet.

| Scenario | Coverage of the four-packet rescheduling branch |
|---|---|
| Four packets processed, then more data waits without a new interrupt | Another event is posted; the next invocation can continue reading if INT is high |
| Only one to three packets processed before exit, with no other event pending | This threshold branch is not reached, so it cannot guarantee another check |
| The first interrupt is missed and the handler never runs | This workaround has no opportunity to execute |
| The posted event has already been serviced before a later edge is missed | The earlier rescheduling provides no ongoing protection |

`ble_diag_int_kick()` (§5) adds a different trigger: it repeatedly checks the
INT level and IRQ count from outside the handler and posts a transfer event when
the stalled-state conditions hold. It therefore complements the vendor's
four-packet rescheduling mechanism. Both keep actual SPI transfers in RadioTask.

---

## 4. What Cordio does when the timeout expires

```c
/* hci_cmd.c */
void hciCmdTimeout(wsfMsgHdr_t *pMsg)
{
    HciDrvRadioShutdown();
    HciDrvRadioBoot(0);
    DmDevReset();
}
```

The response to a missed packet is a full controller restart: all HCI/DM state
is discarded, `g_ui32InterruptsSeen` resets to zero, and roughly 60 HCI commands
are re-issued. The packet that was stuck disappears with the restart rather than
being read.

It also **does not raise `DM_HW_ERROR_IND`**. The application sees no error at
all — only `[ble] reset cmpl` appearing inexplicably at 11 s or 21 s. Without
instrumentation there is nothing to connect that to an interrupt problem, which
is why this took as long as it did to characterize.

Shortening `HCI_CMD_TIMEOUT` does not help: it would reach
`shutdown + boot + DmDevReset` sooner, restart the controller more often, and
still never read the waiting packet.

---

## 5. The workaround

`ble_diag_int_kick()` (`src/ble_diag.c`) is called once per frame — about every
18 ms — from the render loop (`src/render_task.c:8907`):

```c
while (1)
{
    xSemaphoreTake(g_semDisplayEnd, portMAX_DELAY);

    ble_diag_int_kick();
    ...
}
```

It fires when all three hold:

1. `am_hal_gpio_input_read(AM_BSP_GPIO_EM9305_INT)` is high;
2. `g_ui32InterruptsSeen` has not changed since the previous frame;
3. that has been true for at least `BLE_DIAG_KICK_STUCK_MS` (20 ms,
   `ble_diag.c:377`).

The action is a single line:

```c
/* BLE_TRANSFER_NEEDED_EVENT = 0x01, defined in hci_drv_em9305.c, not exported */
WsfSetEvent(g_HciDrvDiagHandlerId, 0x01);
```

Repeat kicks are limited to one per `BLE_DIAG_KICK_HOLDOFF_MS` (100 ms,
`ble_diag.c:384`) so a genuinely unresponsive controller does not flood the log.

**20 ms threshold:** a normal HCI round trip measures 1–2 ms in our logs, so
20 ms is well clear of any transfer in progress.

**Why `WsfSetEvent()` and not a direct `HciDrvHandler()` call.** The check runs
in RenderTask. Calling the HCI transport from there would move it out of
RadioTask's context and interleave with the event sequence RadioTask owns.
Posting the event keeps the existing architecture intact — RenderTask only
detects, RadioTask still does all transport work:

```
RenderTask          detects "INT high, irq frozen"
     │
     └─ WsfSetEvent(BLE_TRANSFER_NEEDED_EVENT)
            │
            └─ RadioTask → HciDrvHandler() → reads the pin high
                   → am_devices_em9305_block_read()
                   → hciTrSerialRxIncoming() → hciCmdRecvCmpl()
                   → WsfTimerStop(cmdTimer)      ← the 10 s timeout is cancelled
                   → next HCI command proceeds
```

The handler ID is captured at registration (`src/radio_task.c:270`) through an
explicit setter rather than an `extern` of `g_HciDrvHandleID`, to avoid depending
on "0 means uninitialized".

### 5.1 Results

| Case | Before | After |
|---|---|---|
| No missed edge | 1.3 s | 1.3 s (identical) |
| 1 missed edge | **11.3 s** | ~1.35 s |
| 2 missed edges | **21.3 s** | ~1.4 s |
| Cost per miss | +10 s, controller restart, sequence replayed | +20–40 ms, sequence continues |
| Controller state | discarded and rebuilt | untouched |
| Stuck packet | lost in the restart | read normally |

Runtime cost: one GPIO read and two comparisons per frame.

---

## 6. Instrumentation, with the SDK untouched

The hook points needed are all inside `third_party/cordio`. Rather than edit
them, we used GNU ld's `--wrap`, which rewrites only **cross-translation-unit**
calls (`gcc/Makefile:713-717`):

| Wrapped symbol | Defined in | Called from | Records |
|---|---|---|---|
| `hciTrSendCmd` | `hci_tr.c` | `hci_cmd.c` | when a command reached the transport, and its opcode |
| `hciTrSerialRxIncoming` | `hci_tr.c` | `hci_drv_em9305.c` | when bytes were handed to the host stack |
| `hciCmdRecvCmpl` | `hci_cmd.c` | `hci_evt.c` | Command Complete / Status |
| `hciCmdTimeout` | `hci_cmd.c` | `hci_core_ps.c` | **the line that settled this**: timeout and stuck opcode |
| `HciDrvRadioBoot` | `hci_drv_em9305.c` | `radio_task.c`, `hci_cmd.c` | controller reboots, cold vs warm |

All five have their definition and their call sites in different `.c` files,
which is what `--wrap` requires. Interception can be confirmed in the image
rather than assumed:

```
$ arm-none-eabi-objdump -d bin/nemagfx_watchface.axf | grep -A1 __wrap_hciCmdTimeout
4288bc: bl 41a954 <__wrap_hciCmdTimeout>     <- the hci_core_ps.c call now goes to the wrapper
41a972: bl 426de8 <hciCmdTimeout>            <- the wrapper forwards to the original
```

Records are written to RAM only (256 entries × 12 bytes = 3 KB `.bss`) and
drained once per second by SensorTask, the lowest-priority task.

> **One trap worth repeating.** Our first measurement lost the entire record
> between t=1041 ms and t=11048 ms, leaving truncated half-lines in the log.
> `am_util_stdio_printf()` formats into a single **unlocked** static buffer
> (`g_prfbuf`), so two tasks printing concurrently overwrite each other. The
> diagnostics now format into their own buffer with
> `am_util_stdio_vsprintf()` and hand the result to `SEGGER_RTT_WriteString()`,
> which has its own lock. Anything that logs from both RadioTask and a lower
> priority task will hit this.

---

## 7. What we could not determine

**Why the rising edge does not become an ISR is still open.** The workaround is
a recovery mechanism, not a fix for the cause.

**It cannot be probed.** GPIO117 has no package ball. `AP510BFA-CBR` is a
153-pin BGA exposing 96 GPIOs out of the die's 224 pads, and 117 is not among
them — the EM9305 is in-package (`AM_BSP_IS_SIP` in the board's `am_bsp.h:91`),
so IOM6, CS, SPI_RDY, EN and the clocks are all die-to-die connections. Only RF
and power reach balls. A logic analyser cannot be attached to this signal at
all, which also makes signal integrity an unlikely explanation — there is no PCB
trace, connector or board-level noise involved.

The substitute is the MCU's own raw interrupt status latch, sampled on every
kick and reported in the log:

```c
am_hal_gpio_interrupt_irq_status_get(AM_BSP_EM9305_RADIO_INT_IRQ, false, &ui32Stat);
ui32Latch = (ui32Stat >> (AM_BSP_GPIO_EM9305_INT & 0x1FU)) & 1U;   /* pin 117 -> bit 21 */
```

| `latch` | Interpretation | Where to look |
|---|---|---|
| `1` with `irq` frozen | The edge **was** latched but never became `HciDrvIntService()` | Software: the shared `am_gpio0_607f_isr` and its read → clear → service ordering |
| `0` with `pin=1` | The edge was never latched, or was cleared | Edge detection / hardware side |

We checked one hypothesis and can report it as unlikely:
`am_hal_gpio_interrupt_irq_clear(irq, mask)` writes the mask to `MCUN0INTxCLR`
(write-1-to-clear, only the bits passed in), so an edge arriving *after* a
`status_get` is not wiped by that call — its bit stays set and the IRQ re-fires.
That weakens the "read-clear race in the shared ISR" theory and makes `latch=0`
the more likely observation.

**One platform note that may matter to Ambiq.** On this board GPIO98 (touch
panel INT) and GPIO117 (EM9305) both map to `GPIO0_607F_IRQn`. Two different
SDK components expand to the same ISR symbol, so an application using both must
define neither and let `am_hal_gpio_interrupt_service()` dispatch per pin. Our
example documents this in `src/radio_task.c`; it is not obvious from the SDK and
produces a multiple-definition link error when it is first hit.

---

## 8. Reproducing it

The workaround can be disabled without removing the diagnostics, which is the
cleanest way to see the original behaviour:

| Switch | File | Default | Effect |
|---|---|---|---|
| `WATCHFACE_BLE_INT_KICK` | `src/ble_diag.h:85` | `1` | Set to `0` to observe without intervening — the 10 s delay returns |
| `WATCHFACE_BLE_DIAG` | `src/ble_diag.h:62` | `1` | Master switch. Setting `0` also requires commenting out the five `--wrap` lines in `gcc/Makefile`, or the linker reports `__wrap_*` as undefined |
| `WATCHFACE_BLE_IRQ_DIAG` | `src/sensor_task.c` | `1` | The per-second `[irqdiag]` snapshot |

The delay is probabilistic — **boot repeatedly**. A single clean boot proves
nothing. What to look for:

| Observation | Meaning |
|---|---|
| `*** INT stuck high N ms, irq frozen at M, latch=L -> kick ***` | The workaround caught one; this boot would otherwise have taken 10 s longer |
| No more `*** HCI CMD TIMEOUT ***` | The command timeout is gone |
| `[ble] reset cmpl` ≈1.1 s, `start advertising` ≈1.3 s | Normal |
| `HCI CMD TIMEOUT` still appearing | The kick did not recover it — look at `am_devices_em9305_block_read()`'s return value and the SPI direction |
| `INT stuck high` every second | Not an occasional miss; worth investigating the shared `am_gpio0_607f_isr` |

---

## 9. Files

| File | Role |
|---|---|
| `src/ble_diag.h` | Switches, event tags, interface |
| `src/ble_diag.c` | Event ring buffer, `--wrap` hooks, stuck-INT detection and kick |
| `src/render_task.c:8907` | Calls `ble_diag_int_kick()` once per frame |
| `src/radio_task.c:260,270` | Registers the diagnostic handler wrapper and records its ID |
| `src/sensor_task.c:324` | Drains the ring buffer and prints `[irqdiag]` |
| `gcc/Makefile:713-717` | The five `--wrap` link flags |

Relevant SDK sources, unmodified: `hci_drv_em9305.c` (`HciDrvIntService`,
`HciDrvHandler`), `hci_cmd.c` (`HCI_CMD_TIMEOUT`, `hciCmdTimeout`,
`hciCmdRecvCmpl`), `am_devices_em9305.c` (`config_pins`, `block_read`).
