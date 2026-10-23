# nemagfx_watchface — Apollo510B EVB

[![Watch the Apollo510B Smart Watch Demo on YouTube](https://i.ytimg.com/vi/N_bFMPd-4I8/maxresdefault.jpg)](https://youtu.be/N_bFMPd-4I8)

[Watch the Apollo510B Smart Watch Demo on YouTube](https://youtu.be/N_bFMPd-4I8).
The video is hosted externally on YouTube and is not included in this source repository.

A five-page FreeRTOS smartwatch UI on the apollo510b_evb, built on NemaGFX / NemaVG,
with BLE (Cordio + EM9305), a TMP119 temperature sensor, a CHSC5816 touch panel,
external PSRAM and an eMMC-backed asset store.

It started as the AmbiqSuite `nemagfx_watchface` example and grew into a full
application. The comments throughout the source record **why** each decision was
made — including several SDK behaviours that cost real debugging time — so the
code is intended to be read, not only compiled.

---

## 1. Hardware

| Block | Part | Interface |
|---|---|---|
| MCU | Apollo510B (AP510NFA-CBR) | — |
| Display | CO5300 AMOLED, 468x468 round | MIPI DSI |
| Touch | CHSC5816 | IOM2 (SCL GP25, SDA GP26, INT GP98, RST GP99) |
| Temperature | TI TMP119 (mikroBUS) | IOM1 (SCL GP8, SDA GP9) |
| BLE | EM9305 | IOM6, INT GPIO117 |
| PSRAM | APS51216BA, 64 MB, x16 hex DDR | MSPI0, XIP at 0x60000000 |
| Asset store | IS21EF08G eMMC, 8 GB, FAT | SDIO0 |
| Buttons | BTN0 / GP46 (SW1), BTN1 / GP29 (SW3) | GPIO |
| LEDs | LED2/GP11, LED3/GP19 (BLE), LED4/GP83 | GPIO, open-drain, active low |

> **Note on the shield pinout.** The touch INT/RST are GP98/GP99 on Apollo510B.
> The AP510DISP shield datasheet numbers them 77/78, which is the Apollo510
> (non-B) numbering and does not apply here.

---

## 2. Quick start

### 2.1 Prerequisites

| Tool | Version | Why |
|---|---|---|
| Arm GNU Toolchain | any with `-mcpu=cortex-m55` | Apollo510 is Cortex-M55 |
| MSYS2 `make` | — | the Makefile is GNU make with bash constructs |
| SEGGER J-Link | **V8.40 or newer** | older versions have no MRAM flash loader for AP510NFA-CBR |

`gcc/check_toolchain.sh` verifies all three, including whether the installed
J-Link really supports `AP510NFA-CBR`:

```bash
cd gcc
bash check_toolchain.sh
```

### 2.2 Place the tree

This example needs an AmbiqSuite 5.2.0 checkout, because it builds against the
SDK's sources:

```
AmbiqSuite_5.2.0/
  boards/apollo510b_evb/bsp/
  boards/apollo510b_evb/examples/graphics/nemagfx_watchface/   <- this repo
  third_party/ThinkSi/NemaGFX_SDK/
  third_party/cordio/
  third_party/FatFs/
  mcu/apollo510/
```

The Makefile does **not** hard-code that depth. It searches upward for the SDK
root (`SDK_ROOT`) and derives the BSP from it, so the tree also builds one or two
levels deeper — a clone placed inside another directory, for instance. Point it
somewhere else explicitly if needed:

```bash
make SDK_ROOT=/c/AmbiqSuite_5.2.0
```

If the search fails, the Makefile stops with a message naming the variable to set
rather than failing later as `No rule to make target '.../am_devices_chsc5816_ap5.o'`.

### 2.3 Build and flash

```bash
cd gcc
make -j8                 # -> bin/nemagfx_watchface.bin
make flash               # J-Link, loads to 0x00410000
```

There is also a Tkinter front end, `build_gui.py`, which wraps build / rebuild /
clean / flash, auto-detects the toolchain paths, and embeds an RTT log viewer:

```bash
python build_gui.py      # or double-click launch_build_gui.bat
```

> **A note on `TMP`.** GCC writes its intermediate files to `$TMP`, and MSYS2
> make often passes no `TMP` through at all. GCC then falls back to `C:\Windows\`
> and fails with `Cannot create temporary file in C:\Windows\: Permission denied`
> — which reads like a compile error rather than an environment problem. The
> Makefile now falls back to `$TEMP`, then to the `gcc/` directory itself, so no
> setup is required; setting `TMP` yourself still takes precedence. Note that GCC
> is a native Windows executable, so an MSYS-style path such as `/tmp` does not
> work as a value.

### 2.4 Logging

Logging goes over SEGGER RTT (`AM_USE_RTT_LOG`), not ITM/SWO. The RTT viewer in
`build_gui.py` starts a `JLinkGDBServerCL` and reads channel 0.

Both RTT buffers are enlarged from their defaults (`BUFFER_SIZE_UP/DOWN=4096`)
because the asset upload path shares them; see section 5.

---

## 3. Repository layout

```
src/                     application source
  nemagfx_watchface.c    main(): clocks, display, eMMC, PSRAM, scheduler
  rtos.c                 task creation, priorities, idle hook, boot banner
  render_task.c          all drawing: 5 pages, overlays, watch face selector,
                         page overview  (the largest file, heavily commented)
  display_task.c         DC/DSI wake, frame transfer, brightness apply
  touch.c / touch_task.c CHSC5816 driver wrapper + gesture classification
  map_task.c / .h        map tile cache (eMMC -> PSRAM) + navigation simulation
  sensor_task.c          TMP119 sampling into a cache
  asset_store.c          FAT on eMMC: mount, read, write, list, streaming write
  asset_upload.c         RTT receive path for host-pushed assets
  asset_provision.c      optional first-boot provisioning from built-in arrays
  radio_task.c, hts_main.c, ble_*.c   Cordio BLE: HTS over TMP119, LESC pairing
  ble_diag.c / .h        HCI event ring buffer via GNU ld --wrap, stuck-INT kick
  psram.c, backlight.c, led_task.c, tmp119.c, rtt_log.c
  generated/             the asset headers the build actually needs
  font/                  DejaVuSans vector font for NemaVG

gcc/                     Makefile, linker script, startup, J-Link scripts
tools/                   host-side Python: asset conversion and upload
  png_to_bin.py          PNG -> .BIN (the common exit of every conversion path)
  asset_upload.py        push .BIN into eMMC over RTT (GUI and CLI)
  asset_export.py        export the firmware's C arrays into .BIN
  face2/3/4_prep.py      per-watch-face asset preparation
  map_tile_gen.py        map PNGs -> RGB565 C arrays
  assets_out/            ready-made .BIN files for all five pages
  nemagfx_ui_designer/   a PySide6 UI editor that emits NemaGFX/NemaVG C
build_gui.py             build / flash / RTT GUI
```

---

## 4. Runtime architecture

### 4.1 Tasks

| Task | Priority | Stack (words) | Role |
|---|---|---|---|
| RadioTask | HIGHEST (15) | 2048 | Cordio WSF dispatcher |
| TouchTask | HIGHEST (15) | 512 | GP98 interrupt -> I2C read -> gesture |
| RenderTask | HIGH (14) | 1024 | all drawing, UI state machine |
| DisplayTask | MIDDLE (13) | 1024 | DC/DSI wake, frame transfer, DCS brightness |
| SensorTask | LOW (12) | 512 | TMP119 1 s sampling, diagnostics drain |
| MapTask | LOW (12) | 512 | tile cache + navigation simulation |
| UploadTask | LOWEST (11) | 768 | RTT asset receive |
| LedTask | LOWEST (11) | 512 | button debounce -> GPIO |

TouchTask runs at the same priority as RadioTask deliberately: it has to preempt
RenderTask, which holds the CPU for several ms inside `draw_*()` and
`nema_cl_wait()`. It spends almost all its time blocked on a task notification,
so this costs BLE nothing. The reasoning is in the header of `touch_task.h`.

`configTOTAL_HEAP_SIZE` is 48 KB and `configTIMER_QUEUE_LENGTH` is 16; both were
raised from the SDK example's values for reasons recorded in `FreeRTOSConfig.h`.

### 4.2 Memory

```
MRAM     program and const data, flashed at 0x00410000
TCM      FreeRTOS heap, task stacks
SSRAM    VMEM_SIZE = 0x2A0000 (2.75 MB) NemaGFX pool:
           NEMA_MEM_POOL_FB       framebuffer + page-overview thumbnail
           NEMA_MEM_POOL_ASSETS   textures loaded from eMMC
PSRAM    map tiles, XIP at 0x60000000, sampled directly by the GPU
eMMC     FAT: every asset as one file
```

The GPU on this platform **cannot sample MRAM**, so every texture is copied into
SSRAM first (map tiles go to PSRAM, which the GPU *can* sample over MSPI XIP).
Binding a const array's address straight to the GPU — which works on Apollo4P —
produces a flickering red screen here.

The allocation order in `RenderTask()` is deliberate and was the root cause of
two hardware failures: the framebuffer and the thumbnail buffer are allocated
**before** the optional assets, so a short pool costs a black watch face rather
than a bound-to-NULL GPU and a dead board. The comment at that call site records
both incidents.

### 4.3 Pages

| Page | Content |
|---|---|
| HOME | EdgePilot image with an additive sweeping light on the horizon |
| WATCH | analogue face; swipe down for the face selector (3 faces) |
| TEMP | TMP119 reading, circular gauge, BLE state (5 variants incl. LESC pairing) |
| TREND | clock, date, 60-second temperature curve with a shear projection |
| MAP | Taipei 101 -> Taipei Main Station navigation over PSRAM tiles |

Overlays: brightness panel (swipe up from the bottom edge, every page), control
centre (swipe down from the top edge, home only), quick page menu (swipe right,
home only), page overview (BTN0, every page).

---

## 5. Asset workflow

Assets live on eMMC as raw pixel data with no header, so the dimensions and
format must match `ASSET_*` in `src/asset_store.h` exactly. A mismatch is
reported as `short read` in the boot log and that image is not drawn at all.

The whole chain, with no firmware rebuild at any step:

```
PNG --png_to_bin.py--> .BIN --asset_upload.py--> eMMC --> GPU
```

```bash
# convert a folder of PNGs (names matched to targets, scaled if needed)
python tools/png_to_bin.py --dir my_images/
python tools/png_to_bin.py --targets        # list every target and its spec

# push to the board (JLinkGDBServer must be running — the RTT window of
# build_gui.py is one)
python tools/asset_upload.py --list              # what is on eMMC now
python tools/asset_upload.py tools/assets_out/*  # send
python tools/asset_upload.py HOME.png            # converts, then sends
python tools/asset_upload.py                     # GUI
```

`tools/assets_out/` already contains a complete set for all five pages.

### Flow control is the interesting part

The RTT down buffer is 4 KB, and **when it fills, J-Link discards the excess
rather than stalling the host**. So the firmware prints one `[upload] ack <n>`
line per 1 KB received and the host keeps its unacknowledged window below that;
without it, every file over 4 KB fails with `abort <name>: timeout`. The ack
travels on the up buffer, which is `NO_BLOCK_SKIP` and drops lines when full, so
the firmware also re-reports progress after a 250 ms stall. Both halves are
documented in `src/asset_upload.h` and `tools/asset_upload.py`.

### Build switches (`gcc/Makefile`)

| Define | Default | Meaning |
|---|---|---|
| `WATCHFACE_ASSET_PROVISION` | 0 | 1 also builds the assets into MRAM (+2.16 MB) and writes any missing file at boot. Use once on a blank board. |
| `WATCHFACE_ASSET_FORCE_WRITE` | 0 | 1 overwrites existing files unconditionally — needed when swapping artwork of identical dimensions. Set back to 0 afterwards. |
| `WATCHFACE_ASSET_UPLOAD` | 1 | the RTT receive task. Production firmware may turn it off: it lets anyone with SWD rewrite eMMC. |
| `VMEM_SIZE` | 0x2A0000 | NemaGFX pool in SHARED_SRAM |
| `WSF_TRACE_ENABLED` | off | **keep it off** — see below |
| `SDK_ROOT` | searched | AmbiqSuite root; see 2.2 |
| `BSP_ROOT` | `$(SDK_ROOT)/boards/apollo510b_evb/bsp` | board BSP |

---

## 6. Findings worth a look

These are the places where measured behaviour differed from the documentation or
from a reasonable assumption. Each is commented in full at the call site.

**`am_util_stdio_snprintf()` writes no NUL terminator.** It copies characters and
returns; it is not standard `snprintf`. Two failures came from this: a HUD
distance reading `4.9 kmC -`, and a map tile file name becoming
`MAPTILE0.BIN` plus stack garbage, so `f_open()` returned `FR_NO_FILE` and the
map took seconds to appear "sometimes". Every call site on the map page memsets
first. `src/map_task.c`, `src/render_task.c`.

**`WSF_TRACE_ENABLED` costs 10 seconds of boot time.** With it on, DM reset
completion moves from t=1062 ms to t=11067 ms. Cordio emits a great deal of trace
during reset, `am_util_stdio_printf()` formats every line before writing, and
RadioTask does not yield during printf. `gcc/Makefile`.

**A missed GPIO edge has no second chance.** Apollo5 GPIO interrupts are
edge-triggered only, with no level mode. The EM9305 INT (GPIO117, rising edge
only) missing one transition means waiting out the 10 s `hciCmdTimeout()` — the
source of the "sometimes it advertises at boot, sometimes after 10 or 20 seconds"
ladder. `ble_diag.c` detects "INT high while the interrupt count is frozen" from
the render loop and re-posts the event, turning 10 s into tens of ms. The same
reasoning drives the two timeouts in TouchTask. `src/ble_diag.h`, `src/touch_task.h`.

> Written up in full, with the RTT capture that identified it:
> **[docs/BLE_HCI_MISSED_INTERRUPT.md](docs/BLE_HCI_MISSED_INTERRUPT.md)**
> (also in [Traditional Chinese](docs/BLE_HCI_MISSED_INTERRUPT.zh-TW.md))

**HCI instrumentation with zero SDK edits.** The `[bledbg]` ring buffer hooks five
Cordio symbols through GNU ld's `--wrap` (`gcc/Makefile` LFLAGS), so not one line
under `third_party/` changes. The hooks write RAM only and SensorTask drains them,
because `am_util_stdio_printf()` shares one unlocked static buffer and formatting
inside RadioTask both corrupts other tasks' output and stretches the very timing
being measured. `src/ble_diag.c`.

**eMMC bus configuration is tried, not assumed.** The SDK default
(1.8 V / SDR50 / 48 MHz / x8) makes `am_hal_card_cfg_set()` fail on this board
while the card is demonstrably fine. `AM_HAL_CARD_DEBUG` would say why, but the
HAL ships pre-built as `libam_hal.a` and those messages are not compiled in. So
the firmware walks a table and keeps the first combination that works; the
verified one is `1.8V legacy 24M x8`. Also: VCCQ/VCC sit behind load switches on
GPIO220/221 and RSTn's pull-up (R46) is not fitted, so both have to be driven by
firmware. `src/asset_store.c`.

**`nema_texture_size()` is not `w * h * bpp`.** Estimating with the latter
undercounts, and the GPU pool is a `NOLOAD` array, so too small a value produces
no link error at all and only faults at runtime. This is why the fourth watch
face replaced the second rather than being added. `gcc/Makefile` VMEM_SIZE.

**`nema_vg_string_get_bbox()` clamps to its `max_w` argument.** Measuring with the
widget's own box width therefore makes "this line is too wide" undetectable, and
the automatic shrink silently never runs. The UI Designer hit this and the
regression test for it is in `tools/nemagfx_ui_designer/tests/`.

**NemaVG treats the font size as points.** Three independent measurements on
hardware give the same denominator, `1536 = units_per_em * 3/4` — the 72/96
point-to-pixel relationship at 96 DPI. This is consistent with
`nema_vg_get_ascender_pt()` returning point units but is not stated for
`nema_vg_set_font_size()`. The derivation is in
`tools/nemagfx_ui_designer/font_metrics.py`; the on-target probe that produced
the numbers is `design_font_probe()` in `render_task.c`.

**Two drivers, one IRQ line.** GP98 (touch) and GPIO117 (EM9305) both map to
`GPIO0_607F_IRQn`, and GP46 (BTN0) shares `GPIO0_203F_IRQn` with the display TE.
Defining a second ISR for either is a multiple-definition link error, so this
project defines neither and relies on `am_hal_gpio_interrupt_service()`
dispatching per pin. `src/radio_task.c`, `src/led_task.c`.

**`am_bsp_psLEDs[]` polarity loses the open-drain bit.** It carries
`AM_DEVICES_LED_ON_LOW` without `AM_DEVICES_LED_POL_OPEN_DRAIN`, so
`am_devices_led_init()` takes the tristate branch and overrides the BSP's own
open-drain pin config. This project applies `g_AM_BSP_GPIO_LED*` directly
instead. `src/ble_led.c`, `src/led_task.c`.

---

## 7. The UI Designer

`tools/nemagfx_ui_designer/` is a PySide6 editor that emits NemaGFX / NemaVG C99
for this project: a 454x454 round canvas (the circle the watch actually shows,
auto-centred on the 468x468 framebuffer at generation time), Label / Image /
Button / Rectangle / Circle / Line / Arc / Gauge, multiple scenes, button-driven
image rotation with easing, and an interactive preview.

Generated output goes to `src/generated/ui_designer/` and carries its own
`ui_designer.mk`, so the project Makefile needs one `include` line and nothing
else when assets are added. Application logic attaches by overriding weak
callbacks; the generated files are overwritten wholesale on every run.

```bash
pip install PySide6 pillow
python tools/nemagfx_ui_designer/main.py
python -m unittest discover -s tools/nemagfx_ui_designer/tests
```

The canvas preview reads the advance table out of the hardware's NemaVG font so
that "will this line be clipped" matches the device rather than the PC's system
font. `README.md` and `CHANGELOG.md` in that directory have the details.

---

## 8. What is not in this repo

- `docs/` — the working notes this code refers to (BLE startup investigation,
  eMMC asset store, memory architecture, map navigation page, watch face
  selector). The comments cite them by path; the reasoning they contain has been
  summarized into the comments themselves.
- The source PNGs for the map tiles and the watch faces. `tools/assets_out/`
  holds the converted `.BIN` files for all five pages, which is what the board
  actually needs. `map_tile_gen.py` and `face*_prep.py` are included so the
  conversion path is reviewable, but their inputs are not here.
- Experimental asset variants under `src/generated/`. Only the headers the build
  actually `#include`s were copied.

---

## 9. Known rough edges

- The time is "seconds since boot" plus `WATCH_START_HOUR`; there is no RTC on
  this board. The date on the trend page works the same way and does not handle
  leap years. Both are marked for removal once an RTC is attached.
- The selected watch face is not persisted — eMMC currently holds assets only,
  with no settings file.
- BLE pairing is fixed at `TMPS_PAIRING_MODE = 2` (bonding + LE Secure
  Connections). Modes 1 and 0 are implemented and documented in `hts_main.c`,
  but both fail indication subscription and the reason is not yet established.
- `MAP_CACHE_SLOTS` resolves to 3 for 3 tiles, so everything is resident and the
  LRU eviction never runs. It is kept for the day the map grows.
