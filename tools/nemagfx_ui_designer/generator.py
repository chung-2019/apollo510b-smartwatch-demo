"""Generate NemaGFX/NemaVG C99 sources and convert PNG assets."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import math
import re

import font_metrics
from model import UIProject, Widget, c_identifier, c_symbol

try:
    from PIL import Image
except ImportError:  # The GUI can still edit projects without asset conversion.
    Image = None  # type: ignore[assignment]


@dataclass
class GenerationResult:
    output_dir: Path
    files: list[Path] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    asset_bytes: int = 0


@dataclass
class Asset:
    key: str
    source: Path
    width: int
    height: int
    stride: int
    nema_format: str
    data: bytes


def rgba8888(color: str, opacity: int = 255) -> int:
    """Convert #RRGGBB[AA] into Nema's numeric 0xAABBGGRR representation."""
    value = color.strip().lstrip("#")
    if len(value) == 3:
        value = "".join(char * 2 for char in value) + "FF"
    elif len(value) == 6:
        value += "FF"
    if len(value) != 8 or not re.fullmatch(r"[0-9a-fA-F]{8}", value):
        raise ValueError(f"Invalid color '{color}', expected #RRGGBB or #RRGGBBAA")
    red, green, blue, alpha = (int(value[index:index + 2], 16) for index in range(0, 8, 2))
    alpha = (alpha * max(0, min(255, opacity)) + 127) // 255
    return (alpha << 24) | (blue << 16) | (green << 8) | red


def c_string(value: str) -> str:
    return (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def enum_name(scene_name: str) -> str:
    return "UI_SCENE_" + c_identifier(scene_name).upper()


def asset_key(source: Path) -> str:
    return c_identifier(source.stem, "image")


def _resolve_source(raw_source: str, project_file: Path | None) -> Path:
    source = Path(raw_source)
    if source.is_absolute():
        return source
    if project_file is not None:
        return (project_file.parent / source).resolve()
    return source.resolve()


def _convert_image(widget: Widget, source: Path) -> Asset:
    if Image is None:
        raise RuntimeError("Pillow is required to convert image assets: pip install Pillow")
    with Image.open(source) as opened:
        image = opened.convert("RGBA")
        if image.size != (widget.width, widget.height):
            image = image.resize((widget.width, widget.height), Image.Resampling.LANCZOS)
        requested = widget.image_format.lower()
        has_alpha = image.getextrema()[3][0] < 255
        use_rgba = requested == "rgba8888" or (requested == "auto" and has_alpha)
        if use_rgba:
            data = image.tobytes("raw", "RGBA")
            return Asset(asset_key(source), source, image.width, image.height,
                         image.width * 4, "NEMA_RGBA8888", data)

        rgb = image.convert("RGB")
        output = bytearray()
        raw_rgb = rgb.tobytes("raw", "RGB")
        for offset in range(0, len(raw_rgb), 3):
            red, green, blue = raw_rgb[offset:offset + 3]
            rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
            output.extend((rgb565 & 0xFF, (rgb565 >> 8) & 0xFF))
        return Asset(asset_key(source), source, rgb.width, rgb.height,
                     rgb.width * 2, "NEMA_RGB565", bytes(output))


def _collect_assets(project: UIProject, project_file: Path | None, warnings: list[str]) -> tuple[list[Asset], dict[str, str]]:
    assets: list[Asset] = []
    mapping: dict[str, str] = {}
    by_conversion: dict[tuple[Path, int, int, str], Asset] = {}
    used_keys: set[str] = set()
    for scene in project.scenes:
        for widget in scene.widgets:
            if widget.type != "image" or not widget.source:
                continue
            source = _resolve_source(widget.source, project_file)
            conversion_key = (source, widget.width, widget.height, widget.image_format.lower())
            if conversion_key in by_conversion:
                mapping[widget.id] = by_conversion[conversion_key].key
                continue
            if not source.exists():
                warnings.append(f"Image '{widget.name}' not found: {source}")
                continue
            try:
                asset = _convert_image(widget, source)
            except Exception as exc:
                warnings.append(f"Image '{widget.name}' conversion failed: {exc}")
                continue
            base = asset.key
            suffix = 2
            while asset.key in used_keys:
                asset.key = f"{base}_{suffix}"
                suffix += 1
            used_keys.add(asset.key)
            by_conversion[conversion_key] = asset
            mapping[widget.id] = asset.key
            assets.append(asset)
    return assets, mapping


def _byte_array(data: bytes) -> str:
    rows = []
    for offset in range(0, len(data), 16):
        rows.append("    " + ", ".join(f"0x{byte:02X}" for byte in data[offset:offset + 16]) + ",")
    return "\n".join(rows)


def _write(path: Path, content: str, result: GenerationResult) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content.rstrip() + "\n", encoding="utf-8", newline="\n")
    result.files.append(path)


def _generate_config(project: UIProject) -> str:
    settings = project.project
    guard = "UI_DESIGNER_PROJECT_CONFIG_H"
    color_format = {
        "RGB565": "NEMA_RGB565",
        "RGB24": "NEMA_RGB24",
        "RGBA8888": "NEMA_RGBA8888",
    }.get(settings.color_format.upper(), settings.color_format)
    return f"""// Auto-generated by Apollo510B NemaGFX UI Designer. Do not edit.
#ifndef {guard}
#define {guard}

#include \"nema_graphics.h\"

#define UI_DESIGNER_PROJECT_NAME       \"{c_string(settings.name)}\"
// The canvas is the circle the watch actually shows; the framebuffer is the panel's real size, with the canvas centred in it.
// The coordinates the generated code outputs already include ORIGIN, i.e. they are framebuffer coordinates, so callers need no conversion.
#define UI_DESIGNER_DISPLAY_WIDTH      ({settings.display_width})
#define UI_DESIGNER_DISPLAY_HEIGHT     ({settings.display_height})
#define UI_DESIGNER_FRAMEBUFFER_WIDTH  ({settings.framebuffer_width})
#define UI_DESIGNER_FRAMEBUFFER_HEIGHT ({settings.framebuffer_height})
#define UI_DESIGNER_ORIGIN_X           ({settings.origin_x})
#define UI_DESIGNER_ORIGIN_Y           ({settings.origin_y})
#define UI_DESIGNER_SAFE_MARGIN        ({settings.safe_margin})
#define UI_DESIGNER_SCENE_COUNT        ({len(project.scenes)})
#define UI_DESIGNER_COLOR_FORMAT       {color_format}

#endif // {guard}
"""


