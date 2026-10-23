# eMMC Asset Upload GUI — User Guide

`tools/asset_upload.py` writes image assets straight into the board's eMMC over
the existing J-Link / RTT connection. Swapping the home picture, a watch face or
a map tile needs **no firmware rebuild and no reflash**.

中文版：[README_asset_upload.zh-TW.md](README_asset_upload.zh-TW.md)

```
your image (PNG/JPG/BMP...)
     │  converted automatically (png_to_bin.py)
     ▼
  .BIN (raw pixels) ──RTT over J-Link──▶ firmware (UploadTask) ──▶ eMMC (FAT) ──▶ screen
```

---

## 1. Requirements

**Board**

- apollo510b_evb with its J-Link connected over USB.
- Firmware built with `WATCHFACE_ASSET_UPLOAD=1` (the default in `gcc/Makefile`)
  and running. The boot log should show `[upload] task started (RTT ch0 down)`.
- eMMC mounted. If the boot log reports the asset store as `FAIL`, uploads
  cannot work either.

**PC**

| Item | Needed for | Notes |
|---|---|---|
| Python 3 with tkinter | always | The python.org Windows installer includes tkinter. |
| SEGGER J-Link software | always | Provides `JLinkGDBServerCL`, which hosts the RTT port. |
| Pillow | adding images (PNG/JPG/...) | `pip install pillow`. Not needed if you only send ready-made `.BIN` files. |

---

## 2. Quick start

1. Start the RTT server (section 3).
2. Run the GUI:

   ```powershell
   python tools\asset_upload.py
   ```

3. Press **Connect**. The right column fills with what is on the eMMC now.
4. Add files with **Add image / asset...** or **Add assets_out**.
5. Press **Upload**.
6. Reset the board to see the new artwork.

---

## 3. Start the RTT server first

The GUI does not talk to J-Link itself. It connects to the RTT telnet port
(`127.0.0.1:19021`) of a running `JLinkGDBServer`. Start one in either way:

**Option A — `build_gui.py` (easiest)**

1. Run `launch_build_gui.bat` (or `python build_gui.py`).
2. Open the **RTT log** tab and press **Start RTT**.
3. Leave this window open for as long as you upload.

**Option B — command line**

```powershell
JLinkGDBServerCL -device AP510NFA-CBR -if SWD -speed 4000 -rtttelnetport 19021
```

> The port `19021` is fixed in the tool (`PORT` near the top of
> `asset_upload.py`). If you changed **Telnet Port** in `build_gui.py`, change
> `PORT` to match.

---

## 4. The window

```
┌─ eMMC asset upload ───────────────────────────────────────────────┐
│ Connected 127.0.0.1:19021                            [Disconnect] │ ← status / connect button
│ To upload                        On the eMMC                      │
│ ┌──────────────────────────────┐ ┌──────────────────────────────┐ │
│ │ HOME.BIN      412232 bytes   │ │ HANDHR.BIN      5400 bytes   │ │ ← left: queue
│ │ WF4BG.BIN     412232 bytes   │ │ HOME.BIN      412232 bytes   │ │   right: board
│ └──────────────────────────────┘ └──────────────────────────────┘ │
│ [██████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░]                       │ ← progress of current file
│ ┌───────────────────────────────────────────────────────────────┐ │
│ │ sending HOME.BIN ...                                          │ │ ← message log
│ │   OK   [upload] ok HOME.BIN                                   │ │
│ └───────────────────────────────────────────────────────────────┘ │
│ [Add image / asset...][Add assets_out][Remove selected][Clear]    │
│                                   [Read eMMC contents] [Upload]   │
└───────────────────────────────────────────────────────────────────┘
```

| Area | Content |
|---|---|
| Status line | `Not connected`, `Connected 127.0.0.1:19021`, `Connection failed` or `Disconnected`. |
| **To upload** (left) | The files queued for sending, with name and size. Supports multi-select (Ctrl / Shift + click). |
| **On the eMMC** (right) | The board's current files, read back from the firmware. Tags are explained in section 6. |
| Progress bar | Progress of the file being sent. |
| Message log | Every step, result and error. Read this first when something goes wrong. |

