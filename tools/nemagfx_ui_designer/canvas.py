"""Qt graphics canvas with movable and resizable NemaGFX widget previews."""

from __future__ import annotations

from pathlib import Path
import math

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import (QBrush, QColor, QFont, QFontMetricsF, QPainter,
                           QPainterPath, QPen, QPixmap)
from PySide6.QtWidgets import QGraphicsItem, QGraphicsScene, QGraphicsView

import font_metrics
from model import Scene, UIProject, Widget

#
# The margin left when shrinking the font size, the same as UI_TEXT_FIT_RATIO in the
# generated code. One value on both sides keeps the preview's size equal to the hardware's.
#
_TEXT_FIT_RATIO = 0.98


MIME_WIDGET_TYPE = "application/x-nemagfx-widget"


def qt_color(value: str, opacity: int = 255) -> QColor:
    text = value.strip()
    if len(text) == 9 and text.startswith("#"):
        color = QColor(text[:7])
        color.setAlpha((int(text[7:9], 16) * opacity + 127) // 255)
        return color
    color = QColor(text)
    if not color.isValid():
        color = QColor("magenta")
    color.setAlpha((color.alpha() * opacity + 127) // 255)
    return color


class DesignerItem(QGraphicsItem):
    HANDLE = 12.0

    def __init__(self, widget: Widget, change_callback, asset_base_dir: Path | None = None,
                 preview_mode: bool = False) -> None:
        super().__init__()
        self.widget = widget
        self._change_callback = change_callback
        self._resizing = False
        self._press_scene_pos = QPointF()
        self._start_size = (widget.width, widget.height)
        self._pixmap = QPixmap()
        self._asset_base_dir = asset_base_dir or Path.cwd()
        self.preview_mode = preview_mode
        self.setPos(widget.x, widget.y)
        self.setTransformOriginPoint(widget.width / 2.0, widget.height / 2.0)
        self.setRotation(widget.rotation)
        self.apply_visibility()
        self.setFlags(
            QGraphicsItem.GraphicsItemFlag.ItemIsSelectable
            | QGraphicsItem.GraphicsItemFlag.ItemIsMovable
            | QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges
            | QGraphicsItem.GraphicsItemFlag.ItemIsFocusable
        )
        self.reload_image()

    def apply_visibility(self) -> None:
        #
        # In edit mode, "visible = no" only draws the widget faintly rather than really
        # calling hide(): a hidden QGraphicsItem is force-deselected by Qt, which clears
        # and rebuilds the property panel on the right -- i.e. deleting the checkbox from
        # inside that checkbox's own toggled handler. Visibility on hardware follows the
        # visible field, and only the interactive preview mode actually hides.
        #
        alpha = self.widget.opacity / 255.0
        if self.preview_mode:
            self.setVisible(self.widget.visible)
        else:
            self.setVisible(True)
            if not self.widget.visible:
                alpha *= 0.25
        self.setOpacity(alpha)

    def reload_image(self) -> None:
        self._pixmap = QPixmap()
        if self.widget.type == "image" and self.widget.source:
            source = Path(self.widget.source)
            if not source.is_absolute():
                source = self._asset_base_dir / source
            if source.exists():
                self._pixmap = QPixmap(str(source))
        self.update()

    def boundingRect(self) -> QRectF:
        extra = 2.0 if not self.isSelected() else self.HANDLE / 2.0 + 2.0
        return QRectF(-extra, -extra, self.widget.width + extra * 2, self.widget.height + extra * 2)

    def _content_rect(self) -> QRectF:
        return QRectF(0.0, 0.0, float(self.widget.width), float(self.widget.height))

    def _resize_rect(self) -> QRectF:
        return QRectF(
            self.widget.width - self.HANDLE / 2.0,
            self.widget.height - self.HANDLE / 2.0,
            self.HANDLE,
            self.HANDLE,
        )

    def shape(self) -> QPainterPath:
        path = QPainterPath()
        path.addRect(self._content_rect())
        return path

    def paint(self, painter: QPainter, option, widget=None) -> None:
        item = self.widget
        rect = self._content_rect()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(qt_color(item.background))

        if item.type in ("rectangle", "button"):
            painter.drawRoundedRect(rect, item.radius, item.radius)
            if item.border_width:
                painter.setPen(QPen(qt_color(item.border_color), item.border_width))
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.drawRoundedRect(rect.adjusted(item.border_width / 2, item.border_width / 2,
                                                       -item.border_width / 2, -item.border_width / 2),
                                        item.radius, item.radius)
        elif item.type == "circle":
            painter.drawEllipse(rect)
        elif item.type == "line":
            painter.setPen(QPen(qt_color(item.color), item.stroke_width, Qt.PenStyle.SolidLine,
                                Qt.PenCapStyle.RoundCap))
            painter.drawLine(QPointF(0, 0), QPointF(item.width, item.height))
        elif item.type in ("arc", "gauge"):
            arc_rect = rect.adjusted(item.stroke_width / 2, item.stroke_width / 2,
                                     -item.stroke_width / 2, -item.stroke_width / 2)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            if item.type == "gauge":
                painter.setPen(QPen(qt_color(item.background), item.stroke_width,
                                    Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
                painter.drawArc(arc_rect, int(-item.start_angle * 16),
                                int(-(item.end_angle - item.start_angle) * 16))
                ratio = (item.value - item.minimum) / max(0.001, item.maximum - item.minimum)
                ratio = max(0.0, min(1.0, ratio))
                span = (item.end_angle - item.start_angle) * ratio
            else:
                span = item.end_angle - item.start_angle
            painter.setPen(QPen(qt_color(item.color), item.stroke_width,
                                Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            painter.drawArc(arc_rect, int(-item.start_angle * 16), int(-span * 16))
        elif item.type == "image":
            if not self._pixmap.isNull():
                painter.drawPixmap(rect.toRect(), self._pixmap)
            else:
                painter.fillRect(rect, QColor("#182030"))
                painter.setPen(QPen(QColor("#6A7A90"), 1, Qt.PenStyle.DashLine))
                painter.drawRect(rect)
                painter.drawLine(rect.topLeft(), rect.bottomRight())
                painter.drawLine(rect.topRight(), rect.bottomLeft())

        if item.type in ("label", "button") or (item.type == "image" and self._pixmap.isNull()):
            preview_text = item.text if item.type != "image" else "Image"
            self._paint_text(painter, rect, preview_text)

        if self.isSelected():
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(QPen(QColor("#42A5F5"), 1, Qt.PenStyle.DashLine))
            painter.drawRect(rect)
            painter.fillRect(self._resize_rect(), QColor("#42A5F5"))

    def _paint_text(self, painter: QPainter, rect: QRectF, text: str) -> None:
        """Draw the preview text, with the width following the hardware font's advance table.

        The point is not how good the glyphs look but that "how wide this line is" matches
        the hardware -- text that fits on the canvas and is clipped on hardware was exactly
        the result of using the PC's system font here. The glyph outlines are not parsed
        (they are vector data inside the font's .c), so system font glyphs are still used
        and scaled horizontally to the hardware width. The shapes come out slightly fat or
        thin, but the layout positions are right.

        Text wider than the box shrinks, by the same rule as the generated
        ui_designer_draw_text(). Both sides converge on "exactly filling the box width", so
        even if the width estimate and NemaVG's actual conversion differ by a ratio, the
        boundary cases look the same (see the font_metrics module).
        """
        item = self.widget
        painter.setPen(qt_color(item.color))
        size = max(6, item.font_size)
        font = QFont("Microsoft JhengHei UI")
        font.setPixelSize(size)

        #
        # The same inset as before, so the text does not touch the frame.
        #
        box = rect.adjusted(4, 2, -4, -2)
        align_x = item.align if item.align in ("left", "center", "right") else "center"

        metrics = self.scene().font_metrics if isinstance(self.scene(), CanvasScene) else None
        target_w = metrics.text_width(text, float(size)) if metrics else 0.0
        if metrics and target_w > box.width() and box.width() > 0.0:
            #
            # Wider than the box: shrink the font size, by the same rule as the hardware's
            # ui_designer_draw_text(), right down to the 2% margin (UI_TEXT_FIT_RATIO) --
            # the preview should match what the hardware shows, including shrinking below
            # the configured value.
            #
            size = max(1, int(size * _TEXT_FIT_RATIO * box.width() / target_w))
            font.setPixelSize(size)
            target_w = metrics.text_width(text, float(size))

        painter.setFont(font)
        qt_w = QFontMetricsF(font).horizontalAdvance(text)

        if (not metrics) or target_w <= 0.0 or qt_w <= 0.0:
            #
            # With no advance table (or no measurement), fall back to the old behaviour: let Qt lay it out.
            #
            flag = {"left": Qt.AlignmentFlag.AlignLeft,
                    "center": Qt.AlignmentFlag.AlignHCenter,
                    "right": Qt.AlignmentFlag.AlignRight}[align_x]
            painter.drawText(box, flag | Qt.AlignmentFlag.AlignVCenter, text)
            return

        #
        # The alignment decides the start, then the system font's width is squeezed or stretched to the hardware width.
        #
        if align_x == "left":
            start_x = box.left()
        elif align_x == "right":
            start_x = box.right() - target_w
        else:
            start_x = box.center().x() - target_w / 2.0
        baseline_y = box.center().y() + QFontMetricsF(font).capHeight() / 2.0

        painter.save()
        painter.translate(start_x, baseline_y)
        painter.scale(target_w / qt_w, 1.0)
        painter.drawText(QPointF(0.0, 0.0), text)
        painter.restore()

    def mousePressEvent(self, event) -> None:
        if (self.preview_mode and self.widget.type == "button"
                and event.button() == Qt.MouseButton.LeftButton):
            scene = self.scene()
            if isinstance(scene, CanvasScene):
                scene.button_activated.emit(self.widget)
            event.accept()
            return
        if event.button() == Qt.MouseButton.LeftButton and self._resize_rect().contains(event.pos()):
            self._resizing = True
            self._press_scene_pos = event.scenePos()
            self._start_size = (self.widget.width, self.widget.height)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if self._resizing:
            delta = event.scenePos() - self._press_scene_pos
            self.prepareGeometryChange()
            self.widget.width = max(8, int(self._start_size[0] + delta.x()))
            self.widget.height = max(8, int(self._start_size[1] + delta.y()))
            self.setTransformOriginPoint(self.widget.width / 2.0, self.widget.height / 2.0)
            self._change_callback(self, "geometry")
            self.update()
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event) -> None:
        if self._resizing:
            self._resizing = False
            self._change_callback(self, "geometry")
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def keyPressEvent(self, event) -> None:
        directions = {
            Qt.Key.Key_Left: (-1, 0),
            Qt.Key.Key_Right: (1, 0),
            Qt.Key.Key_Up: (0, -1),
            Qt.Key.Key_Down: (0, 1),
        }
        if event.key() in directions:
            dx, dy = directions[event.key()]
            step = 10 if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else 1
            self.moveBy(dx * step, dy * step)
            event.accept()
            return
        super().keyPressEvent(event)

    def itemChange(self, change, value):
        if change == QGraphicsItem.GraphicsItemChange.ItemPositionHasChanged:
            self.widget.x = int(round(value.x()))
            self.widget.y = int(round(value.y()))
            self._change_callback(self, "position")
        return super().itemChange(change, value)


class CanvasScene(QGraphicsScene):
    widget_selected = Signal(object)
    widget_changed = Signal(object)
    request_add_widget = Signal(str, float, float)
    button_activated = Signal(object)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.project: UIProject | None = None
        self.scene_model: Scene | None = None
        self.asset_base_dir = Path.cwd()
        #
        # The hardware font's advance table. Left as None when loading fails, and the
        # preview falls back to the system font's metrics (the behaviour from before this
        # revision) -- a missing font should not stop the canvas from opening.
        #
        self.font_metrics: font_metrics.FontMetrics | None = None
        self.items_by_id: dict[str, DesignerItem] = {}
        self.preview_mode = False
        self.selectionChanged.connect(self._selection_changed)

    def reload_font_metrics(self) -> None:
        """Reload the advance table for the project's configured font. Call after switching project or changing the font setting."""
        self.font_metrics = None
        if self.project is None:
            return
        self.font_metrics = font_metrics.load(self.project.project.font_header,
                                              self.asset_base_dir)

    def load_scene(self, project: UIProject, scene_model: Scene) -> None:
        self.clear()
        self.items_by_id.clear()
        self.project = project
        self.scene_model = scene_model
        self.reload_font_metrics()
        width = project.project.display_width
        height = project.project.display_height
        margin = project.project.safe_margin
        self.setSceneRect(-30, -30, width + 60, height + 60)
        self.setBackgroundBrush(QColor("#20242B"))

        background = self.addRect(0, 0, width, height, QPen(QColor("#758094"), 1),
                                  QBrush(qt_color(scene_model.background)))
        background.setZValue(-100)
        if project.project.shape == "circle":
            boundary = self.addEllipse(0, 0, width, height, QPen(QColor("#A7B0BE"), 2),
                                       QBrush(Qt.BrushStyle.NoBrush))
            boundary.setZValue(-98)
            safe = self.addEllipse(margin, margin, width - margin * 2, height - margin * 2,
                                   QPen(QColor("#4FC3F7"), 1, Qt.PenStyle.DashLine),
                                   QBrush(Qt.BrushStyle.NoBrush))
            safe.setZValue(-97)
        center_pen = QPen(QColor(255, 255, 255, 45), 1, Qt.PenStyle.DashLine)
        hline = self.addLine(0, height / 2, width, height / 2, center_pen)
        vline = self.addLine(width / 2, 0, width / 2, height, center_pen)
        hline.setZValue(-99)
        vline.setZValue(-99)

        for z, widget in enumerate(scene_model.widgets):
            item = DesignerItem(widget, self._item_changed, self.asset_base_dir,
                                self.preview_mode)
            if self.preview_mode:
                item.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, False)
                item.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, False)
                if widget.type == "button":
                    item.setCursor(Qt.CursorShape.PointingHandCursor)
            item.setZValue(z)
            self.addItem(item)
            self.items_by_id[widget.id] = item

    def set_preview_mode(self, enabled: bool) -> None:
        self.preview_mode = enabled
        self.clearSelection()
        for item in self.items_by_id.values():
            item.preview_mode = enabled
            item.apply_visibility()
            item.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, not enabled)
            item.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, not enabled)
            item.setCursor(Qt.CursorShape.PointingHandCursor
                           if enabled and item.widget.type == "button"
                           else Qt.CursorShape.ArrowCursor)

    def _selection_changed(self) -> None:
        selected = [item for item in self.selectedItems() if isinstance(item, DesignerItem)]
        self.widget_selected.emit(selected[0] if selected else None)

    def _item_changed(self, item: DesignerItem, reason: str) -> None:
        self.widget_changed.emit(item)

    def select_widget(self, widget_id: str) -> None:
        item = self.items_by_id.get(widget_id)
        if item:
            self.clearSelection()
            item.setSelected(True)
            self.setFocusItem(item)

    def outside_circle(self, widget: Widget) -> bool:
        if not self.project or self.project.project.shape != "circle":
            return False
        width = self.project.project.display_width
        height = self.project.project.display_height
        cx, cy = width / 2.0, height / 2.0
        radius = min(width, height) / 2.0
        corners = ((widget.x, widget.y), (widget.x + widget.width, widget.y),
                   (widget.x, widget.y + widget.height),
                   (widget.x + widget.width, widget.y + widget.height))
        return any(math.hypot(x - cx, y - cy) > radius for x, y in corners)


class CanvasView(QGraphicsView):
    widget_dropped = Signal(str, float, float)

    def __init__(self, scene: CanvasScene, parent=None) -> None:
        super().__init__(scene, parent)
        self.setAcceptDrops(True)
        self.setRenderHints(QPainter.RenderHint.Antialiasing | QPainter.RenderHint.SmoothPixmapTransform)
        self.setDragMode(QGraphicsView.DragMode.RubberBandDrag)
        self.setViewportUpdateMode(QGraphicsView.ViewportUpdateMode.BoundingRectViewportUpdate)
        self.setTransformationAnchor(QGraphicsView.ViewportAnchor.AnchorUnderMouse)

    def dragEnterEvent(self, event) -> None:
        if event.mimeData().hasFormat(MIME_WIDGET_TYPE):
            event.acceptProposedAction()
        else:
            super().dragEnterEvent(event)

    def dragMoveEvent(self, event) -> None:
        if event.mimeData().hasFormat(MIME_WIDGET_TYPE):
            event.acceptProposedAction()
        else:
            super().dragMoveEvent(event)

    def dropEvent(self, event) -> None:
        if event.mimeData().hasFormat(MIME_WIDGET_TYPE):
            widget_type = bytes(event.mimeData().data(MIME_WIDGET_TYPE)).decode("utf-8")
            position = self.mapToScene(event.position().toPoint())
            self.widget_dropped.emit(widget_type, position.x(), position.y())
            event.acceptProposedAction()
        else:
            super().dropEvent(event)

    def wheelEvent(self, event) -> None:
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
            factor = 1.12 if event.angleDelta().y() > 0 else 1.0 / 1.12
            self.scale(factor, factor)
            event.accept()
        else:
            super().wheelEvent(event)
