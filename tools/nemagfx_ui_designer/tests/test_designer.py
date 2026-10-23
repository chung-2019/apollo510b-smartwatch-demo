from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

TEST_DIR = Path(__file__).resolve().parent
TOOL_DIR = TEST_DIR.parent
sys.path.insert(0, str(TOOL_DIR))

import font_metrics
from generator import generate_project, rgba8888
from model import Scene, UIProject, Widget

#
# The NemaVG font this project actually uses, for verifying the metrics parsing. The tests
# are skipped when it is not found -- the tool itself is not tied to this repo's layout.
#
REPO_FONT_DIR = TOOL_DIR.parent.parent / "src" / "generated" / "ui_designer"

try:
    from PIL import Image
except ImportError:
    Image = None


class ModelAndGeneratorTests(unittest.TestCase):
    def test_default_resolution_is_apollo510b_resolution(self) -> None:
        project = UIProject()
        self.assertEqual((project.project.display_width, project.project.display_height), (454, 454))
        self.assertEqual((project.project.framebuffer_width, project.project.framebuffer_height), (468, 468))
        self.assertEqual((project.project.origin_x, project.project.origin_y), (7, 7))

    def test_project_round_trip(self) -> None:
        project = UIProject()
        project.scenes[0].widgets.append(Widget(type="label", name="Temperature", text="37.25 °C"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "project.json"
            project.save(path)
            loaded = UIProject.load(path)
        self.assertEqual(loaded.scenes[0].widgets[0].text, "37.25 °C")
        self.assertEqual(loaded.scenes[0].widgets[0].name, "temperature")

    def test_nema_color_packing(self) -> None:
        self.assertEqual(rgba8888("#11223344"), 0x44332211)
        self.assertEqual(rgba8888("#FF0000"), 0xFF0000FF)

    def test_generate_c_and_bindings(self) -> None:
        project = UIProject.load(TOOL_DIR / "sample_454.json")
        with tempfile.TemporaryDirectory() as directory:
            result = generate_project(project, directory, TOOL_DIR / "sample_454.json")
            generated = (Path(directory) / "ui_designer_generated.c").read_text(encoding="utf-8")
            bindings = (Path(directory) / "ui_designer_bindings.h").read_text(encoding="utf-8")
        self.assertEqual(len(result.files), 10)
        self.assertIn("nema_vg_bind_font(&DejaVuSans_ttf_kern);", generated)
        self.assertIn("nema_vg_draw_ring", generated)
        self.assertIn("ui_event_connect_clicked", generated)
        self.assertIn("ui_binding_set_temperature_text", bindings)
        self.assertIn("ui_binding_set_battery_level_value", bindings)

    def test_canvas_is_centered_on_framebuffer(self) -> None:
        #
        # The canvas (the circle the watch shows) is smaller than the framebuffer, and code
        # generation has to centre the whole canvas, or canvas coordinates would be taken
        # as framebuffer coordinates and the whole page would sit 7 px up and left -- which
        # is exactly the side the bezel cuts off. Drawing and hit testing both need the
        # offset; with only one of them right, a button is visible but untappable.
        #
        button = Widget(type="button", name="test", x=0, y=0, width=100, height=50,
                        event="tapped")
        project = UIProject(scenes=[Scene(name="home", widgets=[button])])
        with tempfile.TemporaryDirectory() as directory:
            generate_project(project, directory)
            generated = (Path(directory) / "ui_designer_generated.c").read_text(encoding="utf-8")
            config = (Path(directory) / "ui_designer_project_config.h").read_text(encoding="utf-8")
        self.assertIn("#define UI_DESIGNER_ORIGIN_X           (7)", config)
        self.assertIn("#define UI_DESIGNER_FRAMEBUFFER_WIDTH  (468)", config)
        self.assertIn("nema_fill_rounded_rect(7, 7, 100, 50,", generated)
        self.assertIn("if ((x >= 7) && (x < 107) &&", generated)
        self.assertIn("(y >= 7) && (y < 57))", generated)
        #
        # The background fills the whole framebuffer: filling only the canvas leaves the previous page's image in the outer ring.
        #
        self.assertIn(
            "nema_fill_rect(0, 0, UI_DESIGNER_FRAMEBUFFER_WIDTH, UI_DESIGNER_FRAMEBUFFER_HEIGHT,",
            generated,
        )

    def test_button_rotation_action_generation(self) -> None:
        clock = Widget(type="image", name="clock", width=160, height=160)
        button = Widget(type="button", name="test", action="rotate_by",
                        action_target="clock", action_value=360.0,
                        duration_ms=800, easing="ease_in_out")
        project = UIProject(scenes=[Scene(name="home", widgets=[clock, button])])
        with tempfile.TemporaryDirectory() as directory:
            generate_project(project, directory)
            generated = (Path(directory) / "ui_designer_generated.c").read_text(encoding="utf-8")
            header = (Path(directory) / "ui_designer_generated.h").read_text(encoding="utf-8")
        self.assertIn("ui_designer_tick(uint32_t delta_ms)", header)
        self.assertIn(
            "ui_designer_start_rotation(&s_rotation_home_clock, true, 360.000f, 800U, UI_EASING_IN_OUT);",
            generated,
        )
        #
        # This clock has no source, so _render_widget() leaves only a placeholder comment and
        # nothing calls ui_designer_blit_rotated(). The helper is static, so emitting it
        # anyway is an unused-function warning under -Wall, and it must not appear.
        #
        # It comes back when there really is an image to draw; see test_rotated_image_emits_blit_helper().
        #
        self.assertNotIn("ui_designer_blit_rotated", generated)

    #
    # Drawing helpers no scene uses are not emitted, or every Generate C would raise an
    # unused-function warning under -Wall. This project has only a label, so ring/gauge and
    # blit must not appear while draw_text must.
    #
    def test_unused_draw_helpers_are_not_emitted(self) -> None:
        label = Widget(type="label", name="title", text="Hello")
        project = UIProject(scenes=[Scene(name="home", widgets=[label])])
        with tempfile.TemporaryDirectory() as directory:
            generate_project(project, directory)
            generated = (Path(directory) / "ui_designer_generated.c").read_text(encoding="utf-8")
        self.assertIn("ui_designer_draw_text", generated)
        self.assertNotIn("ui_designer_draw_ring", generated)
        self.assertNotIn("ui_designer_draw_gauge", generated)
        self.assertNotIn("ui_designer_blit_rotated", generated)
        #
        # tick is public API declared in the .h and needs a definition even with no animation, or the caller cannot link.
        #
        self.assertIn("ui_designer_tick(uint32_t delta_ms)", generated)

    #
    # The control case: with the image source really present, the blit helper and its call
    # must both be generated. This is the other half of the assertNotIn above -- without it,
    # "do not emit the helper" could easily become "images are never drawn again" unnoticed.
    #
    @unittest.skipIf(Image is None, "Pillow is not installed")
    def test_rotated_image_emits_blit_helper(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            Image.new("RGBA", (4, 4), (10, 20, 30, 255)).save(base / "clock.png")
            clock = Widget(type="image", name="clock", width=4, height=4,
                           source="clock.png")
            button = Widget(type="button", name="test", action="rotate_by",
                            action_target="clock", action_value=90.0,
                            duration_ms=800, easing="ease_in_out")
            project = UIProject(scenes=[Scene(name="home", widgets=[clock, button])])
            project_file = base / "project.json"
            project.save(project_file)
            generate_project(project, base / "generated", project_file)
            generated = (base / "generated" / "ui_designer_generated.c").read_text(encoding="utf-8")
        self.assertIn("ui_designer_blit_rotated(const ui_designer_image_t *image", generated)
        self.assertIn("ui_designer_blit_rotated(&g_ui_asset_clock,", generated)
        self.assertIn("s_rotation_home_clock", generated)

    #
    # The integrating side need not guard against "not initialized yet": draw still fills the
    # canvas with that scene's background colour and touch always returns false. Without those two, the caller would have to keep a ready flag.
    #
    def test_uninitialized_draw_still_clears_and_touch_is_inert(self) -> None:
        button = Widget(type="button", name="go", event="go_clicked")
        project = UIProject(scenes=[Scene(name="home", widgets=[button])])
        with tempfile.TemporaryDirectory() as directory:
            generate_project(project, directory)
            generated = (Path(directory) / "ui_designer_generated.c").read_text(encoding="utf-8")
        draw = generated.split("ui_designer_draw(ui_scene_id_t scene)")[1]
        draw = draw.split("ui_designer_touch")[0]
        self.assertIn("ui_designer_fill_background(scene);", draw)
        touch = generated.split("ui_designer_touch(ui_scene_id_t scene")[1]
        self.assertIn("return false;", touch.split("switch (scene)")[0])
        #
        # The background colour comes from a table, shared by the scene's draw and the uninitialized fallback.
        #
        self.assertIn("s_ui_scene_background[UI_SCENE_COUNT]", generated)

    #
    # The frame interval is computed by the generated side and the caller keeps no previous
    # frame time. resync is for "away from this screen for a while and back"; without it, the first frame back runs the whole animation at once.
    #
    def test_tick_to_and_resync_are_generated(self) -> None:
        project = UIProject(scenes=[Scene(name="home", widgets=[Widget(type="label", name="a")])])
        with tempfile.TemporaryDirectory() as directory:
            generate_project(project, directory)
            generated = (Path(directory) / "ui_designer_generated.c").read_text(encoding="utf-8")
            header = (Path(directory) / "ui_designer_generated.h").read_text(encoding="utf-8")
        for symbol in ("ui_designer_tick_to(uint32_t now_ms)",
                       "ui_designer_resync(uint32_t now_ms)"):
            self.assertIn(symbol, generated)          # definition
            self.assertIn(f"void {symbol};", header)  # declaration
        #
        # The first call only records the base, or the first frame after boot would have the whole of now_ms as its delta.
        #
        self.assertIn("if (!s_tick_primed)", generated)
        #
        # init has to clear primed, so a re-initialization does not inherit the previous round's base.
        #
        self.assertIn("s_tick_primed = false;", generated)

    #
    # The caller need not hard-code the scene's name. After a rename, UI_SCENE_DEFAULT points
    # at the new enum value while a hard-coded old name is a compile error rather than a silent failure.
    #
    def test_scene_default_alias_points_at_first_scene(self) -> None:
        project = UIProject(scenes=[Scene(name="dashboard", widgets=[]),
                                    Scene(name="settings", widgets=[])])
        with tempfile.TemporaryDirectory() as directory:
            generate_project(project, directory)
            header = (Path(directory) / "ui_designer_generated.h").read_text(encoding="utf-8")
        self.assertIn("#define UI_SCENE_DEFAULT               (UI_SCENE_DASHBOARD)", header)

    #
    # The Makefile fragment lets the project's Makefile get by with one include, with the
    # source list maintained by the generator. The directory derives itself from MAKEFILE_LIST, so moving the output directory needs no change to the fragment.
    #
    def test_makefile_fragment_is_self_locating(self) -> None:
        project = UIProject()
        with tempfile.TemporaryDirectory() as directory:
            generate_project(project, directory)
            fragment = (Path(directory) / "ui_designer.mk").read_text(encoding="utf-8")
        self.assertIn("$(dir $(lastword $(MAKEFILE_LIST)))", fragment)
        self.assertIn("SRC      += ui_designer_generated.c", fragment)
        self.assertIn("VPATH    +=:$(UI_DESIGNER_DIR)", fragment)
        self.assertIn("INCLUDES += -I$(UI_DESIGNER_DIR)", fragment)

    #
    # A renamed button callback is the only place this tool can fail silently: the user's
    # override shadows a weak definition, and the linker says nothing when the names differ. So generation compares against the previous snapshot.
    #
    def test_event_symbol_rename_is_warned(self) -> None:
        before = UIProject(scenes=[Scene(name="home", widgets=[
            Widget(type="button", name="b", event="connect_clicked")])])
        after = UIProject(scenes=[Scene(name="home", widgets=[
            Widget(type="button", name="b", event="pair_clicked")])])
        with tempfile.TemporaryDirectory() as directory:
            first = generate_project(before, directory)
            second = generate_project(after, directory)
        #
        # The first time there is no previous version to compare against and nothing should be reported.
        #
        self.assertEqual([w for w in first.warnings if "callback" in w], [])
        joined = "\n".join(second.warnings)
        self.assertIn("ui_event_connect_clicked()", joined)
        self.assertIn("no longer exist", joined)
        self.assertIn("ui_event_pair_clicked()", joined)

    #
    # The rotation direction has to match the canvas preview (QGraphicsItem::setRotation(), positive is clockwise).
    # The generated code once carried an extra minus sign, and hardware turned the opposite way from the preview.
    #
    # A real image is needed: the blit helper is emitted on demand, and with no image it is not generated at all.
    #
    @unittest.skipIf(Image is None, "Pillow is not installed")
    def test_rotation_matches_preview_direction(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            Image.new("RGBA", (4, 4), (10, 20, 30, 255)).save(base / "dial.png")
            dial = Widget(type="image", name="dial", width=4, height=4, source="dial.png")
            project = UIProject(scenes=[Scene(name="home", widgets=[dial])])
            project_file = base / "project.json"
            project.save(project_file)
            generate_project(project, base / "generated", project_file)
            generated = (base / "generated" / "ui_designer_generated.c").read_text(encoding="utf-8")
        self.assertIn("nema_mat3x3_rotate(matrix, angle);", generated)
        self.assertNotIn("nema_mat3x3_rotate(matrix, -angle);", generated)

    #
    # Text wider than the box has to shrink on hardware, or nema_vg_print() clips the overflow.
    # It must use NemaVG's own measurement (nema_vg_string_get_bbox) rather than a constant
    # computed at generation time -- the canvas's widths come from a PC font and do not match hardware.
    #
    def test_draw_text_shrinks_to_fit_at_runtime(self) -> None:
        project = UIProject(scenes=[Scene(name="home", widgets=[
            Widget(type="label", name="title", text="Ambiq_EdgePilot")])])
        with tempfile.TemporaryDirectory() as directory:
            generate_project(project, directory)
            generated = (Path(directory) / "ui_designer_generated.c").read_text(encoding="utf-8")
        helper = generated.split("ui_designer_draw_text(const char")[1].split("\n}")[0]
        self.assertIn("nema_vg_string_get_bbox(text", helper)
        self.assertIn(
            "nema_vg_set_font_size(size * UI_TEXT_FIT_RATIO * ((float)width / measured_w));",
            helper,
        )
        #
        # measured_w is 0 when the measurement fails and must not be used as a divisor.
        #
        self.assertIn("(measured_w > 1.0f)", helper)
        #
        # The measurement's limit must not be the widget's box width: nema_vg_string_get_bbox()
        # clamps its return to max_w, so with the box width passed in measured_w never exceeds
        # width and the automatic shrink effectively does not exist -- on hardware the text is
        # clipped at the right edge by nema_vg_print() (the regression of 2026-09-14).
        #
        self.assertIn("UI_TEXT_MEASURE_MAX_W, 0);", helper)
        self.assertNotIn("&measured_h, (float)width", helper)
        self.assertIn("#define UI_TEXT_MEASURE_MAX_W   (8192.0f)", generated)

    @unittest.skipIf(not REPO_FONT_DIR.is_dir(), "repo font directory not available")
    def test_overflowing_text_is_warned(self) -> None:
        #
        # Shrinking the font size is hardware's backstop, not the design intent: a box too
        # narrow should be reported at Generate C time, or it only shows up as an inexplicably smaller line on hardware.
        #
        project = UIProject(scenes=[Scene(name="home", widgets=[
            Widget(type="label", name="title", text="Ambiq_EdgePilot",
                   width=220, height=60, font_size=28)])])
        with tempfile.TemporaryDirectory() as directory:
            #
            # Generating into a temp directory outside the repo, the advance table can only be found from the project file's location.
            #
            result = generate_project(project, directory, TOOL_DIR / "sample_454.json")
        self.assertTrue(any("shrink it to about" in warning for warning in result.warnings),
                        result.warnings)

    @unittest.skipIf(not REPO_FONT_DIR.is_dir(), "repo font directory not available")
    def test_font_metrics_parses_nemavg_font(self) -> None:
        metrics = font_metrics.load("font/DejaVuSans_ttf_kern.h", REPO_FONT_DIR)
        self.assertIsNotNone(metrics)
        self.assertEqual(metrics.units_per_em, 2048)
        #
        # The space advance of DejaVu Sans is 651/2048 em, and that is what the font's
        # xAdvance field holds. A match means both the field position and the scale parse correctly.
        #
        self.assertEqual(metrics.default_advance, 651.0)
        self.assertEqual(metrics.advances[ord(" ")], 651.0)
        #
        # Both ranges have to be picked up: ASCII 0x20-0x7E and Greek 0x370-0x3FF.
        #
        self.assertIn(ord("A"), metrics.advances)
        self.assertIn(0x3A9, metrics.advances)
        #
        # Text width is proportional to the size, and wider than a single character.
        #
        single = metrics.text_width("A", 28.0)
        self.assertGreater(metrics.text_width("AAAA", 28.0), single)
        self.assertAlmostEqual(metrics.text_width("A", 56.0), single * 2.0, places=4)

    #
    # The estimate has to match hardware. The numbers below were measured on apollo510b_evb
    # with nema_vg_string_get_bbox() (design_font_probe() in render_task.c).
    #
    # "A" and "test" have no kerning pairs and the estimate should agree exactly;
    # "Ambiq_EdgePilot" has kerning, which NemaVG's bbox reflects only partly, so 2 px of error is allowed.
    #
    # What these numbers pin down is the font-units-to-pixel divisor (units_per_em * 3/4).
    # Change it back to the typographic convention of units_per_em and this goes red.
    #
    @unittest.skipIf(not REPO_FONT_DIR.is_dir(), "repo font directory not available")
    def test_font_metrics_matches_device_measurements(self) -> None:
        metrics = font_metrics.load("font/DejaVuSans_ttf_kern.h", REPO_FONT_DIR)
        self.assertIsNotNone(metrics)
        for text, size, measured, tolerance in (
            ("A", 28.0, 25.54, 0.01),
            ("test", 28.0, 71.70, 0.01),
            ("A", 56.0, 51.08, 0.01),
            ("test", 56.0, 143.39, 0.02),
            ("Ambiq_EdgePilot", 28.0, 311.71, 2.0),
        ):
            estimate = metrics.text_width(text, size)
            self.assertAlmostEqual(
                estimate, measured, delta=tolerance,
                msg=f'"{text}" @{size:g}px: estimate {estimate:.2f}, hardware {measured:.2f}')
        #
        # This font has no 'degree sign' (outside 0x20-0x7E), and a missing character has to be reported.
        #
        self.assertEqual(font_metrics.missing_glyphs(metrics, "25°C"), ["°"])
        self.assertEqual(font_metrics.missing_glyphs(metrics, "ok"), [])

    def test_font_metrics_missing_font_is_not_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            self.assertIsNone(font_metrics.load("font/NoSuchFont.h", Path(directory)))

    @unittest.skipIf(Image is None, "Pillow is not installed")
    def test_png_to_rgba8888_asset(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source = base / "icon.png"
            Image.new("RGBA", (2, 2), (1, 2, 3, 128)).save(source)
            widget = Widget(type="image", name="icon", width=2, height=2,
                            source="icon.png", image_format="auto")
            project = UIProject(scenes=[Scene(widgets=[widget])])
            project_file = base / "project.json"
            project.save(project_file)
            result = generate_project(project, base / "generated", project_file)
            assets = (base / "generated" / "ui_designer_assets.c").read_text(encoding="utf-8")
        self.assertEqual(result.asset_bytes, 16)
        self.assertIn("NEMA_RGBA8888", assets)
        self.assertIn("0x01, 0x02, 0x03, 0x80", assets)


if __name__ == "__main__":
    unittest.main()