def _generate_assets_h(assets: list[Asset]) -> str:
    declarations = "\n".join(f"extern ui_designer_image_t g_ui_asset_{asset.key};" for asset in assets)
    return f"""// Auto-generated by Apollo510B NemaGFX UI Designer. Do not edit.
#ifndef UI_DESIGNER_ASSETS_H
#define UI_DESIGNER_ASSETS_H

#include <stdbool.h>
#include <stdint.h>
#include \"nema_hal.h\"

typedef struct
{{
    nema_buffer_t bo;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint32_t format;
}} ui_designer_image_t;

{declarations}

bool ui_designer_assets_init(void);
void ui_designer_assets_deinit(void);

#endif // UI_DESIGNER_ASSETS_H
"""


def _generate_assets_c(assets: list[Asset]) -> str:
    arrays: list[str] = []
    definitions: list[str] = []
    init_blocks: list[str] = []
    destroy_blocks: list[str] = []
    for asset in assets:
        arrays.append(f"""static const uint8_t s_ui_asset_{asset.key}_pixels[{len(asset.data)}]
    __attribute__((aligned(8))) =
{{
{_byte_array(asset.data)}
}};""")
        definitions.append(
            f"ui_designer_image_t g_ui_asset_{asset.key} = {{ .width = {asset.width}, .height = {asset.height}, "
            f".stride = {asset.stride}, .format = {asset.nema_format} }};"
        )
        init_blocks.append(f"""    g_ui_asset_{asset.key}.bo = nema_buffer_create_pool(
        NEMA_MEM_POOL_ASSETS, (int)sizeof(s_ui_asset_{asset.key}_pixels));
    if (g_ui_asset_{asset.key}.bo.base_virt == NULL)
    {{
        ui_designer_assets_deinit();
        return false;
    }}
    nema_memcpy(g_ui_asset_{asset.key}.bo.base_virt,
                (void *)s_ui_asset_{asset.key}_pixels,
                sizeof(s_ui_asset_{asset.key}_pixels));""")
        destroy_blocks.append(f"""    if (g_ui_asset_{asset.key}.bo.base_virt != NULL)
    {{
        nema_buffer_destroy(&g_ui_asset_{asset.key}.bo);
        g_ui_asset_{asset.key}.bo.base_virt = NULL;
    }}""")
    if not assets:
        init_blocks.append("    // No image assets in this project.")
        destroy_blocks.append("    // No image assets in this project.")
    # nema_utils.h is where nema_memcpy() is declared, and neither nema_core.h nor
    # nema_graphics.h pulls it in (the chain stops at nema_hal.h -> nema_sys_defs.h).
    # Without this line, ui_designer_assets_init() reports an implicit declaration under -Wall.
    return f"""// Auto-generated by Apollo510B NemaGFX UI Designer. Do not edit.
#include \"ui_designer_assets.h\"
#include \"nema_core.h\"
#include \"nema_graphics.h\"
#include \"nema_utils.h\"

{chr(10).join(arrays)}

{chr(10).join(definitions)}

bool
ui_designer_assets_init(void)
{{
{chr(10).join(init_blocks)}
    return true;
}}

void
ui_designer_assets_deinit(void)
{{
{chr(10).join(destroy_blocks)}
}}
"""


def _binding_fields(project: UIProject) -> tuple[list[str], list[str], list[str]]:
    fields: list[str] = []
    declarations: list[str] = []
    definitions: list[str] = []
    seen: set[tuple[str, str]] = set()
    for scene in project.scenes:
        for widget in scene.widgets:
            if not widget.binding:
                continue
            key = c_identifier(widget.binding, "binding")
            kind = "text" if widget.type in ("label", "button") else (
                "value" if widget.type in ("arc", "gauge") else "visible")
            if (key, kind) in seen:
                continue
            seen.add((key, kind))
            field_name = f"{key}_{kind}"
            if kind == "text":
                fields.append(f"    char {field_name}[96];")
                declarations.append(f"void ui_binding_set_{key}_text(const char *value);")
                definitions.append(f"""void
ui_binding_set_{key}_text(const char *value)
{{
    if (value == NULL)
    {{
        g_ui_designer_state.{field_name}[0] = '\\0';
        return;
    }}
    (void)snprintf(g_ui_designer_state.{field_name}, sizeof(g_ui_designer_state.{field_name}), \"%s\", value);
}}""")
            elif kind == "value":
                fields.append(f"    float {field_name};")
                declarations.append(f"void ui_binding_set_{key}_value(float value);")
                definitions.append(f"""void
ui_binding_set_{key}_value(float value)
{{
    g_ui_designer_state.{field_name} = value;
}}""")
            else:
                fields.append(f"    bool {field_name};")
                declarations.append(f"void ui_binding_set_{key}_visible(bool visible);")
                definitions.append(f"""void
ui_binding_set_{key}_visible(bool visible)
{{
    g_ui_designer_state.{field_name} = visible;
}}""")
    if not fields:
        fields.append("    uint8_t unused;")
    return fields, declarations, definitions


def _generate_bindings(project: UIProject) -> tuple[str, str]:
    fields, declarations, definitions = _binding_fields(project)
    initializers: list[str] = []
    seen: set[tuple[str, str]] = set()
    for scene in project.scenes:
        for widget in scene.widgets:
            if not widget.binding:
                continue
            key = c_identifier(widget.binding, "binding")
            kind = "text" if widget.type in ("label", "button") else (
                "value" if widget.type in ("arc", "gauge") else "visible")
            if (key, kind) in seen:
                continue
            seen.add((key, kind))
            field_name = f"{key}_{kind}"
            if kind == "text":
                initializers.append(f'    .{field_name} = "{c_string(widget.text)}",')
            elif kind == "value":
                initializers.append(f"    .{field_name} = {widget.value:.3f}f,")
            else:
                initializers.append(f"    .{field_name} = true,")
    if not initializers:
        initializers.append("    .unused = 0U,")
    header = f"""// Auto-generated by Apollo510B NemaGFX UI Designer. Do not edit.
#ifndef UI_DESIGNER_BINDINGS_H
#define UI_DESIGNER_BINDINGS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{{
{chr(10).join(fields)}
}} ui_designer_state_t;

extern ui_designer_state_t g_ui_designer_state;

{chr(10).join(declarations)}

#endif // UI_DESIGNER_BINDINGS_H
"""
    source = f"""// Auto-generated by Apollo510B NemaGFX UI Designer. Do not edit.
#include \"ui_designer_bindings.h\"
#include <stdio.h>

ui_designer_state_t g_ui_designer_state =
{{
{chr(10).join(initializers)}
}};

{chr(10).join(definitions)}
"""
    return header, source


