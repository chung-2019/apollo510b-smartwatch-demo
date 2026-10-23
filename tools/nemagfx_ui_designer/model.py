"""Serializable project model for the Apollo510B NemaGFX UI Designer."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
import json
from pathlib import Path
import re
import uuid
from typing import Any


WIDGET_TYPES = ("label", "image", "button", "rectangle", "circle", "line", "arc", "gauge")


def c_identifier(value: str, fallback: str = "item") -> str:
    value = re.sub(r"[^A-Za-z0-9_]", "_", value.strip())
    value = re.sub(r"_+", "_", value).strip("_")
    if not value:
        value = fallback
    if value[0].isdigit():
        value = "_" + value
    return value.lower()


def c_symbol(value: str, fallback: str = "item") -> str:
    """Sanitize a C symbol without changing its case."""
    value = re.sub(r"[^A-Za-z0-9_]", "_", value.strip())
    value = re.sub(r"_+", "_", value).strip("_")
    if not value:
        value = fallback
    if value[0].isdigit():
        value = "_" + value
    return value


def unique_name(base: str, existing: set[str]) -> str:
    base = c_identifier(base)
    if base not in existing:
        return base
    number = 2
    while f"{base}_{number}" in existing:
        number += 1
    return f"{base}_{number}"


@dataclass
class Widget:
    type: str
    name: str
    x: int = 40
    y: int = 40
    width: int = 160
    height: int = 56
    text: str = "Text"
    color: str = "#FFFFFFFF"
    background: str = "#26364FFF"
    border_color: str = "#56D6FFFF"
    border_width: int = 2
    radius: int = 12
    font_size: int = 28
    align: str = "center"
    source: str = ""
    image_format: str = "auto"
    opacity: int = 255
    rotation: float = 0.0
    visible: bool = True
    start_angle: float = 210.0
    end_angle: float = 510.0
    stroke_width: float = 12.0
    value: float = 65.0
    minimum: float = 0.0
    maximum: float = 100.0
    binding: str = ""
    event: str = ""
    action: str = "none"
    action_target: str = ""
    action_value: float = 360.0
    duration_ms: int = 800
    easing: str = "ease_in_out"
    id: str = field(default_factory=lambda: uuid.uuid4().hex)

    def normalize(self) -> None:
        if self.type not in WIDGET_TYPES:
            raise ValueError(f"Unsupported widget type: {self.type}")
        self.name = c_identifier(self.name, self.type)
        self.width = max(1, int(self.width))
        self.height = max(1, int(self.height))
        self.opacity = max(0, min(255, int(self.opacity)))
        self.font_size = max(1, int(self.font_size))
        self.radius = max(0, int(self.radius))
        self.border_width = max(0, int(self.border_width))
        self.stroke_width = max(0.1, float(self.stroke_width))
        if self.action not in ("none", "rotate_by", "rotate_to"):
            self.action = "none"
        if self.easing not in ("linear", "ease_in_out"):
            self.easing = "ease_in_out"
        self.duration_ms = max(0, int(self.duration_ms))
        self.action_value = float(self.action_value)
        if self.maximum <= self.minimum:
            self.maximum = self.minimum + 1.0
        self.value = max(self.minimum, min(self.maximum, float(self.value)))

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "Widget":
        allowed = cls.__dataclass_fields__.keys()
        widget = cls(**{key: value for key, value in data.items() if key in allowed})
        widget.normalize()
        return widget


@dataclass
class Scene:
    name: str = "Home"
    background: str = "#050A12FF"
    widgets: list[Widget] = field(default_factory=list)

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "Scene":
        return cls(
            name=str(data.get("name", "Scene")),
            background=str(data.get("background", "#050A12FF")),
            widgets=[Widget.from_dict(item) for item in data.get("widgets", [])],
        )


@dataclass
class ProjectSettings:
    name: str = "ApolloWatchUI"
    #
    # The canvas is the circle the watch actually shows, 454x454. The panel's
    # framebuffer is 468x468, but the outermost 7 px are hidden by the bezel and text
    # placed there is clipped on hardware, so the editor should not let anyone put
    # things there. The generated side centres the whole canvas on the framebuffer (see
    # framebuffer_width).
    #
    display_width: int = 454
    display_height: int = 454
    #
    # The target board's framebuffer size (468x468 on apollo510b_evb, see FB_RESX).
    # Only used at code generation time to compute the centring offset; it is not visible on the Designer canvas.
    #
    framebuffer_width: int = 468
    framebuffer_height: int = 468
    shape: str = "circle"
    safe_margin: int = 24
    color_format: str = "RGB565"
    output_dir: str = "src/generated/ui_designer"
    font_header: str = "font/DejaVuSans_ttf_kern.h"
    font_symbol: str = "DejaVuSans_ttf_kern"
    build_working_dir: str = "gcc"
    build_command: str = "make -j8"
    flash_command: str = ""

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "ProjectSettings":
        allowed = cls.__dataclass_fields__.keys()
        result = cls(**{key: value for key, value in data.items() if key in allowed})
        result.display_width = max(1, int(result.display_width))
        result.display_height = max(1, int(result.display_height))
        #
        # Older project files have neither field and the dataclass defaults them to 468;
        # but a project whose canvas is larger than the framebuffer (an old 468 canvas,
        # for instance) should not be forced to a smaller target, which would give a
        # negative offset. Taking the larger of the two degenerates the offset to 0,
        # which is the behaviour from before this revision.
        #
        result.framebuffer_width = max(1, int(result.framebuffer_width), result.display_width)
        result.framebuffer_height = max(1, int(result.framebuffer_height), result.display_height)
        result.safe_margin = max(0, int(result.safe_margin))
        return result

    @property
    def origin_x(self) -> int:
        """x of the canvas's top-left corner on the framebuffer. The canvas is centred, so it is half the difference."""
        return (self.framebuffer_width - self.display_width) // 2

    @property
    def origin_y(self) -> int:
        """y of the canvas's top-left corner on the framebuffer."""
        return (self.framebuffer_height - self.display_height) // 2


