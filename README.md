# nemagfx_watchface — Apollo510B EVB

<p align="center">
  <a href="https://youtu.be/N_bFMPd-4I8"><img src="https://i.ytimg.com/vi/N_bFMPd-4I8/maxresdefault.jpg" alt="Watch the Apollo510B Smart Watch Demo on YouTube" width="73%"></a>
  <a href="https://www.youtube.com/shorts/pJUpLRMOmOk"><img src="https://i.ytimg.com/vi/pJUpLRMOmOk/oardefault.jpg" alt="Watch Linux BLE Am62P connect to Apollo 510B on YouTube Shorts" width="23%"></a>
</p>

Watch on YouTube: [Apollo510B Smart Watch Demo](https://youtu.be/N_bFMPd-4I8) ·
[Linux BLE Am62P connect to Apollo 510B](https://www.youtube.com/shorts/pJUpLRMOmOk) (Shorts).
Both videos are hosted externally on YouTube and are not included in this source repository.

A six-page FreeRTOS smartwatch UI on the apollo510b_evb, built on NemaGFX / NemaVG,
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
  render_task.c          all drawing: 6 pages, overlays, watch face selector,
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
  Generate_3/            weather page sources: five sky PNGs, the overlay, the
                         original composite it is cropped from (original/)

gcc/                     Makefile, linker script, startup, J-Link scripts
tools/                   host-side Python: asset conversion and upload
  png_to_bin.py          PNG -> .BIN (the common exit of every conversion path)
  asset_upload.py        push .BIN into eMMC over RTT (GUI and CLI)
  asset_export.py        export the firmware's C arrays into .BIN
  face2/3/4_prep.py      per-watch-face asset preparation
  map_tile_gen.py        map PNGs -> RGB565 C arrays
  weather_prep.py        weather page: five backgrounds + the overlay -> WTHR*.BIN
  assets_out/            ready-made .BIN files for all six pages
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

### 4.2 Memory and where the graphics live

Two rules decide where every image ends up:

- **The GPU cannot sample MRAM**, let alone eMMC. A bitmap has to sit in SSRAM,
  or in PSRAM through MSPI XIP, before it can be drawn. Binding a const array's
  address straight to the GPU — which works on Apollo4P — produces a flickering
  red screen here.
- **Large bitmaps are files on eMMC, not part of the firmware.** They are read
  at run time, so artwork can be replaced without reflashing (section 5).
  Anything that can be described as geometry — gauges, curves, icons and all
  text — is drawn by NemaVG at run time and needs no bitmap at all.

#### Graphics by page

| Page | Bitmaps (eMMC file → runtime copy) | Drawn at run time |
|---|---|---|
| HOME | `HOME.BIN` 454x454 RGB565 → SSRAM | the sweeping light (`nema_fill_rounded_rect`, additive blend) |
| WATCH | dial and hands of all three faces → SSRAM | the face selector ring; its previews are the same textures, scaled |
| TEMP | — | everything, with NemaVG: gauge arcs (`nema_vg_draw_ring`), ticks, the BLE and pairing icons (VG paths), the reading and the status text (DejaVuSans vector font) |
| TREND | — | everything, with NemaVG: the 60-second curve, grid and rim (`nema_vg_draw_line`, `nema_vg_draw_ring`), clock and date text; the area under the curve is `nema_fill_quad_f` |
| WEATHER | `WTHRBG0..4.BIN` 468x468 RGB565 (five skies) and `WTHRUI.BIN` 348x392 RGBA8888 (overlay) → PSRAM | the time and the Bluetooth symbol with a stroked halo (NemaVG); the slow pan and the cross-fade are GPU blits (`nema_blit_quad_fit`) |
| MAP | `MAPTILE0..2.BIN` 468x468 RGB565 → PSRAM | the car and turn arrows (VG paths), the HUD panel and its text |
| overlays | — | control centre, brightness panel and the temperature page's Bluetooth profile panel (rounded rects plus VG icons and text); the page overview scales the current framebuffer into a thumbnail |

The vector font needs no copy either: NemaVG expands the glyphs into paths on
the CPU, reading the font straight from MRAM. The bitmap font it replaced was a
texture, and cost 358 KB of MRAM plus the same again in SSRAM.

**The weather page and the old weather icon are different things.** The page is
the bitmaps above, on eMMC. `src/generated/blowing.h`, `climate.h` and `day.h`
are a small TSVG icon for the watch face, compiled into the firmware; it is
switched off (`WATCHFACE_SHOW_WEATHER_ICON 0` in `render_task.c` — its position
collides with the current dials), and `--gc-sections` then drops it, so it costs
nothing in the current build.

The one bitmap still built into the firmware is the UI Designer page's 178x191
image (68 KB in MRAM, copied into SSRAM by `ui_designer_init()`). That page is
currently out of the carousel (see `ui_page_e`), but the copy is still made at
boot.

#### eMMC contents

FAT on the 8 GB eMMC, one file per asset, raw pixels with no header. The full
set is `tools/assets_out/`: 27 files, 6,173,912 B.

| Group | Files | Bytes | Where it goes |
|---|---|---|---|
| home image | `HOME.BIN` | 412,232 | SSRAM, at boot |
| face "chrono" | `WATCHBG`, `HANDHR`, `HANDMIN`, `HANDSEC` | 435,940 | SSRAM, at boot |
| face "ambiq" | `WF4BG`, `WF4HR`, `WF4MIN`, `WF4SEC`, `WF4CAP` | 468,528 | SSRAM, at boot |
| face "numbers" | `WF3BG`, `WF3HR`, `WF3MIN`, `WF3SEC` | 460,908 | SSRAM, at boot |
| face "aurora" | `WF2BG`, `WF2HR`, `WF2MIN`, `WF2SEC` | 346,256 | not loaded — its slot went to the ambiq face |
| map tiles | `MAPTILE0` .. `MAPTILE2` | 1,314,144 | PSRAM, by MapTask (the start area is preloaded at boot) |
| weather page | `WTHRBG0` .. `WTHRBG4`, `WTHRUI` | 2,735,904 | PSRAM, at boot (about 200 ms); a missing file is retried every 2 s while the page is shown |

`WATCHFACE_ASSET_PROVISION=1` seeds only the home image, the chrono face and the
map tiles (8 files, 2,162,316 B — the "+2.16 MB" in section 5). The ambiq and
numbers faces always come from `asset_upload.py`.

#### Memory map

From `gcc/linker_script.ld` and the `.map` of the build described below.

| Region | Base | Size | Used | Contents |
|---|---|---|---|---|
| MRAM | `0x00410000` | 4,128,768 B | 571,348 B (13.8%) | code, const data, initial values of `.data` |
| ITCM | `0x00000000` | 32 KB | 28 B | `.itcm_text` |
| DTCM | `0x20000000` | 512 KB | `.data` + `.bss` 116,720 B of 472 KB; main stack 11,008 B of 36 KB | FreeRTOS heap 48 KB (every task's stack and TCB), IOM / MSPI driver state, RTT buffers 2 x 4 KB, Cordio control blocks |
| SSRAM | `0x20080000` | 3 MB | 2,757,472 B (87.7%) | the NemaGFX pool (2,752,512 B) plus 4,960 B of `AM_SHARED_RW` buffers (eMMC / FatFs / SDIO, touch DMA) |
| PSRAM | `0x60000000` | 64 MB | 4,087,808 B (6%) | two 4 MB regions (see `psram.h`): the map tile cache at `0x60000000`, 3 slots x 442,368 B; the weather page at `0x60400000`, 5 x 442,368 B + 548,864 B (4 KB aligned) |
| eMMC | SDIO0 | 8 GB | 3,438,008 B | the asset files above |

About 379 KB of SSRAM is still outside the pool; raising `VMEM_SIZE` is the way
to use it.

#### The GPU pool

`VMEM_SIZE = 0x2A0000` (2,752,512 B) in SSRAM. Every `NEMA_MEM_POOL_*` name
allocates from this one array. In `RenderTask()` order:

| Allocation | Bytes |
|---|---|
| command-list ring buffer (`nema_init`) | 9,664 |
| NemaVG working buffers (`nema_vg_init(468, 468)`) and VG paths | not reported |
| UI Designer image, copied from MRAM | 67,996 |
| command list, sized to hold a whole TEMP frame | 32,768 |
| framebuffer, 468x468 RGB565 | 438,048 |
| page-overview thumbnail, 208x208 RGB565 | 86,528 |
| 14 textures from eMMC (home image + three faces) | 1,777,608 |
| **listed total** | **2,412,612** |

The remaining 339,900 B covers NemaVG's buffers, allocation alignment and
whatever is actually free; only the target knows the split. The boot log line
`[render] textures: N loaded (X B), M failed` is the measured figure.

The order is deliberate and was the root cause of two hardware failures: the
framebuffer and the thumbnail are allocated **before** the eMMC textures, so a
short pool costs a black watch face rather than a GPU bound to NULL and a dead
board. The comment at that call site records both incidents.

#### Build size

```
nemagfx_watchface.axf  5,446,340 B  ELF with symbols and debug info, for the debugger only
nemagfx_watchface.bin    571,348 B  the exact MRAM image J-Link writes at 0x00410000
nemagfx_watchface.hex  1,607,113 B  the same 571,348 B as Intel HEX text
```

Only the `.bin` reaches the chip: `.text` 566,384 + `.ARM.exidx` 8 +
`.itcm_text` 28 + `.data` initial values 4,928 = 571,348 B. `.bss` (111,792 B)
and the GPU pool are `NOLOAD`, so they take RAM but no MRAM — which is also why
a pool that is too small still links cleanly. The `.axf` size varies with the
build directory, because the debug info records source paths.

By component, from the `.map` (default build, `WATCHFACE_ASSET_PROVISION=0`):

| Component | Bytes | Share |
|---|---|---|
| NemaGFX / NemaVG libraries | 101,253 | 17.7% |
| Cordio BLE stack, WSF, uECC | 95,839 | 16.8% |
| Ambiq HAL (`libam_hal.a`) | 88,408 | 15.5% |
| UI Designer image (`ui_designer_assets.c`) | 68,136 | 11.9% |
| DejaVuSans vector font | 65,613 | 11.5% |
| other application sources | 37,260 | 6.5% |
| `render_task.c` | 36,622 | 6.4% |
| FatFs and the eMMC driver | 23,620 | 4.1% |
| Ambiq device drivers and utils | 16,512 | 2.9% |
| FreeRTOS | 15,767 | 2.8% |
| NemaGFX platform port (`nema_hal.c` etc.) | 6,735 | 1.2% |
| BSP and SEGGER RTT | 4,712 | 0.8% |
| alignment and unattributed | 10,871 | 1.9% |

With `WATCHFACE_ASSET_PROVISION=1` the seed images add 2,162,316 B, for an image
of about 2.6 MB — still well inside MRAM.

### 4.3 Pages

| Page | Content |
|---|---|
| HOME | EdgePilot image with an additive sweeping light on the horizon |
| WATCH | analogue face; swipe down for the face selector (3 faces) |
| TEMP | TMP119 reading, circular gauge, BLE state (5 variants incl. LESC pairing); pull down from the top edge for the Bluetooth profile panel (device name, RSSI, service, security) |
| TREND | clock, date, 60-second temperature curve with a shear projection |
| WEATHER | five cloud skies panning slowly and cross-fading every 24 s under a fixed overlay (date, temperature, forecast), with a live clock |
| MAP | Taipei 101 -> Taipei Main Station navigation over PSRAM tiles; tap the top panel to pause / resume, swipe down from the centre to leave |

Overlays: brightness panel (swipe up from the bottom edge, every page), control
centre (swipe down from the top edge, home only), quick page menu (swipe right,
home only: trend, weather, navigation), Bluetooth profile panel (swipe down from
the top edge, temperature page only), page overview (BTN0 or a double tap, every
page).

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

`tools/assets_out/` already contains a complete set for all six pages.

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

**A FreeRTOS mutex created before the scheduler starts leaves interrupts masked.**
Creating it enters a critical section, and before the scheduler runs the port's
nesting count is not 0, so leaving the section does not unmask interrupts again
-- the eMMC provisioning and `psram_init()` that follow would run with interrupts
off. The filesystem lock is therefore created by `AssetStoreLockInit()` in
`setup_task()`. (It used to be declared and never created at all, so MapTask,
RenderTask and the upload task could all be inside FatFs at once.)
`src/asset_store.c`, `src/rtos.c`.

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
  holds the converted `.BIN` files for all six pages, which is what the board
  actually needs. `map_tile_gen.py` and `face*_prep.py` are included so the
  conversion path is reviewable, but their inputs are not here. (The weather
  page's sources are included: `src/Generate_3/`, used by `weather_prep.py`.)
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
- The weather page shows sample data: the date, place, temperature and forecast
  are baked into `WTHRUI.BIN` by `weather_prep.py`, and only the clock is live.
  There is no weather source on the board yet.

---

## 10. License and notices

The project's own code and assets are under the BSD-3-Clause license in
[`LICENSE`](LICENSE). Files derived from AmbiqSuite (Ambiq, BSD-3-Clause), the
Cordio sample applications (Arm / Packetcraft, Apache-2.0), FreeRTOS (MIT) and
the DejaVu fonts keep their own notices; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Where each image comes from is
described in [ASSET_PROVENANCE.md](ASSET_PROVENANCE.md).

Map data © [OpenStreetMap contributors](https://www.openstreetmap.org/copyright),
available under the Open Database License (ODbL).
