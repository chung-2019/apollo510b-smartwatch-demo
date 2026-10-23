# Generated UI integration

Generated for **454×454** `circle` display.

## 1. Add it to the build

Add one line to `gcc/Makefile` after the `SRC` block and before `CSRC` is expanded:

```make
include ../src/generated/ui_designer/ui_designer.mk
```

Adjust the path to the actual output directory. `SRC` / `VPATH` / `INCLUDES` are
maintained by the fragment, so files added or removed by later generation never require a Makefile change.

## 2. Initialize once

Call this after `nema_init()` and `nema_vg_init(FB_RESX, FB_RESY)`:

```c
#include "ui_designer_generated.h"

if (!ui_designer_init())
{
    am_util_debug_printf("UI Designer init failed\n");
}
```

The return value need not be stored as a flag. After a failure `ui_designer_draw()` still
fills the canvas with the background colour and `ui_designer_touch()` always returns false, so callers need no guard of their own.

## 3. Draw in RenderTask

Call it while building the existing NemaGFX command list:

```c
ui_designer_tick_to(now_ms);
ui_designer_draw(UI_SCENE_DEFAULT);
```

`now_ms` is "what time it is now in milliseconds", and the difference between calls is computed by the generated code -- the caller keeps no variable for it.
When the screen has been away for a while and comes back (switching to another page and
back, for instance), call `ui_designer_resync(now_ms)` first to pull the base up to now, so a running animation does not jump to its end.

`UI_SCENE_DEFAULT` is the first scene. Projects with several scenes use their own `UI_SCENE_xxx`.

The generated functions never create, submit or wait on a command list; `RenderTask`
remains the sole owner of the framebuffer and the command list.

## 4. Touch events

Pass the coordinates in from the `RenderTask` context:

```c
(void)ui_designer_touch(UI_SCENE_DEFAULT, touch_x, touch_y);
```

The coordinate system is the framebuffer's 468x468: the Designer's
454x454 canvas is already centred at (7, 7) at code generation time,
and both drawing and hit testing include that offset, so the caller can pass the raw touch panel coordinates directly.

## 5. Button callback

The weak callbacks produced by this generation:

- `ui_event_button_clicked(void)`

Override one by defining a function of the same name in your own `.c` file; do not edit the
generated files -- the next Generate C overwrites them wholesale.

The symbol name comes from the button's **"click event"** field, falling back to
`<button name>_clicked` only when that field is empty (that is: with the event field
filled in, renaming the button does not affect the symbol). **Changing the "click event"
field changes the symbol name**, and an override works by shadowing a weak definition: when
the names do not match the linker says nothing, the override simply becomes a function
nobody calls, and the button does nothing when pressed. Generate C lists added and removed
symbols in its warnings.

## Assets

- Converted image payload: 67996 bytes.
- Image assets are copied to `NEMA_MEM_POOL_ASSETS` during `ui_designer_init()`.
- Keep the generated font include and symbol synchronized with the project settings.
