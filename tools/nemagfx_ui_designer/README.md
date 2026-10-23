# Apollo510B NemaGFX UI Designer

A Python / PySide6 desktop UI editor that runs on Windows, aimed at providing a workflow
similar to STM32 TouchGFX Designer and emitting NemaGFX / NemaVG C99 code this project can use directly.

The default canvas is **454x454**, the circle the watch actually shows. The framebuffer of
`apollo510b_evb` is 468x468, whose outermost 7 px are hidden by the bezel, so text placed
there is cut off on hardware and the canvas is deliberately smaller; code generation
centres the whole canvas on the framebuffer (+7 px per side), and both drawing and touch
hit testing already include that offset, so the firmware needs no conversion. The target
framebuffer size can be changed under "Project settings".

## Features

- A 454x454 round canvas with safe-area hints (centred automatically on the 468x468 framebuffer at generation time).
- Drag and drop, move, and resize by dragging the bottom-right corner.
- Label, Image, Button, Rectangle, Circle, Line, Arc, Gauge.
- Multiple scenes and layer order.
- Live property editing: colour, opacity, rotation, data binding, button events.
- A Button can rotate an Image by a relative amount or to a given angle, with duration and easing.
- `F5` / "Interactive preview" tests the animations by clicking the buttons on the PC canvas.
- JSON project save and load.
- PNG/JPEG/BMP conversion into NemaGFX RGB565 or RGBA8888 C arrays.
- One-click Generate C.
- Configurable Build and Flash commands, runnable from the GUI.
- Weak button callbacks are generated, keeping the generated files separate from hand-written application logic.

## Installation and startup

In PowerShell:

```powershell
cd tools\nemagfx_ui_designer
python -m pip install -r requirements.txt
python main.py
```

Or install the dependencies and double-click `run_designer.bat`.

Pillow is already installed in the current working environment, but if PySide6 is not, the pip command above has to be run first.

## Basic use

1. Drag a widget from the left onto the canvas, or double-click its name.
2. Drag a widget to move it; drag the blue handle at the bottom right of the selection to resize.
3. Edit text, colour, image, events and binding under Properties on the right.
   A widget with "Visible" unchecked is drawn faintly in edit mode but stays on the canvas
   and can still be selected and edited; only the interactive preview and the generated C
   really hide it.
4. Confirm the canvas, the output directory, the NemaVG font and the Build/Flash commands under "Project settings".
5. Save the JSON project.
6. Press `Generate C`.
7. Add the files to the existing RenderTask following `README_INTEGRATION.md` in the output directory.

## Driving an Image rotation from a Button

Taking "pressing `test` turns the central clock one full turn" as the example:

1. Select the `test` Button.
2. Set "Press action" to "Rotate by".
3. Set "Action target" to the central clock's Image name, e.g. `image` or `clock`.
4. Set "Rotation angle" to `360`.
5. Set "Animation time" to `800` ms.
6. Set "Easing" to `Ease In Out`.
7. Press `F5` or "Interactive preview" on the toolbar, then click `test` on the canvas.

"Rotate by" adds the given angle to the current one on every press; "Rotate to" turns to a
fixed absolute angle. The generated C updates the animation with `ui_designer_tick(delta_ms)`
and creates no FreeRTOS software timer.

[`sample_454.json`](sample_454.json) can be opened first to try it out.

## Generated files

The default output is `src/generated/ui_designer` under the project root:

```text
ui_designer_project_config.h
ui_designer_assets.c
ui_designer_assets.h
ui_designer_bindings.c
ui_designer_bindings.h
ui_designer_generated.c
ui_designer_generated.h
ui_project.generated.json
README_INTEGRATION.md
```

RenderTask has to advance the animation before drawing, every frame:

```c
ui_designer_tick(frame_delta_ms);
ui_designer_draw(current_scene);
```

Touch events should be passed in from the RenderTask context:

```c
(void)ui_designer_touch(current_scene, touch_x, touch_y);
```

The generator emits APIs this SDK provides directly, for instance:

- `nema_fill_rect()`, `nema_fill_rounded_rect()`.
- `nema_fill_circle_aa()`, `nema_draw_line_aa()`.
- `nema_vg_draw_ring()`, `nema_vg_print()`.
- `nema_bind_src_tex()`, `nema_blit_rect_fit()`.
- `nema_buffer_create_pool(NEMA_MEM_POOL_ASSETS, ...)`.

## Threading and resource ownership

The generated `ui_designer_draw()` only records drawing commands; it never creates, submits or waits on a command list.
It should be called by the existing RenderTask, so RenderTask remains the sole owner of the
framebuffer, the scene state and the NemaGFX command list.

BLE and sensor tasks should not call drawing functions directly; they should hand their data
to RenderTask through a FreeRTOS queue or the existing synchronization, and RenderTask calls the generated binding setters.

## Current boundaries

This is a working first version of the full loop, not all of TouchGFX. Not yet included:

- A TTF/OTF glyph subset converter.
- A timeline / keyframe animation editor.
- An undo/redo command stack.
- Automatic modification of every IDE project file.
- Automatic detection of each flashing tool's parameters.

The font currently used is the project's existing `DejaVuSans_ttf_kern`. Other fonts can be
produced with the Ambiq / NemaVG font tools and then entered as a header and symbol under "Project settings".

The existing `DejaVuSans_ttf_kern` covers Basic Latin and Greek only; it contains no
Traditional Chinese glyphs and no degree sign. Generate C warns about text outside the
existing ranges; displaying such characters on hardware requires generating a NemaVG font
with the needed glyph subset first.

## Change log

See [`CHANGELOG.md`](CHANGELOG.md).