### Buttons

| Button | What it does |
|---|---|
| **Connect / Disconnect** | Opens or closes the connection to the RTT port. Connecting also reads the eMMC contents once. |
| **Add image / asset...** | Opens a file dialog. `.BIN` files are queued as they are; images (PNG, JPG, BMP, WEBP...) are converted to `.BIN` first. Multiple files can be selected. |
| **Add assets_out** | Queues every file in `tools/assets_out/` — the complete ready-made set (21 files, about 3.4 MB). |
| **Remove selected** | Removes the selected rows from the queue. Does not touch the eMMC. |
| **Clear** | Empties the queue. Does not touch the eMMC. |
| **Read eMMC contents** | Asks the firmware for its file list and refreshes the right column. |
| **Upload** | Sends the queue. If rows are selected in the left column, **only those** are sent; with no selection, everything is sent. |

---

## 5. Step by step

### 5.1 Connect

Press **Connect**. After about 0.4 s the tool reads the eMMC and the log prints
a summary, for example:

```
eMMC holds 21 files, 21 files, 7654321 KB free of 7812500 KB
  all 17 assets present.
```

or, when something is missing:

```
  missing assets (5/17): WF4BG.BIN, WF4CAP.BIN, WF4HR.BIN, WF4MIN.BIN, WF4SEC.BIN
```

### 5.2 Add files

**Ready-made `.BIN` files** — use **Add image / asset...** and pick them, or
**Add assets_out** for the whole set.

**Images** — use **Add image / asset...** and pick the image. The tool converts
it automatically:

- **The target is taken from the file name.** `HOME.png` becomes `HOME.BIN`,
  `WF4CAP.png` becomes `WF4CAP.BIN` (case does not matter; `HOME.BIN.png` also
  works).
- **Any other name opens a dialog** titled *Which asset should this image
  replace?*. Choose the target and press **OK** (or double-click / Enter).
  **Skip this one** (or Esc) leaves the image out.
- **Size**: if the image is not the target's size, it is scaled with LANCZOS.
  The log shows the conversion as `converted my_photo.png -> HOME.BIN`.
- **Format**: chosen by the target (RGB565 or RGBA8888). You do not set it.
- **Dial centring**: for `WATCHBG.BIN`, `WF2BG.BIN` and `WF3BG.BIN`, the round
  dial is shifted so its centre sits exactly in the middle of the image, so the
  hands meet at the knob. `WF4BG.BIN` is **not** auto-centred; prepare it with
  `tools/face4_prep.py`.

The converted file is written to `tools/assets_out/<TARGET>.BIN` and that path is
queued. Adding a second image for the same target updates the existing row
instead of adding a duplicate.

### 5.3 Check the queue

Each row shows name and size. A row ending in **`<- size mismatch!`** is a
`.BIN` whose length does not match what the firmware expects; the upload will be
refused for that file (see section 7).

### 5.4 Upload

- Send everything: make sure no row is selected, then press **Upload**.
- Send only some files: select them in the left column (Ctrl / Shift + click),
  then press **Upload**. The log says
  `Sending only the N selected (clear the selection to send everything).`

Each file is an independent transfer. Changing one image only requires sending
that one file — the rest of the set does not need resending.

During the transfer the progress bar fills per file and the log shows:

```
sending HOME.BIN ...
  OK   [upload] ok HOME.BIN
Done: 1 / 1 succeeded.
Reboot (or switch to the map page) to see the new assets.
```

Speed is limited by RTT: roughly ten-odd seconds per 2 MB. The full set takes
a few tens of seconds. The GUI does not respond to clicks while sending.

### 5.5 Verify

After a successful upload the right column is refreshed automatically, so the
sizes shown are the board's real state. You can also press
**Read eMMC contents** at any time.

### 5.6 See it on screen

| Asset | When the firmware reads it | What to do |
|---|---|---|
| Home image, all watch face files | Once, at boot | Reset the board. |
| Map tiles | When the map page needs a tile | A tile that was missing appears when you open the map page. A tile already shown before the upload may still be cached: reset the board. |