def _widget_visible_expr(widget: Widget) -> str:
    if widget.binding and widget.type not in ("label", "button", "arc", "gauge"):
        return f"g_ui_designer_state.{c_identifier(widget.binding)}_visible"
    return "true" if widget.visible else "false"


def _widget_text_expr(widget: Widget) -> str:
    if widget.binding and widget.type in ("label", "button"):
        return f"g_ui_designer_state.{c_identifier(widget.binding)}_text"
    return f'"{c_string(widget.text)}"'


def _widget_value_expr(widget: Widget) -> str:
    if widget.binding and widget.type in ("arc", "gauge"):
        return f"g_ui_designer_state.{c_identifier(widget.binding)}_value"
    return f"{widget.value:.3f}f"


def _text_align(widget: Widget) -> str:
    horizontal = {
        "left": "NEMA_VG_ALIGNX_LEFT",
        "center": "NEMA_VG_ALIGNX_CENTER",
        "right": "NEMA_VG_ALIGNX_RIGHT",
    }.get(widget.align, "NEMA_VG_ALIGNX_CENTER")
    return f"{horizontal} | NEMA_VG_ALIGNY_CENTER"


def _render_widget(widget: Widget, assets: dict[str, str], rotation_expr: str | None = None,
                   origin: tuple[int, int] = (0, 0)) -> str:
    #
    # Widget coordinates are canvas coordinates, and the canvas's origin on the
    # framebuffer is added once here, so every drawing call below is in framebuffer
    # coordinates. Added here rather than on every line, because centres and endpoints
    # are all derived from x/y, and missing one would misplace exactly one primitive.
    #
    x, y = widget.x + origin[0], widget.y + origin[1]
    width, height = widget.width, widget.height
    color = f"0x{rgba8888(widget.color, widget.opacity):08X}UL"
    background = f"0x{rgba8888(widget.background, widget.opacity):08X}UL"
    border = f"0x{rgba8888(widget.border_color, widget.opacity):08X}UL"
    visible = _widget_visible_expr(widget)
    lines = [f"    // {widget.type}: {widget.name}", f"    if ({visible})", "    {"]
    indent = "        "

    if widget.type == "rectangle":
        lines.append(f"{indent}nema_set_blend_fill(NEMA_BL_SIMPLE);")
        if widget.radius:
            lines.append(f"{indent}nema_fill_rounded_rect({x}, {y}, {width}, {height}, {widget.radius}, {background});")
        else:
            lines.append(f"{indent}nema_fill_rect({x}, {y}, {width}, {height}, {background});")
        if widget.border_width:
            lines.append(f"{indent}nema_draw_rounded_rect_aa({x}.0f, {y}.0f, {width}.0f, {height}.0f, {widget.radius}.0f, {widget.border_width}.0f, {border});")
    elif widget.type == "button":
        lines.extend([
            f"{indent}nema_set_blend_fill(NEMA_BL_SIMPLE);",
            f"{indent}nema_fill_rounded_rect({x}, {y}, {width}, {height}, {widget.radius}, {background});",
        ])
        if widget.border_width:
            lines.append(f"{indent}nema_draw_rounded_rect_aa({x}.0f, {y}.0f, {width}.0f, {height}.0f, {widget.radius}.0f, {widget.border_width}.0f, {border});")
        lines.append(f"{indent}ui_designer_draw_text({_widget_text_expr(widget)}, {x}, {y}, {width}, {height}, {widget.font_size}.0f, {color}, {_text_align(widget)});")
    elif widget.type == "label":
        lines.append(f"{indent}ui_designer_draw_text({_widget_text_expr(widget)}, {x}, {y}, {width}, {height}, {widget.font_size}.0f, {color}, {_text_align(widget)});")
    elif widget.type == "circle":
        radius = min(width, height) / 2.0
        cx, cy = x + width / 2.0, y + height / 2.0
        lines.extend([
            f"{indent}nema_set_blend_fill(NEMA_BL_SIMPLE);",
            f"{indent}nema_fill_circle_aa({cx:.3f}f, {cy:.3f}f, {radius:.3f}f, {background});",
        ])
        if widget.border_width:
            border_radius = max(0.5, radius - widget.border_width / 2.0)
            lines.append(f"{indent}nema_draw_circle_aa({cx:.3f}f, {cy:.3f}f, {border_radius:.3f}f, {widget.border_width:.3f}f, {border});")
    elif widget.type == "line":
        lines.append(f"{indent}nema_draw_line_aa({x}.0f, {y}.0f, {x + width}.0f, {y + height}.0f, {widget.stroke_width:.3f}f, {color});")
    elif widget.type in ("arc", "gauge"):
        cx, cy = x + width / 2.0, y + height / 2.0
        radius = max(1.0, min(width, height) / 2.0 - widget.stroke_width / 2.0)
        if widget.type == "gauge":
            lines.extend([
                f"{indent}ui_designer_draw_ring({cx:.3f}f, {cy:.3f}f, {radius:.3f}f, {widget.start_angle:.3f}f, {widget.end_angle:.3f}f, {widget.stroke_width:.3f}f, {background});",
                f"{indent}ui_designer_draw_gauge({cx:.3f}f, {cy:.3f}f, {radius:.3f}f, {widget.start_angle:.3f}f, {widget.end_angle:.3f}f, {widget.stroke_width:.3f}f, {widget.minimum:.3f}f, {widget.maximum:.3f}f, {_widget_value_expr(widget)}, {color});",
            ])
        else:
            lines.append(f"{indent}ui_designer_draw_ring({cx:.3f}f, {cy:.3f}f, {radius:.3f}f, {widget.start_angle:.3f}f, {widget.end_angle:.3f}f, {widget.stroke_width:.3f}f, {color});")
    elif widget.type == "image":
        key = assets.get(widget.id)
        if key:
            lines.extend([
                f"{indent}ui_designer_blit_rotated(&g_ui_asset_{key}, {x}.0f, {y}.0f,",
                f"{indent}                            {width}.0f, {height}.0f,",
                f"{indent}                            {rotation_expr or '0.0f'});",
            ])
        else:
            lines.append(f"{indent}// Image source missing or conversion failed; placeholder omitted.")
    lines.append("    }")
    return "\n".join(lines)


