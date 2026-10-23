# Change log

## 2026-09-14 -- Fix text clipped on hardware (the automatic font shrink never worked)

### Symptom

The Designer preview shows the whole of `Ambiq_EdgePilot`, while the flashed firmware shows
only `Ambiq_EdgePil` with the last two letters clipped.

### Root cause

The measurement in the generated `ui_designer_draw_text()` was written as

```c
nema_vg_string_get_bbox(text, &measured_w, &measured_h, (float)width, 0);
if ((measured_w > (float)width) && ...)
```

The 4th argument of `nema_vg_string_get_bbox()` is max_w, and the width returned is clamped
to it. Measuring with the widget's box width means `measured_w` can never exceed `width`,
that if never holds -- the whole automatic shrink effectively does not exist,
`nema_vg_print()` draws at the original size, and the overflow is clipped at the right edge.

The Designer preview uses the advance table it parses itself, with no such limit, so the
preview shrinks while hardware does not and the two disagree. Measured in this case:
`Ambiq_EdgePilot` is 313 px wide at 28 px, while the box is only 220 px.

### Changes

- The measurement now uses `UI_TEXT_MEASURE_MAX_W` (8192.0f), which gives the real single-line width.
- Shrinking leaves 2% of margin (`UI_TEXT_FIT_RATIO`): the measurement and print's internal
  rounding do not agree exactly, and shrinking to exactly the box width can still clip half
  a stroke off the last letter. The preview in canvas.py applies the same value.
- Generate C gains a warning: when static text will be shrunk on hardware, it says how many
  px were measured, how wide the box is, and what size it will be shrunk to. Shrinking is a
  backstop, not the design intent, and should not first be discovered on hardware.
- When the advance table is not found, it is searched for again from the project file's location (which is how it is found when generating outside the repo).
- The label box in `tools/ui_project3.json` widened from 220 to 320 (`x` 67), so
  `Ambiq_EdgePilot` fits at 28 px and no longer triggers the shrink.

### Verification

- `python -m unittest discover -s tests`: 20 passed, with a new
  `test_overflowing_text_is_warned` and a regression assertion added to
  `test_draw_text_shrinks_to_fit_at_runtime` that the measurement limit must not be the box width.
- Re-running Generate C with `tools/ui_project3.json`: no warnings.

## 2026-09-14 -- The canvas becomes 454x454, centred on the 468x468 framebuffer at generation time

### Symptom

Text laid out neatly in the Designer is clipped near the edges once flashed.

### Root cause

The canvas size followed the framebuffer's 468x468, while the watch actually shows only the
middle circle of about 454 diameter, with the outermost ring hidden by the bezel. The
editable area was larger than the hardware's, so widgets naturally ended up where they cannot be seen.

### Changes

- `ProjectSettings.display_width/height` now default to **454** (the visible circle), with
  new `framebuffer_width/height` (default 468 = `FB_RESX`) describing the target board's framebuffer.
- Code generation centres the whole canvas on the framebuffer: `origin = (fb - display) // 2 = 7`,
  and both the drawing coordinates and the button hit tests include that offset, so the
  generated code emits framebuffer coordinates and the firmware still passes the raw touch
  panel coordinates to `ui_designer_touch()` with no conversion of its own.
- The background now fills the whole framebuffer (`UI_DESIGNER_FRAMEBUFFER_WIDTH/HEIGHT`), so
  a canvas smaller than the framebuffer leaves no image from the previous page in the outer ring.
- The config header gains `UI_DESIGNER_FRAMEBUFFER_WIDTH/HEIGHT` and
  `UI_DESIGNER_ORIGIN_X/Y`.
- The project settings dialog renames "Framebuffer width / height" to "Visible canvas width /
  height" and adds "Panel framebuffer width / height".
- `sample_468.json` is renamed `sample_454.json`; `tools/ui_project.json` and
  `tools/ui_project1.json` are converted to a 454 canvas with every widget coordinate reduced
  by 7, so their positions on hardware are exactly as before.

### Compatibility

Older project files have no `framebuffer_*` fields and are filled in with values "not smaller
than the canvas" on load, degenerating the offset to 0 and behaving as before -- i.e. an old
468-canvas project is not silently moved when opened.

### Verification

- `python -m unittest discover -s tests`: 19 passed (with a new
  `test_canvas_is_centered_on_framebuffer`).
- Re-running Generate C with `tools/ui_project1.json` gives coordinates identical to before,
  character for character (127,95 / 184,184 / 150,320), proving "shrink the canvas + centre at generation" is a zero change on hardware.

## 2026-09-14 -- Fix the program closing without warning while using the property panel on the right

### Symptom

After starting with `run_designer.bat`, selecting any widget on the canvas and then using the
Properties panel on the right (adjusting X/Y/width/height, holding a spin button, or
unchecking "Visible") makes the window simply disappear. There is no Python traceback, no
error dialog, and even `if errorlevel 1 pause` in `run_designer.bat` often cannot catch it --
because this is not a Python exception but an access violation in Qt's C++ layer.

### Root cause

The property panel deleted one of its own controls **while that control was emitting a signal** (use-after-free).

`PropertyEditor._clear()` used `QFormLayout.removeRow()`, an API that **immediately** deletes
that row's controls. And `_clear()` is the first thing `show_widget()` does, while
`show_widget()` is itself called back indirectly through a signal chain the property panel started.

**Path A -- adjusting any geometry field (X / Y / width / height / rotation / opacity)**