When in doubt, reset the board.

---

## 6. Tags in the "On the eMMC" column

| Tag | Meaning |
|---|---|
| *(none)* | A current asset with the correct size. |
| `<- size mismatch!` | A known asset with the wrong size. The firmware prints `short read` at boot and **does not draw it**. Re-upload a correct file. |
| `(kept, not loaded by the firmware)` | `WF2*.BIN` (the "aurora" face). Its slot was given to the Ambiq face; the files are kept so it can be switched back. Harmless. |
| `(not an asset)` | A file the firmware does not use, e.g. a leftover `~UPLOAD.TMP`. It can be removed with `--delete` (section 9). |
| `(empty)` | The eMMC has no files. Send the full set with **Add assets_out**. |
| `read failed` | The firmware did not answer. See section 8. |

---

## 7. Asset reference

Assets are raw pixel data with **no header**, so size and format must match
`ASSET_*` in `src/asset_store.h` exactly. Run
`python tools/png_to_bin.py --targets` to print this list.

| Target | Size (px) | Format | Bytes | Used by |
|---|---|---|---:|---|
| `HOME.BIN` | 454×454 | RGB565 | 412,232 | Home page picture |
| `WATCHBG.BIN` | 454×454 | RGB565 | 412,232 | "chrono" face — dial (auto-centred) |
| `HANDHR.BIN` | 15×90 | RGBA8888 | 5,400 | "chrono" face — hour hand |
| `HANDMIN.BIN` | 15×150 | RGBA8888 | 9,000 | "chrono" face — minute hand |
| `HANDSEC.BIN` | 13×179 | RGBA8888 | 9,308 | "chrono" face — second hand |
| `WF4BG.BIN` | 454×454 | RGB565 | 412,232 | "ambiq" face — dial (prepare with `face4_prep.py`) |
| `WF4HR.BIN` | 30×130 | RGBA8888 | 15,600 | "ambiq" face — hour hand |
| `WF4MIN.BIN` | 30×193 | RGBA8888 | 23,160 | "ambiq" face — minute hand |
| `WF4SEC.BIN` | 18×126 | RGBA8888 | 9,072 | "ambiq" face — second hand |
| `WF4CAP.BIN` | 46×46 | RGBA8888 | 8,464 | "ambiq" face — centre cap |
| `WF3BG.BIN` | 454×454 | RGB565 | 412,232 | "numbers" face — dial (auto-centred) |
| `WF3HR.BIN` | 23×107 | RGBA8888 | 9,844 | "numbers" face — hour hand |
| `WF3MIN.BIN` | 33×178 | RGBA8888 | 23,496 | "numbers" face — minute hand |
| `WF3SEC.BIN` | 18×213 | RGBA8888 | 15,336 | "numbers" face — second hand |
| `MAPTILE0.BIN` – `MAPTILE2.BIN` | 468×468 | RGB565 | 438,048 each | Map page tiles |
| `WF2BG.BIN` | 390×390 | RGB565 | 304,200 | "aurora" face (kept, not loaded) |
| `WF2HR.BIN` / `WF2MIN.BIN` / `WF2SEC.BIN` | 22×104 / 28×153 / 18×219 | RGBA8888 | 9,152 / 17,136 / 15,768 | "aurora" face (kept, not loaded) |

**Artwork tips**

- **Hands**: use a transparent background (RGBA). The rotation pivot of each
  hand is fixed in the firmware (`PIVOT_*` in `src/render_task.c`), so a new hand
  must have the **same size** and the pivot at the **same pixel** as the
  original. Auto-scaling a hand of a different size moves the pivot.
- **Dials**: for `WF3BG.BIN` use `src/generated/face3_dial_454_centered.png`;
  for `WF4BG.BIN` use `src/generated/face4_dial_454_centered.png`. The other
  PNGs next to them are design previews with hands already painted on, or are
  off-centre — the tool accepts them without error, but the result is wrong on
  screen.