def _event_names(project: UIProject) -> list[str]:
    names: list[str] = []
    seen: set[str] = set()
    for scene in project.scenes:
        for widget in scene.widgets:
            if widget.type == "button" and widget.event:
                name = c_identifier(widget.event, f"{widget.name}_clicked")
                if name not in seen:
                    names.append(name)
                    seen.add(name)
    return names


def _generate_ui_h(project: UIProject) -> str:
    enums = ",\n".join(f"    {enum_name(scene.name)} = {index}" for index, scene in enumerate(project.scenes))
    events = "\n".join(f"void ui_event_{name}(void);" for name in _event_names(project))
    first_scene = enum_name(project.scenes[0].name) if project.scenes else "UI_SCENE_COUNT"
    return f"""// Auto-generated by Apollo510B NemaGFX UI Designer. Do not edit.
#ifndef UI_DESIGNER_GENERATED_H
#define UI_DESIGNER_GENERATED_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{{
{enums},
    UI_SCENE_COUNT = {len(project.scenes)}
}} ui_scene_id_t;

//
// The first scene. Projects with only one scene use this, so the caller need not
// hard-code the scene's name -- after a rename in the Designer, a hard-coded enum value becomes a compile error.
//
#define UI_SCENE_DEFAULT               ({first_scene})

bool ui_designer_init(void);
void ui_designer_deinit(void);

//
// There are two ways to feed the animation time; use one or the other:
//
//   ui_designer_tick_to()  pass "what time it is now in ms", and the difference
//                          between calls is computed here. The first call only records the base and does not advance the animation.
//   ui_designer_tick()     pass "how many ms since last time", with the caller keeping the previous time.
//
// tick_to() is the usual choice: the caller needs no extra variable to compute the
// difference, and there is no "that variable was never reset" bug visible only in a particular order of operations.
//
void ui_designer_tick(uint32_t delta_ms);
void ui_designer_tick_to(uint32_t now_ms);

//
// Pull tick_to()'s time base up to now_ms without advancing the animation.
//
// Used when "this screen was away for a while and came back" (switching away and back,
// for instance): without the reset, the first frame back would take that whole absence as the delta and the running animation would jump to its end.
//
void ui_designer_resync(uint32_t now_ms);

//
// When uninitialized (ui_designer_init() failed or was never called) it only fills the
// canvas with that scene's background colour and returns. Callers therefore need no guard -- no image from the previous page is left behind.
//
void ui_designer_draw(ui_scene_id_t scene);

//
// Always returns false when uninitialized. Hit testing touches no drawing resources, so it is safe to call at any time.
//
bool ui_designer_touch(ui_scene_id_t scene, int32_t x, int32_t y);

{events}

#endif // UI_DESIGNER_GENERATED_H
"""


# ---------------------------------------------------------------------------
# Drawing helpers.
#
# All of them are static, so "defined but never called" is an unused-function warning
# under -Wall -- a project holding only Labels and Images used to emit gauge/ring warnings
# on every Generate C. Now only what is actually used is emitted, so they are split into
# individual constants and picked by _generate_ui_c() from the generated function bodies.
#
# These are plain strings rather than f-strings, so C's braces need no escaping.
# ---------------------------------------------------------------------------
_HELPER_START_ROTATION = """static void
ui_designer_start_rotation(float *target, bool relative, float angle,
                           uint32_t duration_ms, ui_easing_t easing)
{
    float destination;
    if (target == NULL)
    {
        return;
    }
    destination = relative ? (*target + angle) : angle;
    if (duration_ms == 0U)
    {
        *target = destination;
        s_rotation_animation.active = false;
        return;
    }
    s_rotation_animation.active = true;
    s_rotation_animation.target = target;
    s_rotation_animation.from = *target;
    s_rotation_animation.to = destination;
    s_rotation_animation.elapsed_ms = 0U;
    s_rotation_animation.duration_ms = duration_ms;
    s_rotation_animation.easing = easing;
}"""

#
# tick is public API (declared in the .h) and must be emitted even with no animation, or the caller cannot link.
#
_HELPER_TICK = """void
ui_designer_tick(uint32_t delta_ms)
{
    float ratio;
    float eased;
    if (!s_rotation_animation.active || (s_rotation_animation.target == NULL))
    {
        return;
    }
    if (delta_ms >= (s_rotation_animation.duration_ms - s_rotation_animation.elapsed_ms))
    {
        s_rotation_animation.elapsed_ms = s_rotation_animation.duration_ms;
    }
    else
    {
        s_rotation_animation.elapsed_ms += delta_ms;
    }
    ratio = (float)s_rotation_animation.elapsed_ms /
            (float)s_rotation_animation.duration_ms;
    eased = (s_rotation_animation.easing == UI_EASING_IN_OUT)
                ? (ratio * ratio * (3.0f - (2.0f * ratio)))
                : ratio;
    *s_rotation_animation.target = s_rotation_animation.from +
        ((s_rotation_animation.to - s_rotation_animation.from) * eased);
    if (s_rotation_animation.elapsed_ms >= s_rotation_animation.duration_ms)
    {
        s_rotation_animation.active = false;
    }
}

void
ui_designer_resync(uint32_t now_ms)
{
    s_tick_last_ms = now_ms;
    s_tick_primed = true;
}

void
ui_designer_tick_to(uint32_t now_ms)
{
    uint32_t delta_ms;

    //
    // The first call has no "last time" to subtract and only records the base. Without
    // this step the first frame after boot would take the whole of now_ms as the delta
    // and any running animation would jump straight to its end.
    //
    if (!s_tick_primed)
    {
        ui_designer_resync(now_ms);
        return;
    }

    //
    // Unsigned subtraction, which still gives the correct difference when the timer wraps.
    //
    delta_ms = now_ms - s_tick_last_ms;
    s_tick_last_ms = now_ms;
    ui_designer_tick(delta_ms);
}"""

