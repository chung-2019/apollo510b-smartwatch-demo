"""Apollo510B NemaGFX UI Designer desktop application."""

from __future__ import annotations

import os
from pathlib import Path
import sys

try:
    from PySide6.QtCore import QEasingCurve, QMimeData, QProcess, Qt, QVariantAnimation
    from PySide6.QtGui import QAction, QCloseEvent, QColor, QDrag, QFont, QFontDatabase, QKeySequence, QTextCursor
    from PySide6.QtWidgets import (
        QApplication,
        QDialog,
        QDialogButtonBox,
        QColorDialog,
        QFileDialog,
        QFormLayout,
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QListWidget,
        QListWidgetItem,
        QMainWindow,
        QMessageBox,
        QPushButton,
        QSpinBox,
        QSplitter,
        QStatusBar,
        QTextEdit,
        QToolBar,
        QVBoxLayout,
        QWidget,
        QInputDialog,
    )
except ImportError as exc:
    print("PySide6 is required. Run: python -m pip install -r requirements.txt", file=sys.stderr)
    raise SystemExit(2) from exc

from canvas import CanvasScene, CanvasView, DesignerItem, MIME_WIDGET_TYPE
from generator import generate_project
from model import Scene, UIProject, Widget, c_identifier, new_widget, unique_name
from property_editor import PropertyEditor


APP_TITLE = "Apollo510B NemaGFX UI Designer"
TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[1]


class WidgetPalette(QListWidget):
    LABELS = {
        "label": "Label",
        "image": "Image",
        "button": "Button",
        "rectangle": "Rectangle",
        "circle": "Circle",
        "line": "Line",
        "arc": "Arc",
        "gauge": "Gauge",
    }

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setDragEnabled(True)
        for widget_type, label in self.LABELS.items():
            item = QListWidgetItem(label)
            item.setData(Qt.ItemDataRole.UserRole, widget_type)
            self.addItem(item)

    def startDrag(self, supported_actions) -> None:
        item = self.currentItem()
        if item is None:
            return
        mime = QMimeData()
        mime.setData(MIME_WIDGET_TYPE, item.data(Qt.ItemDataRole.UserRole).encode("utf-8"))
        drag = QDrag(self)
        drag.setMimeData(mime)
        drag.exec(Qt.DropAction.CopyAction)


class ProjectSettingsDialog(QDialog):
    def __init__(self, project: UIProject, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Project settings")
        self.project = project
        form = QFormLayout(self)
        settings = project.project

        self.name = QLineEdit(settings.name)
        self.width = QSpinBox(); self.width.setRange(1, 4096); self.width.setValue(settings.display_width)
        self.height = QSpinBox(); self.height.setRange(1, 4096); self.height.setValue(settings.display_height)
        self.fb_width = QSpinBox(); self.fb_width.setRange(1, 4096); self.fb_width.setValue(settings.framebuffer_width)
        self.fb_height = QSpinBox(); self.fb_height.setRange(1, 4096); self.fb_height.setValue(settings.framebuffer_height)
        self.margin = QSpinBox(); self.margin.setRange(0, 1024); self.margin.setValue(settings.safe_margin)
        self.output = QLineEdit(settings.output_dir)
        self.font_header = QLineEdit(settings.font_header)
        self.font_symbol = QLineEdit(settings.font_symbol)
        self.build_dir = QLineEdit(settings.build_working_dir)
        self.build_command = QLineEdit(settings.build_command)
        self.flash_command = QLineEdit(settings.flash_command)
        form.addRow("Project name", self.name)
        form.addRow("Visible canvas width", self.width)
        form.addRow("Visible canvas height", self.height)
        form.addRow("Panel framebuffer width", self.fb_width)
        form.addRow("Panel framebuffer height", self.fb_height)
        form.addRow("Round safe margin", self.margin)
        form.addRow("Output directory", self.output)
        form.addRow("NemaVG font header", self.font_header)
        form.addRow("NemaVG font symbol", self.font_symbol)
        form.addRow("Build working directory", self.build_dir)
        form.addRow("Build command", self.build_command)
        form.addRow("Flash command", self.flash_command)
        note = QLabel("The canvas is the circle the watch actually shows, 454x454 by default; the framebuffer of apollo510b_evb is 468x468, "
                      "and the canvas is centred automatically at code generation (7 px per side). Text outside the canvas is hidden by the bezel on hardware.")
        note.setWordWrap(True)
        note.setStyleSheet("color: #4FC3F7")
        form.addRow(note)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)

    def accept(self) -> None:
        settings = self.project.project
        settings.name = self.name.text().strip() or "ApolloWatchUI"
        settings.display_width = self.width.value()
        settings.display_height = self.height.value()
        settings.framebuffer_width = max(self.fb_width.value(), settings.display_width)
        settings.framebuffer_height = max(self.fb_height.value(), settings.display_height)
        settings.safe_margin = self.margin.value()
        settings.output_dir = self.output.text().strip()
        settings.font_header = self.font_header.text().strip()
        settings.font_symbol = self.font_symbol.text().strip()
        settings.build_working_dir = self.build_dir.text().strip()
        settings.build_command = self.build_command.text().strip()
        settings.flash_command = self.flash_command.text().strip()
        super().accept()


