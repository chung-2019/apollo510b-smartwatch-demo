# Asset Provenance

## Author-created images

The home image, the watch faces, the UI Designer page image and the weather
page were created by the project author -- for this project, or for the
author's earlier Apollo4 Plus watch face project -- using AI image generation
tools or by hand. They are not taken from third-party image libraries.

No private filesystem paths, unrelated project names, or conversation content
are required to build this repository.

| Included resource | What it is | Source and processing |
| --- | --- | --- |
| `tools/assets_out/HOME.BIN`, `src/Generated_2/test_image_rgb565.h` | home page image (EdgePilot), 454 x 454 | author-created; RGB565 conversion with `tools/png_to_bin.py` |
| `WATCHBG.BIN`, `HANDHR/HANDMIN/HANDSEC.BIN`, `src/generated/Watch_face/Watch7*.h` | watch face "chrono" | from the author's earlier Apollo4 Plus watch face project; the dial's inner glow was flattened before conversion |
| `WF2*.BIN` | watch face "aurora" (not loaded) | author-created; `tools/face2_prep.py` |
| `WF3*.BIN` | watch face "numbers" | author-created; `tools/face3_prep.py` (hands scaled by 454/390 with LANCZOS) |
| `WF4*.BIN` | watch face "ambiq" | author-created artwork showing the Ambiq logo; `tools/face4_prep.py` (centred on the knob, hand ghost repaired, tails cropped) |
| `src/generated/ui_designer/ui_designer_assets.c` | UI Designer page image, 178 x 191 | author-created; emitted by `tools/nemagfx_ui_designer` |
| `src/Generate_3/weather_cloud_01..05.png` -> `WTHRBG0..4.BIN` | weather page backgrounds, 468 x 468 | author-created; RGB565 conversion with `tools/weather_prep.py` |
| `src/Generate_3/original/weather_ui_01.png` | the original weather composite (overlay baked into a sky) | generated with an AI image generation tool over one of the backgrounds; the prompts are in `src/Generate_3/weather_ui_prompts.md` |
| `src/Generate_3/weather_overlay.png` -> `WTHRUI.BIN` | weather page overlay, 348 x 392 | drawn by `tools/weather_prep.py`: the text is rendered with Noto Sans TC (SIL Open Font License 1.1; the font itself is not included), and the forecast cards are cropped from the original composite |

The weather page's date, place, temperature and forecast are illustrative
sample values, not weather data.

The weather assets in `tools/assets_out/` have been regenerated from the
included sources with `tools/weather_prep.py` and match the checked-in
`WTHRBG0..4.BIN` and `WTHRUI.BIN` exactly, byte for byte. The source PNGs of
the other images are not included (see README section 8); the checked-in
`.BIN` files and C arrays are their converted output.

The project author makes the author-created image data available under the
project BSD-3-Clause license to the extent the author holds applicable rights
in that data.

## Third-party graphics and data

| Included resource | Source |
| --- | --- |
| `src/generated/map/map_tile_0*.h`, `MAPTILE0..2.BIN` | rendered by the author's `tools/render_taipei_osm_tiles.py` from OpenStreetMap data -- map data © OpenStreetMap contributors, ODbL (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) |
| `src/generated/blowing.h`, `climate.h`, `day.h` | TSVG weather icons from the AmbiqSuite `nemagfx_watchface` example (Ambiq notice; currently not built in) |
| `src/font/DejaVuSans_ttf_kern.c`, `src/font/DejaVuSerif24pt4b.c` | DejaVu font data from the AmbiqSuite NemaGFX examples (DejaVu fonts license) |