_HELPER_BLIT_ROTATED = """static void
ui_designer_blit_rotated(const ui_designer_image_t *image, float x, float y,
                         float width, float height, float angle)
{
    float x0 = -width * 0.5f;
    float y0 = -height * 0.5f;
    float x1 =  width * 0.5f;
    float y1 = -height * 0.5f;
    float x2 =  width * 0.5f;
    float y2 =  height * 0.5f;
    float x3 = -width * 0.5f;
    float y3 =  height * 0.5f;
    nema_matrix3x3_t matrix;

    nema_mat3x3_load_identity(matrix);
    //
    // A positive angle is clockwise, matching the Designer preview (QGraphicsItem::setRotation()).
    //
    // No minus sign belongs here. Compare the watch face examples in this SDK: the
    // hands' angle increases with time (positive), draw_hand() is called with one
    // negation and the function negates again internally, so what reaches
    // nema_mat3x3_rotate() is positive -- and on hardware the hands run clockwise.
    //
    nema_mat3x3_rotate(matrix, angle);
    nema_mat3x3_translate(matrix, x + (width * 0.5f), y + (height * 0.5f));
    nema_mat3x3_mul_vec(matrix, &x0, &y0);
    nema_mat3x3_mul_vec(matrix, &x1, &y1);
    nema_mat3x3_mul_vec(matrix, &x2, &y2);
    nema_mat3x3_mul_vec(matrix, &x3, &y3);

    nema_bind_src_tex(image->bo.base_phys, image->width, image->height,
                      image->format, image->stride, NEMA_FILTER_BL);
    nema_set_blend_blit(NEMA_BL_SIMPLE);
    nema_blit_quad_fit(x0, y0, x1, y1, x2, y2, x3, y3);
}"""

_HELPER_DRAW_TEXT = """//
// The width limit passed to nema_vg_string_get_bbox() when measuring text.
//
// That function clamps the width it returns to max_w, so the measurement must not be
// given the widget's box width -- that would make "this line is wider than the box"
// impossible to detect. The value only has to be large enough never to clamp any line; a few times the framebuffer width is used.
//
#define UI_TEXT_MEASURE_MAX_W   (8192.0f)

//
// The margin left when shrinking the font size.
//
// The measurement and nema_vg_print()'s internal rounding do not agree exactly, and
// shrinking to "exactly the box width" can still clip half a stroke off the last letter. A 2% margin is invisible but absorbs that error.
//
#define UI_TEXT_FIT_RATIO       (0.98f)

static void
ui_designer_draw_text(const char *text, int32_t x, int32_t y, int32_t width,
                      int32_t height, float size, uint32_t color, uint32_t align)
{
    float measured_w = 0.0f;
    float measured_h = 0.0f;

    //
    // Both blend and fill rule are VG context state preserved across command lists
    // rather than reset for each draw. Both are set here, so the appearance of the text
    // does not depend on what was drawn earlier in the same context -- glyphs are closed
    // outlines meant to be filled, and leaving a NEMA_VG_STROKE set elsewhere would only
    // outline them into hollow text.
    //
    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_FILL_NON_ZERO);
    nema_vg_paint_set_paint_color(s_ui_paint, color);

    //
    // If the measurement is wider than the box it shrinks proportionally, or nema_vg_print() would simply clip the overflow.
    //
    // The measurement's max_w must be UI_TEXT_MEASURE_MAX_W and never the widget's box
    // width: nema_vg_string_get_bbox() clamps the width it returns to max_w, so with the
    // box width passed in, measured_w could never exceed width, this if could never hold
    // -- the size would not shrink, hardware would still clip it, while the Designer
    // preview (using its own advance table, with no such limit) shows it shrunk, and
    // that is exactly why the two disagree.
    //
    // Why this is necessary: the Designer's canvas preview lays text out with a system
    // font on the PC, while hardware uses the NemaVG font configured for the project, and
    // the widths differ at the same size -- a line that clearly fits on the canvas loses
    // its last letter or two once flashed. This decides whether to shrink from NemaVG's
    // own measurement, so it does not depend on relating the two widths at all.
    //
    // No re-measurement after shrinking: text width is proportional to the size, so one pass lands inside the box.
    //
    // When the measurement fails (a problem with the font or the VG context) measured_w is
    // 0; nothing is shrunk then and it is drawn at the original size -- at least it is still a line of text, rather than infinite scaling from a 0.
    //
    nema_vg_set_font_size(size);
    (void)nema_vg_string_get_bbox(text, &measured_w, &measured_h,
                                  UI_TEXT_MEASURE_MAX_W, 0);
    if ((measured_w > (float)width) && (measured_w > 1.0f) && (width > 0))
    {
        nema_vg_set_font_size(size * UI_TEXT_FIT_RATIO * ((float)width / measured_w));
    }

    nema_vg_print(s_ui_paint, text, (float)x, (float)y, (float)width, (float)height,
                  align, NULL);
}"""

_HELPER_DRAW_RING = """static void
ui_designer_draw_ring(float cx, float cy, float radius, float start, float end,
                      float width, uint32_t color)
{
    nema_vg_set_blend(NEMA_BL_SIMPLE);
    nema_vg_set_fill_rule(NEMA_VG_STROKE);
    nema_vg_stroke_set_width(width);
    nema_vg_stroke_set_cap_style(NEMA_VG_CAP_ROUND, NEMA_VG_CAP_ROUND);
    nema_vg_paint_set_paint_color(s_ui_paint, color);
    if (end <= 360.0f)
    {
        (void)nema_vg_draw_ring(cx, cy, radius, start, end, s_ui_paint);
    }
    else
    {
        (void)nema_vg_draw_ring(cx, cy, radius, start, 360.0f, s_ui_paint);
        (void)nema_vg_draw_ring(cx, cy, radius, 0.0f, end - 360.0f, s_ui_paint);
    }
}"""