class LogWindow(QDialog):
    def __init__(self, title: str, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle(title)
        self.resize(850, 480)
        layout = QVBoxLayout(self)
        self.output = QTextEdit()
        self.output.setReadOnly(True)
        self.output.setFontFamily("Consolas")
        layout.addWidget(self.output)
        self.close_button = QPushButton("Close")
        self.close_button.clicked.connect(self.close)
        layout.addWidget(self.close_button)


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.resize(1450, 900)
        self.project = UIProject()
        self.project_path: Path | None = None
        self.current_scene_index = 0
        self.dirty = False
        self.process: QProcess | None = None
        self.log_window: LogWindow | None = None
        self.preview_animations: list[QVariantAnimation] = []
        # True while the property panel is applying a change: it stops a signal bounced back from the canvas rebuilding the panel again.
        self._applying_property = False

        self.canvas_scene = CanvasScene(self)
        self.canvas = CanvasView(self.canvas_scene)
        self.canvas_scene.widget_selected.connect(self._selection_changed)
        self.canvas_scene.widget_changed.connect(self._canvas_widget_changed)
        self.canvas_scene.button_activated.connect(self._preview_button_action)
        self.canvas.widget_dropped.connect(self.add_widget)

        self.palette = WidgetPalette()
        self.palette.itemDoubleClicked.connect(
            lambda item, column=0: self.add_widget(item.data(Qt.ItemDataRole.UserRole), 54, 54))
        self.scene_list = QListWidget()
        self.scene_list.currentRowChanged.connect(self.switch_scene)
        self.layer_list = QListWidget()
        self.layer_list.currentRowChanged.connect(self._layer_selected)
        self.properties = PropertyEditor()
        self.properties.changed.connect(self._property_changed)
        self.warning_label = QLabel()
        self.warning_label.setWordWrap(True)
        self.warning_label.setStyleSheet("color: #FFB74D; padding: 6px")

        self._build_layout()
        self._create_actions()
        self._create_toolbar()
        self.setStatusBar(QStatusBar())
        self._refresh_all()

    @property
    def scene_model(self) -> Scene:
        return self.project.scenes[self.current_scene_index]

    def _build_layout(self) -> None:
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.addWidget(QLabel("Widgets (drag onto the canvas or double-click)"))
        left_layout.addWidget(self.palette, 2)
        left_layout.addWidget(QLabel("Scenes"))
        left_layout.addWidget(self.scene_list, 1)
        scene_buttons = QHBoxLayout()
        add_scene = QPushButton("+")
        remove_scene = QPushButton("-")
        rename_scene = QPushButton("Rename")
        scene_color = QPushButton("Background")
        add_scene.clicked.connect(self.add_scene)
        remove_scene.clicked.connect(self.remove_scene)
        rename_scene.clicked.connect(self.rename_scene)
        scene_color.clicked.connect(self.change_scene_background)
        scene_buttons.addWidget(add_scene)
        scene_buttons.addWidget(remove_scene)
        scene_buttons.addWidget(rename_scene)
        scene_buttons.addWidget(scene_color)
        left_layout.addLayout(scene_buttons)
        left_layout.addWidget(QLabel("Layers (lower drawn first, upper drawn last)"))
        left_layout.addWidget(self.layer_list, 2)
        layer_buttons = QHBoxLayout()
        up = QPushButton("Move up")
        down = QPushButton("Move down")
        up.clicked.connect(lambda: self.move_layer(1))
        down.clicked.connect(lambda: self.move_layer(-1))
        layer_buttons.addWidget(up)
        layer_buttons.addWidget(down)
        left_layout.addLayout(layer_buttons)
        left.setMinimumWidth(225)

        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.addWidget(QLabel("Properties"))
        right_layout.addWidget(self.properties, 1)
        right_layout.addWidget(self.warning_label)
        right.setMinimumWidth(310)

        splitter = QSplitter()
        splitter.addWidget(left)
        splitter.addWidget(self.canvas)
        splitter.addWidget(right)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setStretchFactor(2, 0)
        splitter.setSizes([240, 880, 330])
        self.setCentralWidget(splitter)

    def _action(self, text: str, slot, shortcut=None) -> QAction:
        action = QAction(text, self)
        action.triggered.connect(slot)
        if shortcut:
            action.setShortcut(shortcut)
        return action

    def _create_actions(self) -> None:
        self.action_new = self._action("New", self.new_project, QKeySequence.StandardKey.New)
        self.action_open = self._action("Open", self.open_project, QKeySequence.StandardKey.Open)
        self.action_save = self._action("Save", self.save_project, QKeySequence.StandardKey.Save)
        self.action_save_as = self._action("Save as", self.save_project_as, QKeySequence.StandardKey.SaveAs)
        self.action_settings = self._action("Project settings", self.edit_settings)
        self.action_generate = self._action("Generate C", self.generate_c, "Ctrl+G")
        self.action_build = self._action("Build", self.build_project, "Ctrl+B")
        self.action_flash = self._action("Flash", self.flash_project)
        self.action_delete = self._action("Delete widget", self.delete_selected, QKeySequence.StandardKey.Delete)
        self.action_duplicate = self._action("Duplicate widget", self.duplicate_selected, "Ctrl+D")
        self.action_zoom_in = self._action("Zoom in", lambda: self.canvas.scale(1.15, 1.15), QKeySequence.StandardKey.ZoomIn)
        self.action_zoom_out = self._action("Zoom out", lambda: self.canvas.scale(1 / 1.15, 1 / 1.15), QKeySequence.StandardKey.ZoomOut)
        self.action_fit = self._action("Fit to window", self.fit_canvas, "Ctrl+0")
        self.action_center_h = self._action("Centre horizontally", self.center_selected_horizontally)
        self.action_center_v = self._action("Centre vertically", self.center_selected_vertically)
        self.action_preview = self._action("Interactive preview", self.toggle_preview, "F5")
        self.action_preview.setCheckable(True)

        file_menu = self.menuBar().addMenu("File")
        file_menu.addActions([self.action_new, self.action_open, self.action_save, self.action_save_as])
        file_menu.addSeparator(); file_menu.addAction(self.action_settings)
        edit_menu = self.menuBar().addMenu("Edit")
        edit_menu.addActions([self.action_duplicate, self.action_delete])
        edit_menu.addSeparator()
        edit_menu.addActions([self.action_center_h, self.action_center_v])
        build_menu = self.menuBar().addMenu("Generate and flash")
        build_menu.addActions([self.action_generate, self.action_build, self.action_flash])
        view_menu = self.menuBar().addMenu("View")
        view_menu.addActions([self.action_zoom_in, self.action_zoom_out, self.action_fit, self.action_preview])

    def _create_toolbar(self) -> None:
        toolbar = QToolBar("Main")
        toolbar.setMovable(False)
        self.addToolBar(toolbar)
        toolbar.addActions([self.action_new, self.action_open, self.action_save])
        toolbar.addSeparator()
        toolbar.addAction(self.action_settings)
        toolbar.addSeparator()
        toolbar.addActions([self.action_duplicate, self.action_delete])
        toolbar.addActions([self.action_center_h, self.action_center_v])
        toolbar.addSeparator()
        toolbar.addActions([self.action_zoom_out, self.action_fit, self.action_zoom_in])
        toolbar.addAction(self.action_preview)
        toolbar.addSeparator()
        toolbar.addActions([self.action_generate, self.action_build, self.action_flash])

    def _refresh_all(self) -> None:
        self.scene_list.blockSignals(True)
        self.scene_list.clear()
        self.scene_list.addItems(scene.name for scene in self.project.scenes)
        self.scene_list.setCurrentRow(self.current_scene_index)
        self.scene_list.blockSignals(False)
        self.canvas_scene.asset_base_dir = self.project_path.parent if self.project_path else REPO_ROOT
        self.canvas_scene.load_scene(self.project, self.scene_model)
        self._refresh_layers()
        self.properties.show_widget(None)
        self.fit_canvas()
        self._update_title()

    def _refresh_layers(self, selected_id: str | None = None) -> None:
        self.layer_list.blockSignals(True)
        self.layer_list.clear()
        for widget in self.scene_model.widgets:
            item = QListWidgetItem(f"{widget.name}  [{widget.type}]")
            item.setData(Qt.ItemDataRole.UserRole, widget.id)
            self.layer_list.addItem(item)
            if selected_id == widget.id:
                self.layer_list.setCurrentItem(item)
        self.layer_list.blockSignals(False)

    def _mark_dirty(self) -> None:
        self.dirty = True
        self._update_title()

    def _update_title(self) -> None:
        name = self.project_path.name if self.project_path else self.project.project.name
        self.setWindowTitle(f"{'*' if self.dirty else ''}{name} — {APP_TITLE}")

    def fit_canvas(self) -> None:
        self.canvas.fitInView(self.canvas_scene.sceneRect(), Qt.AspectRatioMode.KeepAspectRatio)

    def _selected_designer_item(self) -> DesignerItem | None:
        selected = [item for item in self.canvas_scene.selectedItems() if isinstance(item, DesignerItem)]
        return selected[0] if selected else None

    def center_selected_horizontally(self) -> None:
        item = self._selected_designer_item()
        if item is not None:
            item.setX((self.project.project.display_width - item.widget.width) / 2.0)

    def center_selected_vertically(self) -> None:
        item = self._selected_designer_item()
        if item is not None:
            item.setY((self.project.project.display_height - item.widget.height) / 2.0)

    def add_widget(self, widget_type: str, x: float, y: float) -> None:
        existing = {widget.name for widget in self.scene_model.widgets}
        width = self.project.project.display_width
        height = self.project.project.display_height
        widget = new_widget(widget_type, existing, max(0, int(x)), max(0, int(y)))
        widget.x = min(widget.x, max(0, width - widget.width))
        widget.y = min(widget.y, max(0, height - widget.height))
        self.scene_model.widgets.append(widget)
        self.canvas_scene.load_scene(self.project, self.scene_model)
        self.canvas_scene.select_widget(widget.id)
        self._refresh_layers(widget.id)
        self._mark_dirty()

    def delete_selected(self) -> None:
        selected = [item for item in self.canvas_scene.selectedItems() if isinstance(item, DesignerItem)]
        if not selected:
            return
        ids = {item.widget.id for item in selected}
        self.scene_model.widgets[:] = [widget for widget in self.scene_model.widgets if widget.id not in ids]
        self.canvas_scene.load_scene(self.project, self.scene_model)
        self._refresh_layers()
        self.properties.show_widget(None)
        self._mark_dirty()

    def duplicate_selected(self) -> None:
        selected = [item for item in self.canvas_scene.selectedItems() if isinstance(item, DesignerItem)]
        if not selected:
            return
        source = selected[0].widget
        data = source.__dict__.copy()
        data.pop("id", None)
        existing = {widget.name for widget in self.scene_model.widgets}
        data["name"] = unique_name(source.name, existing)
        data["x"] = source.x + 12
        data["y"] = source.y + 12
        clone = Widget(**data)
        self.scene_model.widgets.append(clone)
        self.canvas_scene.load_scene(self.project, self.scene_model)
        self.canvas_scene.select_widget(clone.id)
        self._refresh_layers(clone.id)
        self._mark_dirty()

    def add_scene(self) -> None:
        name, accepted = QInputDialog.getText(self, "New scene", "Scene name")
        if not accepted:
            return
        existing = {scene.name for scene in self.project.scenes}
        scene = Scene(name=unique_name(name or "scene", existing))
        self.project.scenes.append(scene)
        self.current_scene_index = len(self.project.scenes) - 1
        self._refresh_all()
        self._mark_dirty()

    def remove_scene(self) -> None:
        if len(self.project.scenes) <= 1:
            QMessageBox.information(self, APP_TITLE, "A project needs at least one scene.")
            return
        scene = self.scene_model
        answer = QMessageBox.question(self, "Delete scene", f"Delete {scene.name}?")
        if answer != QMessageBox.StandardButton.Yes:
            return
        del self.project.scenes[self.current_scene_index]
        self.current_scene_index = min(self.current_scene_index, len(self.project.scenes) - 1)
        self._refresh_all()
        self._mark_dirty()

    def rename_scene(self) -> None:
        current = self.scene_model
        name, accepted = QInputDialog.getText(self, "Rename scene", "Scene name", text=current.name)
        if not accepted:
            return
        existing = {scene.name for scene in self.project.scenes if scene is not current}
        current.name = unique_name(name or "scene", existing)
        self._refresh_all()
        self._mark_dirty()

    def change_scene_background(self) -> None:
        current = self.scene_model
        value = current.background
        initial = QColor(value[:7])
        if len(value) == 9:
            initial.setAlpha(int(value[7:9], 16))
        color = QColorDialog.getColor(initial, self, "Scene background colour",
                                      QColorDialog.ColorDialogOption.ShowAlphaChannel)
        if not color.isValid():
            return
        current.background = color.name(QColor.NameFormat.HexRgb).upper() + f"{color.alpha():02X}"
        self.canvas_scene.load_scene(self.project, current)
        self._refresh_layers()
        self._mark_dirty()

    def switch_scene(self, index: int) -> None:
        if index < 0 or index >= len(self.project.scenes) or index == self.current_scene_index:
            return
        self.current_scene_index = index
        self.canvas_scene.load_scene(self.project, self.scene_model)
        self._refresh_layers()
        self.properties.show_widget(None)
        self.fit_canvas()

    def move_layer(self, delta: int) -> None:
        selected = [item for item in self.canvas_scene.selectedItems() if isinstance(item, DesignerItem)]
        if not selected:
            return
        widget = selected[0].widget
        index = self.scene_model.widgets.index(widget)
        new_index = max(0, min(len(self.scene_model.widgets) - 1, index + delta))
        if new_index == index:
            return
        self.scene_model.widgets[index], self.scene_model.widgets[new_index] = (
            self.scene_model.widgets[new_index], self.scene_model.widgets[index])
        self.canvas_scene.load_scene(self.project, self.scene_model)
        self.canvas_scene.select_widget(widget.id)
        self._refresh_layers(widget.id)
        self._mark_dirty()

    def _layer_selected(self, row: int) -> None:
        if row >= 0:
            widget_id = self.layer_list.item(row).data(Qt.ItemDataRole.UserRole)
            item = self.canvas_scene.items_by_id.get(widget_id)
            self.canvas_scene.select_widget(widget_id)
            if item is not None and not item.widget.visible:
                self.properties.show_widget(item.widget)

    def _update_warning(self, widget: Widget | None) -> None:
        if widget is not None and self.canvas_scene.outside_circle(widget):
            self.warning_label.setText(f"! The corners of {widget.name} fall outside the round visible area.")
        else:
            self.warning_label.setText("")

    def _selection_changed(self, item: DesignerItem | None) -> None:
        if self._applying_property:
            return
        self.properties.set_available_targets(
            [widget.name for widget in self.scene_model.widgets if widget.type == "image"])
        self.properties.show_widget(item.widget if item else None)
        if item:
            self._refresh_layers(item.widget.id)
        self._update_warning(item.widget if item else None)

    def _canvas_widget_changed(self, item: DesignerItem) -> None:
        if self._applying_property:
            return
        self.properties.set_available_targets(
            [widget.name for widget in self.scene_model.widgets if widget.type == "image"])
        self.properties.show_widget(item.widget)
        self._refresh_layers(item.widget.id)
        self._mark_dirty()
        self._update_warning(item.widget)

    def _property_changed(self, widget: Widget) -> None:
        #
        # setPos() triggers DesignerItem.itemChange -> widget_changed -> a property panel
        # rebuild, while we are still inside some control's signal handler on that panel. The lock blocks that way back.
        #
        self._applying_property = True
        try:
            item = self.canvas_scene.items_by_id.get(widget.id)
            if item:
                item.prepareGeometryChange()
                item.setPos(widget.x, widget.y)
                item.setTransformOriginPoint(widget.width / 2.0, widget.height / 2.0)
                item.setRotation(widget.rotation)
                item.apply_visibility()
                item.reload_image()
                item.update()
        finally:
            self._applying_property = False
        self._refresh_layers(widget.id)
        self._update_warning(widget)
        self._mark_dirty()

    def toggle_preview(self, enabled: bool) -> None:
        self.preview_animations.clear()
        self.canvas_scene.load_scene(self.project, self.scene_model)
        self.canvas_scene.set_preview_mode(enabled)
        self.properties.setEnabled(not enabled)
        self.palette.setEnabled(not enabled)
        self.layer_list.setEnabled(not enabled)
        self.action_preview.setText("Exit preview" if enabled else "Interactive preview")
        self.statusBar().showMessage(
            "Interactive preview: click the buttons on the canvas directly" if enabled else "Back to edit mode", 5000)

    def _preview_button_action(self, button: Widget) -> None:
        if button.action not in ("rotate_by", "rotate_to") or not button.action_target:
            self.statusBar().showMessage(f"{button.name} has no rotation action configured", 4000)
            return
        target = next((widget for widget in self.scene_model.widgets
                       if widget.name == button.action_target and widget.type == "image"), None)
        if target is None:
            self.statusBar().showMessage(f"Image target not found: {button.action_target}", 5000)
            return
        item = self.canvas_scene.items_by_id.get(target.id)
        if item is None:
            return
        start = item.rotation()
        end = start + button.action_value if button.action == "rotate_by" else button.action_value
        animation = QVariantAnimation(self)
        animation.setStartValue(start)
        animation.setEndValue(end)
        animation.setDuration(button.duration_ms)
        animation.setEasingCurve(QEasingCurve.Type.Linear if button.easing == "linear"
                                 else QEasingCurve.Type.InOutCubic)
        animation.valueChanged.connect(lambda value, target_item=item: target_item.setRotation(float(value)))
        animation.finished.connect(lambda active=animation: self.preview_animations.remove(active)
                                   if active in self.preview_animations else None)
        self.preview_animations.append(animation)
        animation.start()

    def _confirm_discard(self) -> bool:
        if not self.dirty:
            return True
        result = QMessageBox.question(
            self, APP_TITLE, "The current project is unsaved. Discard the changes?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        return result == QMessageBox.StandardButton.Yes

    def new_project(self) -> None:
        if not self._confirm_discard():
            return
        self.project = UIProject()
        self.project_path = None
        self.current_scene_index = 0
        self.dirty = False
        self._refresh_all()

    def open_project(self) -> None:
        if not self._confirm_discard():
            return
        path, _ = QFileDialog.getOpenFileName(self, "Open a UI project", str(REPO_ROOT), "UI Project (*.json)")
        if not path:
            return
        try:
            self.project = UIProject.load(path)
        except Exception as exc:
            QMessageBox.critical(self, APP_TITLE, f"Cannot open the project:\n{exc}")
            return
        self.project_path = Path(path)
        self.current_scene_index = 0
        self.dirty = False
        self._refresh_all()

    def save_project(self) -> bool:
        if self.project_path is None:
            return self.save_project_as()
        try:
            self.project.save(self.project_path)
        except Exception as exc:
            QMessageBox.critical(self, APP_TITLE, f"Cannot save the project:\n{exc}")
            return False
        self.dirty = False
        self._update_title()
        self.statusBar().showMessage(f"Saved {self.project_path}", 5000)
        return True

    def save_project_as(self) -> bool:
        path, _ = QFileDialog.getSaveFileName(self, "Save the UI project", str(REPO_ROOT / "ui_project.json"),
                                              "UI Project (*.json)")
        if not path:
            return False
        self.project_path = Path(path).with_suffix(".json")
        return self.save_project()

    def edit_settings(self) -> None:
        dialog = ProjectSettingsDialog(self.project, self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            self.canvas_scene.load_scene(self.project, self.scene_model)
            self.fit_canvas()
            self._mark_dirty()

    def _resolve_from_repo(self, value: str) -> Path:
        path = Path(value)
        return path if path.is_absolute() else REPO_ROOT / path

    def generate_c(self) -> None:
        if self.project_path is None and not self.save_project_as():
            return
        if self.dirty and not self.save_project():
            return
        target = self._resolve_from_repo(self.project.project.output_dir)
        try:
            result = generate_project(self.project, target, self.project_path)
        except Exception as exc:
            QMessageBox.critical(self, APP_TITLE, f"Generate C failed:\n{exc}")
            return
        details = (f"{len(result.files)} files generated\nOutput: {result.output_dir}\n"
                   f"Image assets: {result.asset_bytes:,} bytes")
        if result.warnings:
            details += "\n\nWarnings:\n- " + "\n- ".join(result.warnings)
            QMessageBox.warning(self, "Generate C finished with warnings", details)
        else:
            QMessageBox.information(self, "Generate C finished", details)
        self.statusBar().showMessage(details.replace("\n", " | "), 10000)

    def _run_command(self, title: str, command: str, working_dir: str) -> None:
        if not command.strip():
            QMessageBox.information(self, title, "No command has been set in the project settings.")
            return
        if self.process and self.process.state() != QProcess.ProcessState.NotRunning:
            QMessageBox.warning(self, title, "A command is already running.")
            return
        workdir = self._resolve_from_repo(working_dir)
        if not workdir.exists():
            QMessageBox.critical(self, title, f"The working directory does not exist:\n{workdir}")
            return
        self.log_window = LogWindow(title, self)
        self.log_window.output.append(f"> {command}\n")
        self.log_window.show()
        self.process = QProcess(self)
        self.process.setWorkingDirectory(str(workdir))
        self.process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        self.process.readyReadStandardOutput.connect(self._read_process_output)
        self.process.finished.connect(self._process_finished)
        self.process.startCommand(command)

    def _read_process_output(self) -> None:
        if self.process and self.log_window:
            text = bytes(self.process.readAllStandardOutput()).decode(errors="replace")
            self.log_window.output.moveCursor(QTextCursor.MoveOperation.End)
            self.log_window.output.insertPlainText(text)

    def _process_finished(self, exit_code: int, exit_status) -> None:
        if self.log_window:
            self.log_window.output.append(f"\n[finished, exit code {exit_code}]")

    def build_project(self) -> None:
        self._run_command("Build", self.project.project.build_command,
                          self.project.project.build_working_dir)

    def flash_project(self) -> None:
        self._run_command("Flash", self.project.project.flash_command,
                          self.project.project.build_working_dir)

    def closeEvent(self, event: QCloseEvent) -> None:
        if self._confirm_discard():
            event.accept()
        else:
            event.ignore()


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName(APP_TITLE)
    app.setStyle("Fusion")
    if sys.platform == "win32":
        font_file = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts" / "msjh.ttc"
        if font_file.exists():
            font_id = QFontDatabase.addApplicationFont(str(font_file))
            families = QFontDatabase.applicationFontFamilies(font_id)
            if families:
                app.setFont(QFont(families[-1], 10))
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