- **Map tiles**: replacing a tile changes only the picture. The navigation
  route is in `src/map_route.h` and does not follow.

---

## 8. Troubleshooting

| Message in the log | Cause | Fix |
|---|---|---|
| `cannot connect to 127.0.0.1:19021 -- ...` | No `JLinkGDBServer` running, or a different port. | Start RTT (section 3); check the port. |
| `failed to read the eMMC contents: no reply from the target ...` | The firmware is not running, was built without `WATCHFACE_ASSET_UPLOAD=1`, or RTT has not attached yet. | Check the boot log for `[upload] task started`; press **Read eMMC contents** again. |
| `failed to read the eMMC contents: the target reported a read failure` | The eMMC is not mounted. | Check the boot log for the asset store status; reset the board. |
| `FAIL size mismatch: N, expected M ...` | The `.BIN` does not match the target size. | Add the original image instead (it is scaled automatically), or regenerate the `.BIN` with `png_to_bin.py`. |
| `FAIL file name longer than 12 characters` | Names must be 8.3 format. | Use one of the target names in section 7. |
| `FAIL [upload] abort <name>: timeout` | The transfer stalled on the target. | Upload that file again. |
| `FAIL the target stopped responding at X/Y bytes` | The target stopped acknowledging mid-file. | Check the board is still running; reconnect and upload again. |
| `FAIL [upload] err <name>: sum ... != ...` | Data was corrupted on the way. | Upload again; the new file replaces it. |
| `FAIL no reply from the firmware ...` | Same as the "no reply" case above. | As above. |
| `Adding an image directly needs Pillow` | Pillow is not installed. | `pip install pillow`, or convert first with `png_to_bin.py` and add the `.BIN`. |
| `skipped <file> (no target chosen)` | The target dialog was closed with **Skip this one**. | Add the image again and choose a target. |
| The image is uploaded but the screen still shows the old one | Assets are read at boot. | Reset the board. |
| The image is uploaded but that area is black | Size mismatch, or a file is missing. | Press **Read eMMC contents** and look for `<- size mismatch!` and the `missing assets` line. |
| Errors / no response after pressing **Stop RTT** in `build_gui.py` | Stopping RTT ends the GDB server and drops this tool's connection. | Start RTT again, then **Disconnect** and **Connect** here. |

An interrupted or failed upload is safe: the firmware writes to a temporary file
(`~UPLOAD.TMP`) and only replaces the real file after the whole file has arrived,
so the previous version stays on the eMMC.

---

## 9. Command-line mode

The same tool without the GUI. Use it for scripts, or to delete files (the GUI
has no delete button).

```powershell
python tools\asset_upload.py                          # GUI
python tools\asset_upload.py --list                   # list the eMMC contents (-l also works)
python tools\asset_upload.py tools\assets_out\HOME.BIN # send .BIN files
python tools\asset_upload.py HOME.png WF4CAP.png      # convert images, then send
python tools\asset_upload.py --delete ~UPLOAD.TMP     # delete files from the eMMC
```

- Images on the command line must be named after their target (`HOME.png`);
  there is no dialog to ask, so other names are rejected with the list of
  valid targets.
- The exit code is `0` on success and `1` if any file failed.

Converting without uploading:

```powershell
python tools\png_to_bin.py my_photo.png HOME.BIN      # one image -> tools\assets_out\HOME.BIN
python tools\png_to_bin.py --dir my_images\           # a folder, matched by file name
python tools\png_to_bin.py --targets                  # list every target
```

---

## 10. Notes

- **Converting an image overwrites `tools/assets_out/<TARGET>.BIN`**, which is
  the ready-made set kept in git. To restore the original:
  `git checkout -- tools/assets_out/`.
- **Security**: `WATCHFACE_ASSET_UPLOAD` lets anyone with SWD access rewrite the
  eMMC. Set it to `0` in `gcc/Makefile` for production firmware.
- **Blank board**: send the full set with **Add assets_out** → **Upload**, then
  reset.
- How the transfer works (protocol, flow control): `src/asset_upload.h` and the
  header of `tools/asset_upload.py`.