_HELPER_DRAW_GAUGE = """static void
ui_designer_draw_gauge(float cx, float cy, float radius, float start, float end,
                       float width, float minimum, float maximum, float value,
                       uint32_t color)
{
    float ratio;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    ratio = (value - minimum) / (maximum - minimum);
    ui_designer_draw_ring(cx, cy, radius, start, start + ((end - start) * ratio),
                          width, color);
}"""


def _select_helpers(scene_body: str, touch_body: str) -> str:
    """Pick out the drawing helpers this project actually calls.

    The test is whether a call appears in the generated C body, rather than deriving it
    from the widget types again -- the call sites are scattered across several branches of
    _render_widget() (a gauge also brings in a ring, for instance), and deriving from
    widget types easily drifts from the actual output.

    The returned order is the order written to the file, and dependencies have to come
    first: draw_gauge calls draw_ring.
    """
    parts: list[str] = []
    if "ui_designer_start_rotation(" in touch_body:
        parts.append(_HELPER_START_ROTATION)
    parts.append(_HELPER_TICK)
    if "ui_designer_blit_rotated(" in scene_body:
        parts.append(_HELPER_BLIT_ROTATED)
    if "ui_designer_draw_text(" in scene_body:
        parts.append(_HELPER_DRAW_TEXT)
    gauge_used = "ui_designer_draw_gauge(" in scene_body
    if gauge_used or ("ui_designer_draw_ring(" in scene_body):
        parts.append(_HELPER_DRAW_RING)
    if gauge_used:
        parts.append(_HELPER_DRAW_GAUGE)
    return "\n\n".join(parts)


def _generate_ui_c(project: UIProject, assets: dict[str, str]) -> str:
    origin = (project.project.origin_x, project.project.origin_y)
    scene_functions: list[str] = []
    scene_backgrounds: list[str] = []
    switch_cases: list[str] = []
    touch_cases: list[str] = []
    rotation_vars: dict[str, str] = {}
    rotation_definitions: list[str] = []
    rotation_resets: list[str] = []
    used_rotation_names: set[str] = set()
    for scene in project.scenes:
        for widget in scene.widgets:
            if widget.type != "image":
                continue
            base = f"s_rotation_{c_identifier(scene.name)}_{c_identifier(widget.name)}"
            variable = base
            suffix = 2
            while variable in used_rotation_names:
                variable = f"{base}_{suffix}"
                suffix += 1
            used_rotation_names.add(variable)
            rotation_vars[widget.id] = variable
            rotation_definitions.append(f"static float {variable} = {widget.rotation:.3f}f;")
            rotation_resets.append(f"    {variable} = {widget.rotation:.3f}f;")

    for scene in project.scenes:
        scene_id = c_identifier(scene.name)
        widgets = "\n\n".join(
            _render_widget(widget, assets, rotation_vars.get(widget.id), origin)
            for widget in scene.widgets)
        scene_backgrounds.append(
            f"    [{enum_name(scene.name)}] = 0x{rgba8888(scene.background):08X}UL,")
        scene_functions.append(f"""static void
ui_scene_{scene_id}_draw(void)
{{
    ui_designer_fill_background({enum_name(scene.name)});

{widgets if widgets else '    // Empty scene.'}
}}""")
        switch_cases.append(f"        case {enum_name(scene.name)}: ui_scene_{scene_id}_draw(); break;")

        buttons = [widget for widget in reversed(scene.widgets)
                   if widget.type == "button" and (widget.event or widget.action != "none")
                   and widget.visible]
        checks: list[str] = []
        for widget in buttons:
            calls: list[str] = []
            if widget.action in ("rotate_by", "rotate_to") and widget.action_target:
                target = next((candidate for candidate in scene.widgets
                               if candidate.name == widget.action_target and candidate.type == "image"), None)
                if target is not None:
                    easing = "UI_EASING_LINEAR" if widget.easing == "linear" else "UI_EASING_IN_OUT"
                    relative = "true" if widget.action == "rotate_by" else "false"
                    calls.append(
                        f"ui_designer_start_rotation(&{rotation_vars[target.id]}, {relative}, "
                        f"{widget.action_value:.3f}f, {widget.duration_ms}U, {easing});")
            if widget.event:
                event = c_identifier(widget.event, f"{widget.name}_clicked")
                calls.append(f"ui_event_{event}();")
            hit_x = widget.x + origin[0]
            hit_y = widget.y + origin[1]
            checks.append(f"""            if ((x >= {hit_x}) && (x < {hit_x + widget.width}) &&
                (y >= {hit_y}) && (y < {hit_y + widget.height}))
            {{
                {chr(10).join('                ' + call for call in calls).lstrip()}
                return true;
            }}""")
        touch_cases.append(f"""        case {enum_name(scene.name)}:
{chr(10).join(checks) if checks else '            break;'}
            break;""")

    weak_callbacks = "\n\n".join(f"""__attribute__((weak)) void
ui_event_{name}(void)
{{
}}""" for name in _event_names(project))

    font_include = c_string(project.project.font_header)
    font_symbol = c_symbol(project.project.font_symbol, "DejaVuSans_ttf_kern")

    #
    # Only the helpers actually used are emitted, avoiding unused-function warnings under -Wall.
    #
    scene_body = chr(10).join(scene_functions)
    helper_block = _select_helpers(scene_body, chr(10).join(touch_cases))

    return f"""// Auto-generated by Apollo510B NemaGFX UI Designer. Do not edit.
#include \"ui_designer_generated.h\"
#include \"ui_designer_project_config.h\"
#include \"ui_designer_assets.h\"
#include \"ui_designer_bindings.h\"
#include \"nema_core.h\"
#include \"nema_graphics.h\"
#include \"nema_provisional.h\"
#include \"nema_vg.h\"
#include \"nema_vg_font.h\"
#include \"nema_vg_paint.h\"
#include \"{font_include}\"

static NEMA_VG_PAINT_HANDLE s_ui_paint;

typedef enum
{{
    UI_EASING_LINEAR = 0,
    UI_EASING_IN_OUT
}} ui_easing_t;

typedef struct
{{
    bool active;
    float *target;
    float from;
    float to;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    ui_easing_t easing;
}} ui_rotation_animation_t;

static ui_rotation_animation_t s_rotation_animation;
{chr(10).join(rotation_definitions)}

//
// Time base of ui_designer_tick_to(). primed being false means there is no "last
// time" to subtract yet, so the first call only records and does not advance.
//
static uint32_t s_tick_last_ms;
static bool s_tick_primed;

//
// The background colour of each scene. A separate table rather than hard-coded in
// each draw, because ui_designer_draw() has to be able to fill the canvas even when
// uninitialized, and that path never reaches a scene's draw function.
//
static const uint32_t s_ui_scene_background[UI_SCENE_COUNT] =
{{
{chr(10).join(scene_backgrounds)}
}};

static void
ui_designer_fill_background(ui_scene_id_t scene)
{{
    uint32_t color = (scene < UI_SCENE_COUNT) ? s_ui_scene_background[scene]
                                              : 0xFF000000UL;
    nema_set_blend_fill(NEMA_BL_SRC);
    //
    // It fills the whole framebuffer rather than the canvas: when the canvas is smaller
    // than the framebuffer, filling only the canvas leaves the previous page's image in
    // the outer ring -- mostly hidden by the bezel, but still visible on both sides of the arc.
    //
    nema_fill_rect(0, 0, UI_DESIGNER_FRAMEBUFFER_WIDTH, UI_DESIGNER_FRAMEBUFFER_HEIGHT,
                   color);
}}

{helper_block}

{scene_body}

bool
ui_designer_init(void)
{{
{chr(10).join(rotation_resets)}
    s_rotation_animation.active = false;
    s_rotation_animation.target = NULL;
    s_tick_primed = false;
    s_ui_paint = nema_vg_paint_create();
    if (s_ui_paint == NULL)
    {{
        return false;
    }}
    nema_vg_paint_set_type(s_ui_paint, NEMA_VG_PAINT_COLOR);
    nema_vg_bind_font(&{font_symbol});
    if (!ui_designer_assets_init())
    {{
        nema_vg_paint_destroy(s_ui_paint);
        s_ui_paint = NULL;
        return false;
    }}
    return true;
}}

void
ui_designer_deinit(void)
{{
    ui_designer_assets_deinit();
    if (s_ui_paint != NULL)
    {{
        nema_vg_paint_destroy(s_ui_paint);
        s_ui_paint = NULL;
    }}
}}

void
ui_designer_draw(ui_scene_id_t scene)
{{
    if (s_ui_paint == NULL)
    {{
        //
        // Not initialized yet, or ui_designer_init() failed. The canvas is still filled
        // before returning: returning directly would leave whatever the caller drew last
        // time, which looks like the whole system hanging rather than this one page not
        // coming up. Text and vector graphics cannot be drawn (their colour comes from
        // this very paint), so only the background colour remains.
        //
        ui_designer_fill_background(scene);
        return;
    }}
    nema_vg_bind_font(&{font_symbol});
    switch (scene)
    {{
{chr(10).join(switch_cases)}
        default: break;
    }}
}}

bool
ui_designer_touch(ui_scene_id_t scene, int32_t x, int32_t y)
{{
    //
    // When uninitialized the screen has nothing but the background and nothing to tap, so
    // no hit testing is done. Without this, invisible buttons would still fire callbacks.
    //
    if (s_ui_paint == NULL)
    {{
        return false;
    }}
    switch (scene)
    {{
{chr(10).join(touch_cases)}
        default: break;
    }}
    return false;
}}

{weak_callbacks}
"""