@dataclass
class UIProject:
    project: ProjectSettings = field(default_factory=ProjectSettings)
    scenes: list[Scene] = field(default_factory=lambda: [Scene()])
    version: int = 1

    def ensure_valid(self) -> None:
        if not self.scenes:
            self.scenes.append(Scene())
        scene_names: set[str] = set()
        for scene_index, scene in enumerate(self.scenes, 1):
            scene.name = unique_name(scene.name or f"scene_{scene_index}", scene_names)
            scene_names.add(scene.name)
            widget_names: set[str] = set()
            for widget in scene.widgets:
                widget.normalize()
                widget.name = unique_name(widget.name, widget_names)
                widget_names.add(widget.name)

    def to_dict(self) -> dict[str, Any]:
        self.ensure_valid()
        return asdict(self)

    def save(self, path: str | Path) -> None:
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(
            json.dumps(self.to_dict(), ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    @classmethod
    def load(cls, path: str | Path) -> "UIProject":
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        result = cls(
            version=int(data.get("version", 1)),
            project=ProjectSettings.from_dict(data.get("project", {})),
            scenes=[Scene.from_dict(scene) for scene in data.get("scenes", [])],
        )
        result.ensure_valid()
        return result


def new_widget(widget_type: str, existing_names: set[str], x: int = 40, y: int = 40) -> Widget:
    defaults: dict[str, dict[str, Any]] = {
        "label": {"width": 220, "height": 60, "text": "Label", "background": "#00000000"},
        "image": {"width": 96, "height": 96, "text": "Image", "background": "#182030FF"},
        "button": {"width": 180, "height": 64, "text": "Button", "event": "button_clicked",
                   "action": "none", "action_value": 360.0, "duration_ms": 800,
                   "easing": "ease_in_out"},
        "rectangle": {"width": 180, "height": 100, "text": "", "background": "#203048FF"},
        "circle": {"width": 100, "height": 100, "text": "", "background": "#38E8FFFF"},
        "line": {"width": 160, "height": 4, "text": "", "color": "#FFFFFFFF", "stroke_width": 4},
        "arc": {"width": 180, "height": 180, "text": "", "background": "#00000000"},
        "gauge": {"width": 240, "height": 240, "text": "", "background": "#26364FFF"},
    }
    if widget_type not in defaults:
        raise ValueError(f"Unsupported widget type: {widget_type}")
    values = defaults[widget_type]
    return Widget(
        type=widget_type,
        name=unique_name(widget_type, existing_names),
        x=int(x),
        y=int(y),
        **values,
    )
