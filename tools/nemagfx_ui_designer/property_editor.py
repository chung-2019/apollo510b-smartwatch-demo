"""Dynamic property editor for selected designer widgets."""

from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QWidget,
)

from model import Widget


class PropertyEditor(QScrollArea):
    changed = Signal(object)

    COMMON = ("name", "x", "y", "width", "height", "rotation", "opacity", "visible", "binding")
    TYPE_FIELDS = {
        "label": ("text", "font_size", "color", "align"),
        "image": ("source", "image_format"),
        "button": ("text", "font_size", "color", "background", "border_color",
                   "border_width", "radius", "align", "event", "action", "action_target",
                   "action_value", "duration_ms", "easing"),
        "rectangle": ("background", "border_color", "border_width", "radius"),
        "circle": ("background", "border_color", "border_width"),
        "line": ("color", "stroke_width"),
        "arc": ("color", "start_angle", "end_angle", "stroke_width"),
        "gauge": ("color", "background", "start_angle", "end_angle", "stroke_width",
                  "value", "minimum", "maximum"),
    }
    LABELS = {
        "name": "Name", "x": "X", "y": "Y", "width": "Width", "height": "Height",
        "rotation": "Rotation", "opacity": "Opacity", "visible": "Visible", "binding": "Data binding",
        "text": "Text", "font_size": "Font size", "color": "Foreground", "align": "Align",
        "source": "Image", "image_format": "Pixel format", "background": "Background",
        "border_color": "Border colour", "border_width": "Border width", "radius": "Corner radius",
        "event": "Click event", "stroke_width": "Line width", "start_angle": "Start angle",
        "end_angle": "End angle", "value": "Value", "minimum": "Minimum", "maximum": "Maximum",
        "action": "Press action", "action_target": "Action target", "action_value": "Rotation angle",
        "duration_ms": "Animation time (ms)", "easing": "Easing",
    }

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.widget_model: Widget | None = None
        self.available_targets: list[str] = []
        self._updating = False
        #
        # The function that writes each field's model value back into its control.
        # show_widget() updates in place with these whenever the panel structure is
        # unchanged, rather than rebuilding the form -- rebuilding deletes a control from
        # inside its own signal handler, which to Qt is a use-after-free and closes the
        # program without warning.
        #
        self._refreshers: dict[str, object] = {}
        self._layout_key: tuple | None = None
        self.container = QWidget()
        self.form = QFormLayout(self.container)
        self.form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        self.setWidget(self.container)
        self.setWidgetResizable(True)
        self.show_widget(None)

    def set_available_targets(self, targets: list[str]) -> None:
        self.available_targets = list(targets)

    def _clear(self) -> None:
        #
        # Deletion must be deferred: this is often called while some control (QSpinBox /
        # QLineEdit / QCheckBox) is emitting a signal, and deleting immediately leaves Qt
        # continuing to process events on a freed object. Signals are disconnected first, so
        # a dying control cannot write its old value into the newly selected widget, and the
        # rest is left to deleteLater().
        #
        self._refreshers.clear()
        self._layout_key = None
        while self.form.rowCount():
            row = self.form.takeRow(0)
            for item in (row.labelItem, row.fieldItem):
                if item is None:
                    continue
                widget = item.widget()
                if widget is None:
                    continue
                widget.blockSignals(True)
                for child in widget.findChildren(QWidget):
                    child.blockSignals(True)
                widget.setParent(None)
                widget.deleteLater()

    def _fields_for(self, widget: Widget) -> tuple[str, ...]:
        return self.COMMON + self.TYPE_FIELDS.get(widget.type, ())

    def _key_for(self, widget: Widget) -> tuple:
        return (id(widget), widget.type, tuple(self.available_targets))

    def show_widget(self, widget: Widget | None) -> None:
        if widget is not None and self._key_for(widget) == self._layout_key:
            # The same widget with the same fields: update the values in place, with no rebuild and no focus stealing.
            self.widget_model = widget
            self._refresh_values()
            return
        self._updating = True
        self._clear()
        self.widget_model = widget
        if widget is None:
            self.form.addRow(QLabel("Select a widget on the canvas"))
            self._updating = False
            return
        self.form.addRow("Widget type", QLabel(widget.type))
        for name in self._fields_for(widget):
            editor, refresher = self._create_editor(name, getattr(widget, name))
            self.form.addRow(self.LABELS.get(name, name), editor)
            self._refreshers[name] = refresher
        self._layout_key = self._key_for(widget)
        self._updating = False

    def _refresh_values(self) -> None:
        """Write the model's current values back into the existing controls; no rebuild, and no interruption to a field being typed in."""
        widget = self.widget_model
        if widget is None:
            return
        self._updating = True
        try:
            focused = self.focusWidget()
            for name, refresher in self._refreshers.items():
                refresher(getattr(widget, name), focused)
        finally:
            self._updating = False

    def _create_editor(self, name: str, value):
        if name == "visible":
            editor = QCheckBox()
            editor.setChecked(bool(value))
            editor.toggled.connect(lambda checked, key=name: self._set(key, checked))

            def refresh_visible(new_value, focused, control=editor):
                if control.isChecked() != bool(new_value):
                    control.setChecked(bool(new_value))
            return editor, refresh_visible
        if name in ("x", "y"):
            editor = QSpinBox()
            editor.setRange(-4096, 4096)
            editor.setValue(int(value))
            editor.valueChanged.connect(lambda number, key=name: self._set(key, number))
            return editor, self._spin_refresher(editor, int)
        if name in ("width", "height", "font_size", "border_width", "radius", "duration_ms"):
            editor = QSpinBox()
            editor.setRange(0 if name not in ("width", "height", "font_size") else 1, 4096)
            editor.setValue(int(value))
            editor.valueChanged.connect(lambda number, key=name: self._set(key, number))
            return editor, self._spin_refresher(editor, int)
        if name == "opacity":
            editor = QSpinBox()
            editor.setRange(0, 255)
            editor.setValue(int(value))
            editor.valueChanged.connect(lambda number, key=name: self._set(key, number))
            return editor, self._spin_refresher(editor, int)
        if name in ("rotation", "stroke_width", "start_angle", "end_angle", "value", "minimum", "maximum"):
            editor = QDoubleSpinBox()
            editor.setRange(-100000.0, 100000.0)
            editor.setDecimals(2)
            editor.setValue(float(value))
            editor.valueChanged.connect(lambda number, key=name: self._set(key, number))
            return editor, self._spin_refresher(editor, float)
        if name == "align":
            editor = QComboBox()
            editor.addItems(["left", "center", "right"])
            editor.setCurrentText(str(value))
            editor.currentTextChanged.connect(lambda text, key=name: self._set(key, text))
            return editor, self._combo_text_refresher(editor)
        if name == "image_format":
            editor = QComboBox()
            editor.addItems(["auto", "rgb565", "rgba8888"])
            editor.setCurrentText(str(value).lower())
            editor.currentTextChanged.connect(lambda text, key=name: self._set(key, text))
            return editor, self._combo_text_refresher(editor, lower=True)
        if name == "action":
            editor = QComboBox()
            editor.addItem("None", "none")
            editor.addItem("Rotate by", "rotate_by")
            editor.addItem("Rotate to", "rotate_to")
            index = editor.findData(str(value))
            editor.setCurrentIndex(max(0, index))
            editor.currentIndexChanged.connect(
                lambda index, control=editor, key=name: self._set(key, control.itemData(index)))
            return editor, self._combo_data_refresher(editor)
        if name == "action_target":
            editor = QComboBox()
            editor.setEditable(False)
            editor.addItem("(none selected)", "")
            for target in self.available_targets:
                editor.addItem(target, target)
            index = editor.findData(str(value))
            editor.setCurrentIndex(max(0, index))
            editor.currentIndexChanged.connect(
                lambda index, control=editor, key=name: self._set(key, control.itemData(index)))
            return editor, self._combo_data_refresher(editor)
        if name == "easing":
            editor = QComboBox()
            editor.addItem("Linear", "linear")
            editor.addItem("Ease In Out", "ease_in_out")
            index = editor.findData(str(value))
            editor.setCurrentIndex(max(0, index))
            editor.currentIndexChanged.connect(
                lambda index, control=editor, key=name: self._set(key, control.itemData(index)))
            return editor, self._combo_data_refresher(editor)
        if name in ("color", "background", "border_color"):
            wrapper = QWidget()
            layout = QHBoxLayout(wrapper)
            layout.setContentsMargins(0, 0, 0, 0)
            line = QLineEdit(str(value))
            button = QPushButton("…")
            button.setFixedWidth(28)
            line.editingFinished.connect(lambda key=name, control=line: self._set(key, control.text()))
            button.clicked.connect(lambda checked=False, key=name, control=line: self._choose_color(key, control))
            layout.addWidget(line)
            layout.addWidget(button)
            return wrapper, self._line_refresher(line)
        if name == "source":
            wrapper = QWidget()
            layout = QHBoxLayout(wrapper)
            layout.setContentsMargins(0, 0, 0, 0)
            line = QLineEdit(str(value))
            button = QPushButton("Browse")
            line.editingFinished.connect(lambda key=name, control=line: self._set(key, control.text()))
            button.clicked.connect(lambda checked=False, control=line: self._choose_image(control))
            layout.addWidget(line)
            layout.addWidget(button)
            return wrapper, self._line_refresher(line)
        editor = QLineEdit(str(value))
        editor.editingFinished.connect(lambda key=name, control=editor: self._set(key, control.text()))
        return editor, self._line_refresher(editor)

    @staticmethod
    def _has_focus(control, focused) -> bool:
        if focused is None:
            return False
        return control is focused or control.isAncestorOf(focused)

    @classmethod
    def _spin_refresher(cls, control, caster):
        def refresh(value, focused):
            if cls._has_focus(control, focused):
                return  # the user is typing in this field; do not overwrite the text under the cursor
            if caster(control.value()) != caster(value):
                control.setValue(caster(value))
        return refresh

    @staticmethod
    def _combo_text_refresher(control, lower: bool = False):
        def refresh(value, focused):
            text = str(value).lower() if lower else str(value)
            if control.currentText() != text:
                control.setCurrentText(text)
        return refresh

    @staticmethod
    def _combo_data_refresher(control):
        def refresh(value, focused):
            index = control.findData(str(value))
            if index >= 0 and index != control.currentIndex():
                control.setCurrentIndex(index)
        return refresh

    @classmethod
    def _line_refresher(cls, control):
        def refresh(value, focused):
            if cls._has_focus(control, focused):
                return
            if control.text() != str(value):
                control.setText(str(value))
        return refresh

    def _set(self, name: str, value) -> None:
        if self._updating or self.widget_model is None:
            return
        setattr(self.widget_model, name, value)
        self.widget_model.normalize()
        self.changed.emit(self.widget_model)
        # normalize() may clamp a value (a gauge's value, for instance); write the result back into the panel.
        self._refresh_values()

    def _choose_color(self, name: str, line: QLineEdit) -> None:
        color = QColorDialog.getColor(QColor(line.text()[:7]), self, "Choose a colour",
                                      QColorDialog.ColorDialogOption.ShowAlphaChannel)
        if color.isValid():
            value = color.name(QColor.NameFormat.HexRgb).upper() + f"{color.alpha():02X}"
            line.setText(value)
            self._set(name, value)

    def _choose_image(self, line: QLineEdit) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Choose an image", "", "Images (*.png *.jpg *.jpeg *.bmp)")
        if path:
            line.setText(path)
            self._set("source", path)