def _generate_makefile_fragment() -> str:
    """Produce a .mk fragment that gcc/Makefile can include.

    The directory derives itself from $(MAKEFILE_LIST) rather than a hard-coded relative
    path: this fragment is the last makefile included, so lastword is the path it was
    included by. Moving the output directory, or including it from another Makefile,
    therefore needs no change here.

    The benefit is that when the generator later emits more .c files (split assets, for
    instance), the project's Makefile needs no edit at all -- the source list is maintained here.
    """
    return """# Auto-generated by Apollo510B NemaGFX UI Designer. Do not edit.
#
# Add this line to gcc/Makefile after the SRC block and before CSRC is expanded:
#
#     include ../src/generated/ui_designer/ui_designer.mk
#
# Adjust the path to the actual output directory; the fragment itself does not depend on it.

# The directory this fragment lives in (without the trailing slash).
UI_DESIGNER_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

SRC      += ui_designer_generated.c
SRC      += ui_designer_assets.c
SRC      += ui_designer_bindings.c
VPATH    +=:$(UI_DESIGNER_DIR)
INCLUDES += -I$(UI_DESIGNER_DIR)
"""


def _previous_event_names(target: Path) -> set[str] | None:
    """Read the project snapshot the last Generate C left in the output directory and take its button callback names.

    None means there is nothing to compare against: either the first generation, or the
    snapshot could not be read (hand-edited, a different version). That case must not
    report "the symbol was renamed", because there is no way to know what it was.
    """
    snapshot = target / "ui_project.generated.json"
    if not snapshot.exists():
        return None
    try:
        return set(_event_names(UIProject.load(snapshot)))
    except Exception:
        return None


def _warn_event_symbol_changes(previous: set[str] | None, project: UIProject,
                               warnings: list[str]) -> None:
    """Warn when a button callback's symbol name differs from last time.

    The symbol name comes from the button's "click event" field (falling back to
    <button name>_clicked only when that field is empty, see _event_names()). An override
    written in the user's project shadows a weak definition -- when the names do not match,
    the linker says nothing at all, the override simply becomes a function nobody calls,
    and the button does nothing when pressed. This is the only place this tool can fail
    silently, so it is said out loud.
    """
    if previous is None:
        return
    current = set(_event_names(project))
    removed = sorted(previous - current)
    added = sorted(current - previous)
    if removed:
        warnings.append(
            "These button callbacks no longer exist: "
            + ", ".join(f"ui_event_{name}()" for name in removed)
            + ". Any override you wrote for them is now an uncalled function - "
              "the linker will not report it. Rename the override to match, or "
              "delete it."
        )
    if added:
        warnings.append(
            "New button callbacks: "
            + ", ".join(f"ui_event_{name}()" for name in added)
            + ". Override them in your own .c file to react to the button."
        )