```text
QSpinBox.valueChanged
  -> PropertyEditor._set()                    # write back to the model
  -> PropertyEditor.changed signal
  → MainWindow._property_changed()
  -> item.setPos()                            # update the canvas
  → DesignerItem.itemChange(ItemPositionHasChanged)
  -> CanvasScene.widget_changed signal
  → MainWindow._canvas_widget_changed()
  → PropertyEditor.show_widget()
  -> _clear() -> QFormLayout.removeRow()      # <- deletes the QSpinBox still emitting
```

By the time control returns to `QAbstractSpinBox`'s mouse / keyboard handling, the object has
been freed. Confirmed in practice with `shiboken6.isValid()`: after one change to X, that
QSpinBox's C++ object really is destroyed while its signal is still on the stack.

**Path B -- unchecking "Visible"**

```text
QCheckBox.toggled → _set() → _property_changed()
  → item.setVisible(False)
  -> Qt force-deselects the hidden item
  → CanvasScene.selectionChanged → widget_selected(None)
  → MainWindow._selection_changed() → show_widget(None)
  -> _clear()                                 # <- deletes the QCheckBox still emitting toggled
```

The same mechanism caused another long-standing problem: once a widget was set invisible it
could never be selected on the canvas again, only from the layer list (the `if not
item.widget.visible` special case in `_layer_selected()` exists for exactly that).

### Fix

**`property_editor.py`**

- `_clear()` ([property_editor.py:77](property_editor.py#L77)) now uses
  `takeRow()` + `blockSignals(True)` + `deleteLater()`. Destruction is deferred to the event
  loop while the signals are cut immediately, so a dying control cannot write its old value
  into the newly selected widget in the meantime.
- `show_widget()` ([property_editor.py:105](property_editor.py#L105)) gains a layout key
  comparison (widget identity + type + the list of selectable action targets). **With the
  same widget and the same fields it only updates the values in place and never rebuilds the
  form**, removing most rebuild opportunities at the source.
- A new `_refresh_values()` ([property_editor.py:126](property_editor.py#L126)) and a
  dedicated refresher per field. A field being typed into (including the QLineEdit inside a
  spinbox, detected with `_has_focus()`) is skipped and the text under the cursor is not overwritten.
- `_set()` ([property_editor.py:283](property_editor.py#L283)) writes back the value clamped
  by `normalize()` after emitting `changed`, for cases such as a gauge's `value` exceeding `maximum`.

**`canvas.py`**

- A new `DesignerItem.apply_visibility()` ([canvas.py:59](canvas.py#L59)). In edit mode,
  `visible = False` only draws the widget faintly (opacity x0.25) and **does not call
  `hide()`**, so it triggers no chain of deselection and a hidden widget can still be
  selected and moved directly on the canvas. Only the interactive preview really hides it.
- `DesignerItem.__init__()` gains a `preview_mode` parameter so the right visibility rule
  applies at construction; `load_scene()` and `set_preview_mode()` are updated accordingly.

**`main.py`**

- A new `_applying_property` flag ([main.py:161](main.py#L161)). `_property_changed()`
  takes the lock while updating canvas items ([main.py:496](main.py#L496)), and
  `_selection_changed()` and `_canvas_widget_changed()` return immediately when they see it,
  cutting the "property panel -> canvas -> property panel" way back.
- `_property_changed()` now calls `item.apply_visibility()` in place of
  `setOpacity()` + `setVisible()`.
- `_update_warning()` ([main.py:470](main.py#L470)) is extracted, removing the duplicated
  round-overflow warning logic from `_selection_changed()` and `_canvas_widget_changed()`.

### User-visible behaviour changes

| Item | Before | After |
| --- | --- | --- |
| Adjusting a property value | the form rebuilt on every change, input focus was stolen, and it could crash at any moment | focus stays in the field; continuous fine-tuning and held arrow keys work |
| Unchecking "Visible" | the widget vanished from the canvas, the property panel was cleared, and it often crashed outright | the widget fades but stays on the canvas and can still be selected and edited |
| Interactive preview (F5) | -- | unchanged; `visible = False` still really hides |
| The generated C | -- | **entirely unchanged**; the output semantics of the `visible` field are not adjusted |

### Verification

- All 18 existing unittests pass (`python -m unittest discover -s tests`).
- An offscreen (`QT_QPA_PLATFORM=offscreen`) test exercised all 8 widget types x all 113
  property fields one by one, confirming no control is destroyed in place after an operation and no panel is rebuilt.
- All 27 scenario tests pass: 20 presses of the up key to nudge X, a widget still selectable
  after toggling visibility, an old input box not polluting a newly selected widget, canvas
  drag -> panel in sync, an out-of-range value clamped and written back, and the preview mode still hiding correctly.
- `sample_454.json` loads and every widget selects normally.

### Backups and recovery

The original files before the change are backed up in the same directory:

```text
property_editor.py.bak
canvas.py.bak
main.py.bak
```

They can be deleted once everything is confirmed; to recover, copy them back over.

### Notes for maintainers

When changing the property panel in future, hold to this rule:

> **Never rebuild or immediately delete a control from inside that control's own signal handling.**

Specifically:

1. Any code that clears the form goes through `_clear()`; do not use any immediate destruction
   other than it -- not `removeRow()` and not a direct `deleteLater()` elsewhere.
2. When adding a property field, `_create_editor()` must return both `(control, refresher)`;
   the refresher has to write the model value back without rebuilding the control, and skip it while it holds focus.
3. Any write from the property panel to the canvas that might trigger `itemChange()` belongs
   inside the protection of the `_applying_property` flag.
