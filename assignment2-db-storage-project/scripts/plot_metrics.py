#!/usr/bin/env python3
import csv
import math
import os
import subprocess
import struct
import sys
import tempfile
import uuid
import zlib
from collections import defaultdict
from pathlib import Path
from xml.sax.saxutils import escape


FONT = "Times New Roman"


def load_rows(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    for row in rows:
        for key in [
            "timestamp_sec", "ops", "total_ops", "tps",
            "logical_read_MBps", "logical_write_MBps",
            "avg_latency_us", "p50_latency_us", "p95_latency_us", "p99_latency_us",
            "total_pages", "memory_usage_MB",
        ]:
            row[key] = float(row[key]) if row.get(key) not in ("", None) else 0.0
    return rows


def fmt_num(value):
    value = float(value)
    abs_v = abs(value)
    if abs_v >= 1_000_000:
        return f"{value / 1_000_000:.1f}M"
    if abs_v >= 1_000:
        return f"{value / 1_000:.0f}K"
    if abs_v >= 10:
        return f"{value:.0f}"
    if abs_v >= 1:
        return f"{value:.1f}"
    return f"{value:.2f}"


def nice_max(values):
    max_v = max(values) if values else 1.0
    if max_v <= 0:
        return 1.0
    raw = max_v * 1.08
    exp = math.floor(math.log10(raw))
    frac = raw / (10 ** exp)
    if frac <= 1:
        nice = 1
    elif frac <= 2:
        nice = 2
    elif frac <= 5:
        nice = 5
    else:
        nice = 10
    return nice * (10 ** exp)


def polyline(points):
    return " ".join(f"{x:.2f},{y:.2f}" for x, y in points)


def svg_text(x, y, text, size=18, anchor="middle", rotate=None, color="#263244", weight="normal"):
    transform = f' transform="rotate({rotate} {x} {y})"' if rotate is not None else ""
    return (
        f'<text x="{x:.2f}" y="{y:.2f}" font-family="{FONT}" font-size="{size}" '
        f'font-weight="{weight}" fill="{color}" text-anchor="{anchor}"{transform}>'
        f'{escape(str(text))}</text>'
    )


def png_read_rgba(path):
    data = Path(path).read_bytes()
    pos = 8
    width = height = color_type = None
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        chunk_type = data[pos + 4 : pos + 8]
        chunk_data = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", chunk_data)
            if bit_depth != 8 or color_type not in (2, 6):
                raise RuntimeError(f"unsupported PNG format: bit_depth={bit_depth}, color_type={color_type}")
        elif chunk_type == b"IDAT":
            idat += chunk_data
        elif chunk_type == b"IEND":
            break
    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(idat)
    stride = 1 + width * channels
    rows = []
    prev = bytearray(width * channels)
    for y in range(height):
        filter_type = raw[y * stride]
        row = bytearray(raw[y * stride + 1 : (y + 1) * stride])
        if filter_type == 1:
            for i in range(len(row)):
                row[i] = (row[i] + (row[i - channels] if i >= channels else 0)) & 0xFF
        elif filter_type == 2:
            for i in range(len(row)):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif filter_type == 3:
            for i in range(len(row)):
                left = row[i - channels] if i >= channels else 0
                row[i] = (row[i] + ((left + prev[i]) // 2)) & 0xFF
        elif filter_type == 4:
            def paeth(a, b, c):
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                return a if pa <= pb and pa <= pc else b if pb <= pc else c

            for i in range(len(row)):
                a = row[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                row[i] = (row[i] + paeth(a, b, c)) & 0xFF
        elif filter_type != 0:
            raise RuntimeError(f"unsupported PNG filter: {filter_type}")
        rgba = bytearray(width * 4)
        if channels == 4:
            rgba[:] = row
        else:
            for x in range(width):
                rgba[4 * x : 4 * x + 4] = row[3 * x : 3 * x + 3] + b"\xff"
        rows.append(rgba)
        prev = row
    return width, height, rows


def png_write_rgb(path, width, height, rows):
    raw_rows = []
    for y in range(height):
        raw = bytearray()
        src = rows[y]
        for x in range(width):
            r, g, b, a = src[4 * x : 4 * x + 4]
            # Flatten transparent pixels onto white.
            if a < 255:
                r = (r * a + 255 * (255 - a)) // 255
                g = (g * a + 255 * (255 - a)) // 255
                b = (b * a + 255 * (255 - a)) // 255
            raw.extend((r, g, b))
        raw_rows.append(b"\x00" + bytes(raw))

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(raw_rows), 9))
    png += chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def crop_png_top_left(input_path, output_path, target_width, target_height):
    width, height, rows = png_read_rgba(input_path)
    if width < target_width or height < target_height:
        raise RuntimeError(f"cannot crop {width}x{height} to {target_width}x{target_height}")
    cropped = []
    for y in range(target_height):
        cropped.append(rows[y][: target_width * 4])
    png_write_rgb(output_path, target_width, target_height, cropped)


def render_svg_to_png(svg, output_path, width, height):
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmpdir:
        stem = f"plot_{uuid.uuid4().hex}"
        svg_path = Path(tmpdir) / f"{stem}.svg"
        svg_path.write_text(svg, encoding="utf-8")
        side = max(width, height)
        subprocess.run(
            ["qlmanage", "-t", "-s", str(side), "-o", tmpdir, str(svg_path)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        png_path = Path(tmpdir) / f"{stem}.svg.png"
        if not png_path.exists():
            raise RuntimeError("qlmanage did not produce PNG output")
        crop_png_top_left(png_path, output_path, width, height)


def axes_svg(width, height, left, top, right, bottom, x_label, y_label, x_ticks, y_ticks, x_to_px, y_to_px):
    parts = []
    axis = "#111827"
    grid = "#d8e0eb"
    text = "#263244"
    for value, label in y_ticks:
        y = y_to_px(value)
        parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{right}" y2="{y:.2f}" stroke="{grid}" stroke-width="1"/>')
        parts.append(f'<line x1="{left - 6}" y1="{y:.2f}" x2="{left}" y2="{y:.2f}" stroke="{axis}" stroke-width="1.2"/>')
        parts.append(svg_text(left - 12, y + 5, label, 18, "end", color=text))
    for value, label in x_ticks:
        x = x_to_px(value)
        parts.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{bottom}" stroke="{grid}" stroke-width="1"/>')
        parts.append(f'<line x1="{x:.2f}" y1="{bottom}" x2="{x:.2f}" y2="{bottom + 6}" stroke="{axis}" stroke-width="1.2"/>')
        parts.append(svg_text(x, bottom + 28, label, 18, "middle", color=text))

    # Axis lines with arrowheads drawn as filled polygons.
    parts.append(f'<line x1="{left}" y1="{bottom}" x2="{right + 22}" y2="{bottom}" stroke="{axis}" stroke-width="2"/>')
    parts.append(f'<polygon points="{right + 22},{bottom} {right + 8},{bottom - 7} {right + 8},{bottom + 7}" fill="{axis}"/>')
    parts.append(f'<line x1="{left}" y1="{bottom}" x2="{left}" y2="{top - 22}" stroke="{axis}" stroke-width="2"/>')
    parts.append(f'<polygon points="{left},{top - 22} {left - 7},{top - 8} {left + 7},{top - 8}" fill="{axis}"/>')
    parts.append(svg_text((left + right) / 2, height - 36, x_label, 20, "middle", color=text))
    parts.append(svg_text(28, (top + bottom) / 2, y_label, 20, "middle", rotate=-90, color=text))
    return "".join(parts)


def base_svg(width, height, body):
    side = max(width, height)
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{side}" height="{side}" '
        f'viewBox="0 0 {side} {side}">'
        f'<rect width="{side}" height="{side}" fill="white"/>'
        f'{body}</svg>'
    )


def line_chart(rows, phase, metric, y_label, output):
    data = [r for r in rows if r["phase"] == phase]
    if not data:
        return
    width, height = 1100, 700
    left, top, right, bottom = 135, 80, 1040, 600
    xs = [r["total_ops"] for r in data]
    ys = [r[metric] for r in data]
    max_x = max(xs) if max(xs) > 0 else 1.0
    max_y = nice_max(ys)

    def x_to_px(x):
        return left + (x / max_x) * (right - left)

    def y_to_px(y):
        return bottom - (y / max_y) * (bottom - top)

    x_ticks = [(max_x * i / 5, fmt_num(max_x * i / 5)) for i in range(6)]
    y_ticks = [(max_y * i / 5, fmt_num(max_y * i / 5)) for i in range(6)]
    points = [(x_to_px(x), y_to_px(y)) for x, y in zip(xs, ys)]
    blue = "#2563eb"
    body = [
        axes_svg(width, height, left, top, right, bottom, "TOTAL OPS", y_label, x_ticks, y_ticks, x_to_px, y_to_px),
        f'<polyline points="{polyline(points)}" fill="none" stroke="{blue}" stroke-width="2.2" stroke-linejoin="round" stroke-linecap="round"/>',
    ]
    for x, y in points:
        body.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="4" fill="{blue}"/>')
    render_svg_to_png(base_svg(width, height, "".join(body)), output, width, height)


def io_chart(rows, output):
    if not rows:
        return
    width, height = 1100, 700
    left, top, right, bottom = 135, 105, 1040, 600
    xs = list(range(1, len(rows) + 1))
    read = [r["logical_read_MBps"] for r in rows]
    write = [r["logical_write_MBps"] for r in rows]
    max_x = max(xs)
    max_y = nice_max(read + write)

    def x_to_px(x):
        return left + ((x - 1) / max(1, max_x - 1)) * (right - left)

    def y_to_px(y):
        return bottom - (y / max_y) * (bottom - top)

    x_ticks = [(1 + (max_x - 1) * i / 5, fmt_num(1 + (max_x - 1) * i / 5)) for i in range(6)]
    y_ticks = [(max_y * i / 5, fmt_num(max_y * i / 5)) for i in range(6)]
    blue = "#2563eb"
    red = "#dc2626"
    read_points = [(x_to_px(x), y_to_px(y)) for x, y in zip(xs, read)]
    write_points = [(x_to_px(x), y_to_px(y)) for x, y in zip(xs, write)]
    body = [
        axes_svg(width, height, left, top, right, bottom, "SAMPLE INDEX", "MB/S", x_ticks, y_ticks, x_to_px, y_to_px),
        # Legend is placed in a blank region between the read and write curves.
        svg_text(600, 232, "READ", 20, "start", color=blue),
        f'<line x1="665" y1="226" x2="725" y2="226" stroke="{blue}" stroke-width="2.2"/>',
        svg_text(600, 258, "WRITE", 20, "start", color=red),
        f'<line x1="675" y1="252" x2="735" y2="252" stroke="{red}" stroke-width="2.2"/>',
        f'<polyline points="{polyline(read_points)}" fill="none" stroke="{blue}" stroke-width="2.0" stroke-linejoin="round" stroke-linecap="round"/>',
        f'<polyline points="{polyline(write_points)}" fill="none" stroke="{red}" stroke-width="2.0" stroke-linejoin="round" stroke-linecap="round"/>',
    ]
    for x, y in read_points:
        body.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="3.2" fill="{blue}"/>')
    for x, y in write_points:
        body.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="3.2" fill="{red}"/>')
    render_svg_to_png(base_svg(width, height, "".join(body)), output, width, height)


def latency_chart(rows, output):
    query_rows = [r for r in rows if r["phase"] == "query" and r["ops"] > 0]
    if not query_rows:
        return
    total_ops = sum(r["ops"] for r in query_rows)

    def weighted(metric):
        return sum(r[metric] * r["ops"] for r in query_rows) / total_ops if total_ops else 0.0

    labels = ["AVG", "P50", "P95", "P99"]
    values = [weighted(m) for m in ["avg_latency_us", "p50_latency_us", "p95_latency_us", "p99_latency_us"]]
    width, height = 1000, 700
    left, top, right, bottom = 135, 80, 940, 600
    max_y = nice_max(values)

    def x_to_px(i):
        return left + ((i + 0.5) / len(values)) * (right - left)

    def y_to_px(y):
        return bottom - (y / max_y) * (bottom - top)

    x_ticks = [(i, labels[i]) for i in range(len(labels))]
    y_ticks = [(max_y * i / 5, fmt_num(max_y * i / 5)) for i in range(6)]
    colors = ["#2563eb", "#16a34a", "#d97706", "#dc2626"]
    bar_w = (right - left) / len(values) * 0.38
    body = [axes_svg(width, height, left, top, right, bottom, "LATENCY STAT", "US", x_ticks, y_ticks, x_to_px, y_to_px)]
    for i, value in enumerate(values):
        x = x_to_px(i)
        y = y_to_px(value)
        body.append(f'<rect x="{x - bar_w / 2:.2f}" y="{y:.2f}" width="{bar_w:.2f}" height="{bottom - y:.2f}" fill="{colors[i]}"/>')
        body.append(svg_text(x, y - 12, fmt_num(value), 18, "middle"))
    render_svg_to_png(base_svg(width, height, "".join(body)), output, width, height)


def generate_plots(rows, out_dir):
    line_chart(rows, "insert", "tps", "RECORDS/S", os.path.join(out_dir, "insert_tps.png"))
    line_chart(rows, "delete", "tps", "RECORDS/S", os.path.join(out_dir, "delete_tps.png"))
    line_chart(rows, "update", "tps", "RECORDS/S", os.path.join(out_dir, "update_tps.png"))
    line_chart(rows, "query", "tps", "QUERIES/S", os.path.join(out_dir, "query_qps.png"))
    io_chart(rows, os.path.join(out_dir, "io_throughput.png"))
    latency_chart(rows, os.path.join(out_dir, "query_latency.png"))


def main():
    if len(sys.argv) != 3:
        print("usage: plot_metrics.py benchmark_result.csv results/figures", file=sys.stderr)
        return 1
    csv_path, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    rows = load_rows(csv_path)
    generate_plots(rows, out_dir)
    phases = defaultdict(int)
    for row in rows:
        phases[row["phase"]] += int(row["ops"])
    print("generated figures in", out_dir, "from phases:", dict(phases))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