def _generate_integration(project: UIProject, assets: list[Asset]) -> str:
    events = _event_names(project)
    if events:
        event_list = "\n".join(f"- `ui_event_{name}(void)`" for name in events)
    else:
        event_list = "- (this project has no button event configured)"
    return f"""# Generated UI integration

Generated for **{project.project.display_width}×{project.project.display_height}** `{project.project.shape}` display.

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
{{
    am_util_debug_printf("UI Designer init failed\\n");
}}
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

The coordinate system is the framebuffer's {project.project.framebuffer_width}x{project.project.framebuffer_height}: the Designer's
{project.project.display_width}x{project.project.display_height} canvas is already centred at ({project.project.origin_x}, {project.project.origin_y}) at code generation time,
and both drawing and hit testing include that offset, so the caller can pass the raw touch panel coordinates directly.

## 5. Button callback

The weak callbacks produced by this generation:

{event_list}

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

- Converted image payload: {sum(len(asset.data) for asset in assets)} bytes.
- Image assets are copied to `NEMA_MEM_POOL_ASSETS` during `ui_designer_init()`.
- Keep the generated font include and symbol synchronized with the project settings.
"""


def generate_project(project: UIProject, output_dir: str | Path,
                     project_file: str | Path | None = None) -> GenerationResult:
    project.ensure_valid()
    target = Path(output_dir).resolve()
    target.mkdir(parents=True, exist_ok=True)
    result = GenerationResult(target)

    #
    # Read it before overwriting; this snapshot is about to be replaced by the current contents.
    #
    previous_events = _previous_event_names(target)

    project_path = Path(project_file).resolve() if project_file else None

    #
    # The advance table is used to warn that "this line will be shrunk on hardware". It is
    # searched for upward from the output directory first, and from the project file's
    # location second -- the two are not necessarily in the same tree (they are not when
    # generating into a directory outside the repo).
    # With neither found the warning is skipped: code generation does not depend on it, and
    # the hardware side still has ui_designer_draw_text()'s automatic shrink as a backstop.
    #
    metrics = font_metrics.load(project.project.font_header, target)
    if metrics is None and project_path is not None:
        metrics = font_metrics.load(project.project.font_header, project_path)
    assets, asset_mapping = _collect_assets(project, project_path, result.warnings)
    _warn_event_symbol_changes(previous_events, project, result.warnings)
    result.asset_bytes = sum(len(asset.data) for asset in assets)
    for scene in project.scenes:
        for widget in scene.widgets:
            if project.project.shape == "circle":
                cx = project.project.display_width / 2.0
                cy = project.project.display_height / 2.0
                radius = min(project.project.display_width, project.project.display_height) / 2.0
                corners = ((widget.x, widget.y), (widget.x + widget.width, widget.y),
                           (widget.x, widget.y + widget.height),
                           (widget.x + widget.width, widget.y + widget.height))
                if any(math.hypot(x - cx, y - cy) > radius for x, y in corners):
                    result.warnings.append(
                        f"'{scene.name}.{widget.name}' has a corner outside the circular visible area."
                    )
            if abs(widget.rotation) > 0.001 and widget.type != "image":
                result.warnings.append(
                    f"'{scene.name}.{widget.name}' rotation={widget.rotation:g}° is preview-only; generated rotation currently supports Image targets."
                )
            if widget.type == "image" and widget.opacity != 255:
                result.warnings.append(
                    f"'{scene.name}.{widget.name}' image opacity is preview-only; bake opacity into the PNG alpha channel."
                )
            if (metrics is not None and widget.type in ("label", "button")
                    and widget.text and widget.width > 0):
                #
                # Hardware shrinks it proportionally until it fits (see
                # ui_designer_draw_text()), so this is not an error; but it may shrink below
                # legibility, and with both the Designer preview and the hardware shrinking,
                # "why is this line smaller than I set it" is invisible on screen.
                #
                text_width = metrics.text_width(widget.text, float(widget.font_size))
                if text_width > float(widget.width):
                    shrunk = widget.font_size * widget.width / text_width
                    result.warnings.append(
                        f"'{scene.name}.{widget.name}' text is about {text_width:.0f} px wide at "
                        f"font_size={widget.font_size}, but the box is {widget.width} px; the device will "
                        f"shrink it to about {shrunk:.0f} px. Widen the box or lower the font size."
                    )
            if (widget.type in ("label", "button")
                    and project.project.font_symbol == "DejaVuSans_ttf_kern"
                    and any(not (0x20 <= ord(char) <= 0x7E or 0x370 <= ord(char) <= 0x3FF)
                            for char in widget.text)):
                result.warnings.append(
                    f"'{scene.name}.{widget.name}' contains glyphs outside the default DejaVuSans_ttf_kern ranges; generate a font containing those glyphs or change the text."
                )
            if widget.type == "button" and widget.action in ("rotate_by", "rotate_to"):
                action_target = next((candidate for candidate in scene.widgets
                                      if candidate.name == widget.action_target), None)
                if action_target is None:
                    result.warnings.append(
                        f"'{scene.name}.{widget.name}' rotation target '{widget.action_target}' was not found."
                    )
                elif action_target.type != "image":
                    result.warnings.append(
                        f"'{scene.name}.{widget.name}' targets '{action_target.name}', but generated rotation currently supports Image only."
                    )

    bindings_h, bindings_c = _generate_bindings(project)
    _write(target / "ui_designer_project_config.h", _generate_config(project), result)
    _write(target / "ui_designer_assets.h", _generate_assets_h(assets), result)
    _write(target / "ui_designer_assets.c", _generate_assets_c(assets), result)
    _write(target / "ui_designer_bindings.h", bindings_h, result)
    _write(target / "ui_designer_bindings.c", bindings_c, result)
    _write(target / "ui_designer_generated.h", _generate_ui_h(project), result)
    _write(target / "ui_designer_generated.c", _generate_ui_c(project, asset_mapping), result)
    _write(target / "ui_designer.mk", _generate_makefile_fragment(), result)
    _write(target / "README_INTEGRATION.md", _generate_integration(project, assets), result)

    # Keep a generator snapshot beside C output for traceability.
    snapshot = target / "ui_project.generated.json"
    project.save(snapshot)
    result.files.append(snapshot)
    return result
