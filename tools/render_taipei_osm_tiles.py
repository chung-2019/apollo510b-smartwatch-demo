#!/usr/bin/env python3
"""Render offline Taipei City navigation tiles from Geofabrik OSM shapefiles.

The output layout is the standard slippy-map hierarchy::

    <output>/z15/<x>/<y>.png

Every PNG is 468 x 468 pixels.  The geographic footprint of a tile follows
the normal Web Mercator z/x/y grid; only the raster density differs from the
usual 256-pixel web tile.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

import shapefile
from PIL import Image, ImageDraw, ImageFont
from shapely.geometry import box, shape as shapely_shape


TAIPEI_OSM_ID = "1293250"
DEFAULT_ZOOM = 15
DEFAULT_TILE_SIZE = 468

BACKGROUND = "#152233"
BUILDING_FILL = "#314155"
BUILDING_OUTLINE = "#3b4d62"
WATER_FILL = "#174866"
WATERWAY = "#28759a"
RAILWAY = "#8b819d"
ROAD_CASING = "#273748"
TEXT = "#f2f5f7"
TEXT_HALO = "#111b29"
POI_TEXT = "#dfe8ee"
CITY_BOUNDARY = "#75869a"


@dataclass(frozen=True)
class PolygonFeature:
    points: tuple[tuple[float, float], ...]
    parts: tuple[int, ...]
    fill: str
    outline: str | None = None
    outline_width: int = 1


@dataclass(frozen=True)
class LineFeature:
    points: tuple[tuple[float, float], ...]
    parts: tuple[int, ...]
    color: str
    width: int
    rank: int = 0
    casing_color: str | None = None
    casing_width: int = 0
    name: str = ""


@dataclass(frozen=True)
class PointFeature:
    lon: float
    lat: float
    name: str
    kind: str
    priority: int


def lon_to_tile_x(lon: float, zoom: int) -> float:
    return (lon + 180.0) / 360.0 * (1 << zoom)


def lat_to_tile_y(lat: float, zoom: int) -> float:
    lat = max(-85.05112878, min(85.05112878, lat))
    rad = math.radians(lat)
    return (1.0 - math.asinh(math.tan(rad)) / math.pi) * 0.5 * (1 << zoom)


def tile_x_to_lon(x: float, zoom: int) -> float:
    return x / (1 << zoom) * 360.0 - 180.0


def tile_y_to_lat(y: float, zoom: int) -> float:
    n = math.pi - 2.0 * math.pi * y / (1 << zoom)
    return math.degrees(math.atan(math.sinh(n)))


def tile_bounds(x: int, y: int, zoom: int) -> tuple[float, float, float, float]:
    west = tile_x_to_lon(x, zoom)
    east = tile_x_to_lon(x + 1, zoom)
    north = tile_y_to_lat(y, zoom)
    south = tile_y_to_lat(y + 1, zoom)
    return west, south, east, north


def to_pixel(lon: float, lat: float, tile_x: int, tile_y: int,
             zoom: int, tile_size: int) -> tuple[float, float]:
    px = (lon_to_tile_x(lon, zoom) - tile_x) * tile_size
    py = (lat_to_tile_y(lat, zoom) - tile_y) * tile_size
    return px, py


def find_layer(source_dir: Path, short_name: str) -> Path:
    matches = list(source_dir.glob(f"gis_osm_{short_name}_free_1.shp"))
    if len(matches) != 1:
        raise FileNotFoundError(f"Expected one {short_name} shapefile in {source_dir}")
    return matches[0]


def record_dict(reader: shapefile.Reader, record: shapefile._Record) -> dict[str, object]:
    # pyshp already stores the field positions on each Record.  Using
    # ``as_dict`` avoids rebuilding the layer's field-name list for every
    # one of the hundreds of thousands of Taipei features.
    return record.as_dict()


def load_taipei_boundary(source_dir: Path):
    reader = shapefile.Reader(str(find_layer(source_dir, "adminareas_a")), encoding="utf-8")
    try:
        for item in reader.iterShapeRecords():
            data = record_dict(reader, item.record)
            if str(data.get("osm_id")) == TAIPEI_OSM_ID:
                return shapely_shape(item.shape.__geo_interface__)
    finally:
        reader.close()
    raise RuntimeError(f"Taipei administrative boundary {TAIPEI_OSM_ID} not found")


def shape_parts(points: Iterable[Iterable[float]], parts: Iterable[int]) -> tuple[
        tuple[tuple[float, float], ...], tuple[int, ...]]:
    point_tuple = tuple((float(p[0]), float(p[1])) for p in points)
    part_tuple = tuple(int(p) for p in parts)
    return point_tuple, part_tuple


def feature_bbox(shape_obj) -> tuple[float, float, float, float] | None:
    if hasattr(shape_obj, "bbox") and shape_obj.bbox:
        return tuple(float(v) for v in shape_obj.bbox)
    if shape_obj.points:
        lon, lat = shape_obj.points[0]
        return float(lon), float(lat), float(lon), float(lat)
    return None


def bboxes_overlap(a: tuple[float, float, float, float],
                   b: tuple[float, float, float, float]) -> bool:
    return not (a[2] < b[0] or a[0] > b[2] or a[3] < b[1] or a[1] > b[3])


def tile_range_for_bbox(feature_bounds: tuple[float, float, float, float], zoom: int,
                        grid: tuple[int, int, int, int]) -> tuple[int, int, int, int] | None:
    min_lon, min_lat, max_lon, max_lat = feature_bounds
    min_x = math.floor(lon_to_tile_x(min_lon, zoom))
    max_x = math.floor(lon_to_tile_x(max_lon, zoom))
    min_y = math.floor(lat_to_tile_y(max_lat, zoom))
    max_y = math.floor(lat_to_tile_y(min_lat, zoom))
    grid_min_x, grid_max_x, grid_min_y, grid_max_y = grid
    min_x = max(min_x, grid_min_x)
    max_x = min(max_x, grid_max_x)
    min_y = max(min_y, grid_min_y)
    max_y = min(max_y, grid_max_y)
    if min_x > max_x or min_y > max_y:
        return None
    return min_x, max_x, min_y, max_y


def add_to_bins(bins: dict[tuple[int, int], list], feature, bounds,
                zoom: int, grid: tuple[int, int, int, int]) -> None:
    tile_range = tile_range_for_bbox(bounds, zoom, grid)
    if tile_range is None:
        return
    min_x, max_x, min_y, max_y = tile_range
    for x in range(min_x, max_x + 1):
        for y in range(min_y, max_y + 1):
            bins[(x, y)].append(feature)


def landuse_style(fclass: str) -> tuple[str, str | None]:
    styles = {
        "forest": ("#28463d", None),
        "park": ("#315440", None),
        "grass": ("#2e4e3d", None),
        "recreation_ground": ("#315440", None),
        "farmland": ("#344936", None),
        "meadow": ("#344d3b", None),
        "orchard": ("#344d3b", None),
        "cemetery": ("#344b42", None),
        "residential": ("#1b2a3a", None),
        "commercial": ("#263445", None),
        "retail": ("#293747", None),
        "industrial": ("#293240", None),
        "military": ("#3d343e", None),
        "quarry": ("#3c4249", None),
    }
    return styles.get(fclass, ("#243445", None))


def road_style(fclass: str) -> tuple[str, int, int, str | None, int] | None:
    # color, inner width, rank, casing color, casing width
    styles = {
        "footway": ("#556675", 1, 1, None, 0),
        "path": ("#4c5f6d", 1, 1, None, 0),
        "steps": ("#536574", 1, 1, None, 0),
        "cycleway": ("#446b70", 1, 1, None, 0),
        "track": ("#4a5d67", 1, 1, None, 0),
        "service": ("#677786", 1, 2, None, 0),
        "pedestrian": ("#71818f", 2, 2, ROAD_CASING, 3),
        "living_street": ("#8796a3", 2, 3, ROAD_CASING, 4),
        "unclassified": ("#8796a3", 2, 3, ROAD_CASING, 4),
        "residential": ("#9facb7", 3, 4, ROAD_CASING, 5),
        "tertiary": ("#c9d2d9", 4, 5, ROAD_CASING, 6),
        "tertiary_link": ("#c9d2d9", 3, 5, ROAD_CASING, 5),
        "secondary": ("#dbe1e6", 6, 6, ROAD_CASING, 8),
        "secondary_link": ("#dbe1e6", 5, 6, ROAD_CASING, 7),
        "primary": ("#edf0f2", 8, 7, ROAD_CASING, 11),
        "primary_link": ("#edf0f2", 6, 7, ROAD_CASING, 9),
        "trunk": ("#e6e9ec", 9, 8, ROAD_CASING, 12),
        "trunk_link": ("#e6e9ec", 7, 8, ROAD_CASING, 10),
        "motorway": ("#d9e1e6", 10, 9, ROAD_CASING, 13),
        "motorway_link": ("#d9e1e6", 8, 9, ROAD_CASING, 11),
    }
    return styles.get(fclass)


def load_polygon_layer(source_dir: Path, layer: str, city_bbox, zoom: int, grid,
                       style_fn: Callable[[dict[str, object]], tuple[str, str | None] | None]):
    bins: dict[tuple[int, int], list[PolygonFeature]] = defaultdict(list)
    reader = shapefile.Reader(str(find_layer(source_dir, layer)), encoding="utf-8")
    loaded = 0
    try:
        for item in reader.iterShapeRecords(bbox=city_bbox):
            bounds = feature_bbox(item.shape)
            if bounds is None or not bboxes_overlap(bounds, city_bbox):
                continue
            data = record_dict(reader, item.record)
            style = style_fn(data)
            if style is None:
                continue
            fill, outline = style
            points, parts = shape_parts(item.shape.points, item.shape.parts)
            feature = PolygonFeature(points, parts, fill, outline)
            add_to_bins(bins, feature, bounds, zoom, grid)
            loaded += 1
    finally:
        reader.close()
    print(f"loaded {layer}: {loaded}", flush=True)
    return bins


def load_line_layer(source_dir: Path, layer: str, city_bbox, zoom: int, grid,
                    style_fn: Callable[[dict[str, object]], tuple | None]):
    bins: dict[tuple[int, int], list[LineFeature]] = defaultdict(list)
    reader = shapefile.Reader(str(find_layer(source_dir, layer)), encoding="utf-8")
    loaded = 0
    try:
        for item in reader.iterShapeRecords(bbox=city_bbox):
            bounds = feature_bbox(item.shape)
            if bounds is None or not bboxes_overlap(bounds, city_bbox):
                continue
            data = record_dict(reader, item.record)
            style = style_fn(data)
            if style is None:
                continue
            color, width, rank, casing_color, casing_width = style
            points, parts = shape_parts(item.shape.points, item.shape.parts)
            feature = LineFeature(
                points, parts, color, width, rank, casing_color, casing_width,
                str(data.get("name") or "").strip(),
            )
            add_to_bins(bins, feature, bounds, zoom, grid)
            loaded += 1
    finally:
        reader.close()
    print(f"loaded {layer}: {loaded}", flush=True)
    return bins


def load_point_labels(source_dir: Path, layer: str, city_bbox, zoom: int, grid,
                      allowed: dict[str, int]):
    bins: dict[tuple[int, int], list[PointFeature]] = defaultdict(list)
    reader = shapefile.Reader(str(find_layer(source_dir, layer)), encoding="utf-8")
    loaded = 0
    try:
        for item in reader.iterShapeRecords():
            if not item.shape.points:
                continue
            lon, lat = item.shape.points[0]
            if not (city_bbox[0] <= lon <= city_bbox[2] and city_bbox[1] <= lat <= city_bbox[3]):
                continue
            data = record_dict(reader, item.record)
            name = str(data.get("name") or "").strip()
            kind = str(data.get("fclass") or "")
            if not name or kind not in allowed:
                continue
            feature = PointFeature(float(lon), float(lat), name, kind, allowed[kind])
            key = (math.floor(lon_to_tile_x(lon, zoom)), math.floor(lat_to_tile_y(lat, zoom)))
            if grid[0] <= key[0] <= grid[1] and grid[2] <= key[1] <= grid[3]:
                bins[key].append(feature)
                loaded += 1
    finally:
        reader.close()
    print(f"loaded {layer} labels: {loaded}", flush=True)
    return bins


def merge_bins(*sources):
    merged = defaultdict(list)
    for source in sources:
        for key, values in source.items():
            merged[key].extend(values)
    return merged


def part_ranges(feature) -> Iterable[tuple[int, int]]:
    starts = feature.parts or (0,)
    ends = starts[1:] + (len(feature.points),)
    yield from zip(starts, ends)


def draw_polygon(draw: ImageDraw.ImageDraw, feature: PolygonFeature,
                 tile_x: int, tile_y: int, zoom: int, tile_size: int) -> None:
    for start, end in part_ranges(feature):
        coords = [to_pixel(lon, lat, tile_x, tile_y, zoom, tile_size)
                  for lon, lat in feature.points[start:end]]
        if len(coords) >= 3:
            draw.polygon(coords, fill=feature.fill, outline=feature.outline,
                         width=feature.outline_width)


def line_parts(feature: LineFeature, tile_x: int, tile_y: int,
               zoom: int, tile_size: int):
    for start, end in part_ranges(feature):
        coords = [to_pixel(lon, lat, tile_x, tile_y, zoom, tile_size)
                  for lon, lat in feature.points[start:end]]
        if len(coords) >= 2:
            yield coords


def draw_line_pass(draw: ImageDraw.ImageDraw, feature: LineFeature,
                   tile_x: int, tile_y: int, zoom: int, tile_size: int,
                   casing: bool) -> None:
    color = feature.casing_color if casing else feature.color
    width = feature.casing_width if casing else feature.width
    if not color or width <= 0:
        return
    for coords in line_parts(feature, tile_x, tile_y, zoom, tile_size):
        draw.line(coords, fill=color, width=width, joint="curve")


def longest_part_midpoint(feature: LineFeature, tile_x: int, tile_y: int,
                          zoom: int, tile_size: int) -> tuple[float, float] | None:
    best = None
    best_length = 0.0
    for coords in line_parts(feature, tile_x, tile_y, zoom, tile_size):
        for a, b in zip(coords, coords[1:]):
            length = math.hypot(b[0] - a[0], b[1] - a[1])
            if length > best_length:
                best_length = length
                best = ((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5)
    return best if best_length >= 55 else None


def overlaps_any(candidate, occupied) -> bool:
    x0, y0, x1, y1 = candidate
    for ox0, oy0, ox1, oy1 in occupied:
        if not (x1 < ox0 or x0 > ox1 or y1 < oy0 or y0 > oy1):
            return True
    return False


def draw_centered_label(draw: ImageDraw.ImageDraw, xy, text: str, font,
                        occupied, fill=TEXT, stroke_width=3,
                        min_margin=4) -> bool:
    bbox = draw.textbbox(xy, text, font=font, anchor="mm", stroke_width=stroke_width)
    padded = (bbox[0] - 3, bbox[1] - 2, bbox[2] + 3, bbox[3] + 2)
    if padded[0] < min_margin or padded[1] < min_margin or padded[2] > DEFAULT_TILE_SIZE - min_margin or padded[3] > DEFAULT_TILE_SIZE - min_margin:
        return False
    if overlaps_any(padded, occupied):
        return False
    draw.text(xy, text, font=font, anchor="mm", fill=fill,
              stroke_width=stroke_width, stroke_fill=TEXT_HALO)
    occupied.append(padded)
    return True


def load_fonts(font_path: Path):
    return {
        "place": ImageFont.truetype(str(font_path), 23),
        "station": ImageFont.truetype(str(font_path), 18),
        "poi": ImageFont.truetype(str(font_path), 15),
        "road": ImageFont.truetype(str(font_path), 14),
        "credit": ImageFont.truetype(str(font_path), 10),
    }


def render_tile(tile_x: int, tile_y: int, zoom: int, tile_size: int, output: Path,
                polygons, buildings, waterways, railways, roads, labels,
                boundary_line: LineFeature, fonts) -> None:
    image = Image.new("RGB", (tile_size, tile_size), BACKGROUND)
    draw = ImageDraw.Draw(image)

    for feature in polygons.get((tile_x, tile_y), ()):
        draw_polygon(draw, feature, tile_x, tile_y, zoom, tile_size)
    for feature in buildings.get((tile_x, tile_y), ()):
        draw_polygon(draw, feature, tile_x, tile_y, zoom, tile_size)
    for feature in waterways.get((tile_x, tile_y), ()):
        draw_line_pass(draw, feature, tile_x, tile_y, zoom, tile_size, False)
    for feature in railways.get((tile_x, tile_y), ()):
        draw_line_pass(draw, feature, tile_x, tile_y, zoom, tile_size, False)

    tile_roads = sorted(roads.get((tile_x, tile_y), ()), key=lambda item: item.rank)
    for feature in tile_roads:
        draw_line_pass(draw, feature, tile_x, tile_y, zoom, tile_size, True)
    for feature in tile_roads:
        draw_line_pass(draw, feature, tile_x, tile_y, zoom, tile_size, False)

    draw_line_pass(draw, boundary_line, tile_x, tile_y, zoom, tile_size, False)

    occupied = []
    tile_labels = sorted(labels.get((tile_x, tile_y), ()), key=lambda item: -item.priority)
    for feature in tile_labels:
        px, py = to_pixel(feature.lon, feature.lat, tile_x, tile_y, zoom, tile_size)
        if feature.kind in {"national_capital", "city", "town", "suburb"}:
            font = fonts["place"]
        elif feature.kind in {"railway_station", "railway_halt"}:
            font = fonts["station"]
        else:
            font = fonts["poi"]
        draw_centered_label(draw, (px, py), feature.name, font, occupied,
                            fill=TEXT if feature.priority >= 70 else POI_TEXT)

    used_road_names = set()
    for feature in sorted(tile_roads, key=lambda item: -item.rank):
        if feature.rank < 5 or not feature.name or feature.name in used_road_names:
            continue
        midpoint = longest_part_midpoint(feature, tile_x, tile_y, zoom, tile_size)
        if midpoint and draw_centered_label(draw, midpoint, feature.name,
                                            fonts["road"], occupied):
            used_road_names.add(feature.name)

    # Compact per-tile attribution.  The consuming UI should also expose the
    # full ODbL notice from ATTRIBUTION.txt.
    draw.text((tile_size - 4, tile_size - 3), "© OSM", font=fonts["credit"],
              anchor="rs", fill="#a8b4bd", stroke_width=1, stroke_fill=TEXT_HALO)

    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output, format="PNG", compress_level=6)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path, required=True,
                        help="Directory containing Geofabrik free shapefiles")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--zoom", type=int, default=DEFAULT_ZOOM)
    parser.add_argument("--tile-size", type=int, default=DEFAULT_TILE_SIZE)
    parser.add_argument("--font", type=Path,
                        default=Path(r"C:\Windows\Fonts\NotoSansTC-VF.ttf"))
    args = parser.parse_args()

    if args.tile_size != DEFAULT_TILE_SIZE:
        raise SystemExit("This project requires --tile-size 468")
    if not args.font.exists():
        raise SystemExit(f"Traditional Chinese font not found: {args.font}")

    boundary = load_taipei_boundary(args.source_dir)
    min_lon, min_lat, max_lon, max_lat = boundary.bounds
    city_bbox = (min_lon, min_lat, max_lon, max_lat)
    min_x = math.floor(lon_to_tile_x(min_lon, args.zoom))
    max_x = math.floor(lon_to_tile_x(max_lon, args.zoom))
    min_y = math.floor(lat_to_tile_y(max_lat, args.zoom))
    max_y = math.floor(lat_to_tile_y(min_lat, args.zoom))
    grid = (min_x, max_x, min_y, max_y)
    print(f"Taipei bbox: {city_bbox}", flush=True)
    print(f"grid z{args.zoom}: x={min_x}..{max_x}, y={min_y}..{max_y}", flush=True)

    landuse = load_polygon_layer(
        args.source_dir, "landuse_a", city_bbox, args.zoom, grid,
        lambda d: landuse_style(str(d.get("fclass") or "")),
    )
    protected = load_polygon_layer(
        args.source_dir, "protected_areas_a", city_bbox, args.zoom, grid,
        lambda _d: ("#28473d", None),
    )
    water = load_polygon_layer(
        args.source_dir, "water_a", city_bbox, args.zoom, grid,
        lambda _d: (WATER_FILL, None),
    )
    buildings = load_polygon_layer(
        args.source_dir, "buildings_a", city_bbox, args.zoom, grid,
        lambda _d: (BUILDING_FILL, BUILDING_OUTLINE),
    )
    polygons = merge_bins(landuse, protected, water)

    waterways = load_line_layer(
        args.source_dir, "waterways", city_bbox, args.zoom, grid,
        lambda d: (WATERWAY, 3 if d.get("fclass") in {"river", "canal"} else 1,
                   0, None, 0),
    )
    railways = load_line_layer(
        args.source_dir, "railways", city_bbox, args.zoom, grid,
        lambda d: (RAILWAY, 3 if d.get("fclass") == "rail" else 2,
                   0, None, 0),
    )
    roads = load_line_layer(
        args.source_dir, "roads", city_bbox, args.zoom, grid,
        lambda d: road_style(str(d.get("fclass") or "")),
    )

    places = load_point_labels(
        args.source_dir, "places", city_bbox, args.zoom, grid,
        {"national_capital": 100, "city": 95, "town": 90, "suburb": 80,
         "village": 65, "locality": 55},
    )
    transport = load_point_labels(
        args.source_dir, "transport", city_bbox, args.zoom, grid,
        {"railway_station": 92, "railway_halt": 82, "bus_station": 68},
    )
    pois = load_point_labels(
        args.source_dir, "pois", city_bbox, args.zoom, grid,
        {"attraction": 78, "museum": 78, "university": 75, "hospital": 74,
         "arts_centre": 70, "stadium": 70, "viewpoint": 62},
    )
    labels = merge_bins(places, transport, pois)

    boundary_points: list[tuple[float, float]] = []
    boundary_parts: list[int] = []
    geometries = boundary.geoms if boundary.geom_type == "MultiPolygon" else (boundary,)
    for polygon in geometries:
        boundary_parts.append(len(boundary_points))
        boundary_points.extend((float(x), float(y)) for x, y in polygon.exterior.coords)
    boundary_line = LineFeature(tuple(boundary_points), tuple(boundary_parts),
                                CITY_BOUNDARY, 2, 10)

    fonts = load_fonts(args.font)
    total = (max_x - min_x + 1) * (max_y - min_y + 1)
    rendered = 0
    city_tiles = 0
    manifest_tiles = []
    for x in range(min_x, max_x + 1):
        for y in range(min_y, max_y + 1):
            west, south, east, north = tile_bounds(x, y, args.zoom)
            intersects_city = boundary.intersects(box(west, south, east, north))
            city_tiles += int(intersects_city)
            output = args.output_dir / f"z{args.zoom}" / str(x) / f"{y}.png"
            render_tile(x, y, args.zoom, args.tile_size, output, polygons,
                        buildings, waterways, railways, roads, labels,
                        boundary_line, fonts)
            manifest_tiles.append({
                "x": x,
                "y": y,
                "path": output.relative_to(args.output_dir).as_posix(),
                "intersects_taipei_city": intersects_city,
                "bounds_wgs84": [west, south, east, north],
            })
            rendered += 1
            if rendered % 25 == 0 or rendered == total:
                print(f"rendered {rendered}/{total}", flush=True)

    manifest = {
        "schema": 1,
        "name": "Taipei City offline navigation tiles",
        "source": "OpenStreetMap data distributed by Geofabrik",
        "source_url": "https://download.geofabrik.de/asia/taiwan.html",
        "license": "ODbL 1.0",
        "attribution": "© OpenStreetMap contributors",
        "tile_size": args.tile_size,
        "format": "PNG RGB",
        "projection": "EPSG:3857 / Web Mercator",
        "zoom": args.zoom,
        "grid": {"min_x": min_x, "max_x": max_x,
                 "min_y": min_y, "max_y": max_y},
        "taipei_boundary_osm_id": TAIPEI_OSM_ID,
        "taipei_bounds_wgs84": list(city_bbox),
        "tile_count": rendered,
        "tiles_intersecting_city": city_tiles,
        "tiles": manifest_tiles,
    }
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (args.output_dir / "ATTRIBUTION.txt").write_text(
        "Map data © OpenStreetMap contributors\n"
        "OpenStreetMap data is available under the Open Database License (ODbL).\n"
        "https://www.openstreetmap.org/copyright\n",
        encoding="utf-8",
    )
    print(f"done: {rendered} tiles ({city_tiles} intersect Taipei City)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
