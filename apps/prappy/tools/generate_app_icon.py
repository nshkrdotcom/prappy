#!/usr/bin/env python3
import math
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
SVG_OUT = ASSETS / "prappy_icon.svg"
ICO_OUT = ASSETS / "prappy_icon.ico"
BMP_OUT = ASSETS / "prappy_icon.bmp"

ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)


def clamp(value, low=0, high=255):
    return max(low, min(high, int(round(value))))


def blend_pixel(pixels, width, height, x, y, color):
    if x < 0 or x >= width or y < 0 or y >= height:
        return

    r, g, b, a = color
    if a <= 0:
        return

    index = (y * width + x) * 4
    dst_r, dst_g, dst_b, dst_a = pixels[index:index + 4]
    src_a = a / 255.0
    out_a = src_a + (dst_a / 255.0) * (1.0 - src_a)
    if out_a <= 0.0:
        pixels[index:index + 4] = [0, 0, 0, 0]
        return

    pixels[index] = clamp((r * src_a + dst_r * (dst_a / 255.0) * (1.0 - src_a)) / out_a)
    pixels[index + 1] = clamp((g * src_a + dst_g * (dst_a / 255.0) * (1.0 - src_a)) / out_a)
    pixels[index + 2] = clamp((b * src_a + dst_b * (dst_a / 255.0) * (1.0 - src_a)) / out_a)
    pixels[index + 3] = clamp(out_a * 255.0)


def inside_rounded_rect(x, y, width, height, radius):
    px = min(max(x, radius), width - radius)
    py = min(max(y, radius), height - radius)
    return (x - px) ** 2 + (y - py) ** 2 <= radius ** 2


def fill_background(pixels, width, height):
    radius = width * 0.19
    center_x = width * 0.52
    center_y = height * 0.43
    max_distance = math.hypot(width * 0.55, height * 0.62)

    for y in range(height):
        for x in range(width):
            if not inside_rounded_rect(x + 0.5, y + 0.5, width, height, radius):
                continue

            radial = min(1.0, math.hypot(x - center_x, y - center_y) / max_distance)
            top = y / max(height - 1, 1)
            teal = max(0.0, 1.0 - math.hypot(x - width * 0.66, y - height * 0.18) / (width * 0.68))
            green = max(0.0, 1.0 - math.hypot(x - width * 0.38, y - height * 0.56) / (width * 0.58))

            r = 7 + (1.0 - radial) * 10 + teal * 3 + green * 4
            g = 14 + (1.0 - radial) * 17 + teal * 14 + green * 18
            b = 25 + (1.0 - radial) * 24 + teal * 28 + top * 8
            blend_pixel(pixels, width, height, x, y, (r, g, b, 255))


def draw_disc(pixels, width, height, cx, cy, radius, color):
    x0 = int(math.floor(cx - radius))
    x1 = int(math.ceil(cx + radius))
    y0 = int(math.floor(cy - radius))
    y1 = int(math.ceil(cy + radius))
    r2 = radius * radius
    soft = max(1.0, radius * 0.12)

    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            distance = math.hypot((x + 0.5) - cx, (y + 0.5) - cy)
            if distance <= radius:
                alpha = color[3]
                if distance > radius - soft:
                    alpha *= (radius - distance) / soft
                blend_pixel(pixels, width, height, x, y, (*color[:3], alpha))


def draw_polyline(pixels, width, height, points, radius, color):
    scaled = points
    for start, end in zip(scaled, scaled[1:]):
        ax, ay = start
        bx, by = end
        length = max(1.0, math.hypot(bx - ax, by - ay))
        steps = max(2, int(length / max(radius * 0.35, 1.0)))
        for i in range(steps + 1):
            t = i / steps
            draw_disc(
                pixels,
                width,
                height,
                ax + (bx - ax) * t,
                ay + (by - ay) * t,
                radius,
                color,
            )


def cubic(a, b, c, d, segments=72):
    points = []
    for i in range(segments + 1):
        t = i / segments
        u = 1.0 - t
        x = u ** 3 * a[0] + 3 * u * u * t * b[0] + 3 * u * t * t * c[0] + t ** 3 * d[0]
        y = u ** 3 * a[1] + 3 * u * u * t * b[1] + 3 * u * t * t * c[1] + t ** 3 * d[1]
        points.append((x, y))
    return points


def point_in_polygon(x, y, points):
    inside = False
    j = len(points) - 1
    for i, point in enumerate(points):
        xi, yi = point
        xj, yj = points[j]
        if (yi > y) != (yj > y):
            at_x = (xj - xi) * (y - yi) / ((yj - yi) or 1.0e-9) + xi
            if x < at_x:
                inside = not inside
        j = i
    return inside


def fill_polygon(pixels, width, height, points, color):
    min_x = max(0, int(min(point[0] for point in points)))
    max_x = min(width - 1, int(max(point[0] for point in points)) + 1)
    min_y = max(0, int(min(point[1] for point in points)))
    max_y = min(height - 1, int(max(point[1] for point in points)) + 1)

    for y in range(min_y, max_y + 1):
        for x in range(min_x, max_x + 1):
            if point_in_polygon(x + 0.5, y + 0.5, points):
                blend_pixel(pixels, width, height, x, y, color)


def scale_points(points, scale):
    return [(x * scale, y * scale) for x, y in points]


def draw_icon(size):
    supersample = 4
    width = size * supersample
    height = size * supersample
    scale = width / 256.0
    pixels = [0] * (width * height * 4)

    fill_background(pixels, width, height)

    border_color = (96, 218, 236, 72)
    border_radius = 18 * scale
    for inset in (2.4, 3.4):
        draw_polyline(
            pixels,
            width,
            height,
            scale_points(
                [
                    (48 + inset, 23 + inset),
                    (207 - inset, 23 + inset),
                    (231 - inset, 48 + inset),
                    (231 - inset, 207 - inset),
                    (207 - inset, 231 - inset),
                    (48 + inset, 231 - inset),
                    (23 + inset, 207 - inset),
                    (23 + inset, 48 + inset),
                    (48 + inset, 23 + inset),
                ],
                scale,
            ),
            border_radius * 0.07,
            border_color,
        )

    terrain = scale_points(
        [(88, 138), (108, 100), (143, 88), (174, 116), (151, 148), (111, 153)],
        scale,
    )
    fill_polygon(pixels, width, height, terrain, (47, 124, 58, 245))
    fill_polygon(pixels, width, height, scale_points([(88, 138), (108, 100), (122, 151), (111, 153)], scale), (100, 161, 71, 220))
    fill_polygon(pixels, width, height, scale_points([(108, 100), (143, 88), (132, 132), (122, 151)], scale), (61, 151, 72, 230))
    fill_polygon(pixels, width, height, scale_points([(143, 88), (174, 116), (151, 148), (132, 132)], scale), (197, 172, 91, 230))

    topo_lines = [
        [(103, 136), (120, 128), (141, 126), (159, 133)],
        [(111, 119), (129, 111), (149, 113), (164, 122)],
        [(120, 103), (139, 100), (155, 107)],
    ]
    for line in topo_lines:
        draw_polyline(pixels, width, height, scale_points(line, scale), 1.25 * scale, (226, 240, 164, 120))

    stem = scale_points([(72, 192), (72, 60)], scale)
    loop = (
        scale_points([(72, 60)], scale)
        + scale_points(cubic((72, 60), (128, 42), (190, 63), (190, 112))[1:], scale)
        + scale_points(cubic((190, 112), (188, 155), (122, 166), (72, 143))[1:], scale)
    )
    p_path = stem + loop

    draw_polyline(pixels, width, height, p_path, 15.5 * scale, (24, 211, 238, 52))
    draw_polyline(pixels, width, height, p_path, 10.0 * scale, (38, 198, 224, 245))
    draw_polyline(pixels, width, height, p_path, 4.2 * scale, (218, 252, 255, 150))

    route = scale_points([(91, 154), (115, 144), (147, 139), (178, 124)], scale)
    draw_polyline(pixels, width, height, route, 2.0 * scale, (250, 209, 95, 210))

    draw_disc(pixels, width, height, 190 * scale, 112 * scale, 15 * scale, (245, 43, 205, 70))
    draw_disc(pixels, width, height, 190 * scale, 112 * scale, 6.5 * scale, (255, 73, 216, 255))
    draw_disc(pixels, width, height, 190 * scale, 112 * scale, 2.2 * scale, (255, 250, 255, 255))

    return downsample(pixels, width, height, supersample)


def downsample(pixels, width, height, factor):
    out_width = width // factor
    out_height = height // factor
    output = bytearray(out_width * out_height * 4)
    sample_count = factor * factor

    for y in range(out_height):
        for x in range(out_width):
            total = [0, 0, 0, 0]
            for yy in range(factor):
                for xx in range(factor):
                    index = ((y * factor + yy) * width + (x * factor + xx)) * 4
                    total[0] += pixels[index]
                    total[1] += pixels[index + 1]
                    total[2] += pixels[index + 2]
                    total[3] += pixels[index + 3]
            out = (y * out_width + x) * 4
            output[out:out + 4] = bytes(clamp(channel / sample_count) for channel in total)

    return output


def write_bmp(path, pixels, size):
    row_bytes = size * 4
    pixel_bytes = row_bytes * size
    header_size = 14 + 40
    with path.open("wb") as file:
        file.write(b"BM")
        file.write(struct.pack("<IHHI", header_size + pixel_bytes, 0, 0, header_size))
        file.write(struct.pack("<IIIHHIIIIII", 40, size, size, 1, 32, 0, pixel_bytes, 2835, 2835, 0, 0))
        for y in range(size - 1, -1, -1):
            row = bytearray()
            for x in range(size):
                index = (y * size + x) * 4
                r, g, b, a = pixels[index:index + 4]
                row.extend((b, g, r, a))
            file.write(row)


def icon_image_bytes(pixels, size):
    header = struct.pack("<IIIHHIIIIII", 40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            index = (y * size + x) * 4
            r, g, b, a = pixels[index:index + 4]
            xor.extend((b, g, r, a))

    mask_stride = ((size + 31) // 32) * 4
    and_mask = b"\x00" * (mask_stride * size)
    return header + xor + and_mask


def write_ico(path, rendered):
    offset = 6 + len(rendered) * 16
    entries = []
    images = []
    for size, pixels in rendered:
        image = icon_image_bytes(pixels, size)
        width_byte = 0 if size == 256 else size
        entries.append(struct.pack("<BBBBHHII", width_byte, width_byte, 0, 0, 1, 32, len(image), offset))
        images.append(image)
        offset += len(image)

    with path.open("wb") as file:
        file.write(struct.pack("<HHH", 0, 1, len(rendered)))
        for entry in entries:
            file.write(entry)
        for image in images:
            file.write(image)


def write_svg(path):
    path.write_text(
        """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" role="img" aria-label="Prappy app icon">
  <defs>
    <radialGradient id="bg" cx="54%" cy="42%" r="76%">
      <stop offset="0%" stop-color="#182b35"/>
      <stop offset="58%" stop-color="#0b1723"/>
      <stop offset="100%" stop-color="#07101b"/>
    </radialGradient>
    <linearGradient id="terrain" x1="88" y1="95" x2="174" y2="150" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#75aa48"/>
      <stop offset="55%" stop-color="#258845"/>
      <stop offset="100%" stop-color="#d2b75e"/>
    </linearGradient>
    <filter id="glow" x="-40%" y="-40%" width="180%" height="180%">
      <feGaussianBlur stdDeviation="5" result="blur"/>
      <feMerge><feMergeNode in="blur"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
  </defs>
  <rect x="20" y="20" width="216" height="216" rx="42" fill="url(#bg)"/>
  <rect x="23" y="23" width="210" height="210" rx="38" fill="none" stroke="#60daec" stroke-opacity=".28" stroke-width="2"/>
  <path d="M88 138 L108 100 L143 88 L174 116 L151 148 L111 153 Z" fill="url(#terrain)" opacity=".95"/>
  <path d="M88 138 L108 100 L122 151 L111 153 Z" fill="#7aaa47" opacity=".72"/>
  <path d="M143 88 L174 116 L151 148 L132 132 Z" fill="#caae5b" opacity=".82"/>
  <path d="M103 136 C122 127 142 126 159 133 M111 119 C130 111 149 113 164 122 M120 103 C139 100 151 105 155 107" fill="none" stroke="#e6f0a4" stroke-opacity=".55" stroke-width="2"/>
  <path d="M72 192 L72 60 C128 42 190 63 190 112 C188 155 122 166 72 143" fill="none" stroke="#18d3ee" stroke-opacity=".25" stroke-width="31" stroke-linecap="round" stroke-linejoin="round" filter="url(#glow)"/>
  <path d="M72 192 L72 60 C128 42 190 63 190 112 C188 155 122 166 72 143" fill="none" stroke="#26c6e0" stroke-width="20" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="M72 192 L72 60 C128 42 190 63 190 112 C188 155 122 166 72 143" fill="none" stroke="#dafcff" stroke-opacity=".58" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="M91 154 C115 144 147 139 178 124" fill="none" stroke="#fad15f" stroke-width="4" stroke-linecap="round"/>
  <circle cx="190" cy="112" r="15" fill="#f52bcd" opacity=".28"/>
  <circle cx="190" cy="112" r="7" fill="#ff49d8"/>
  <circle cx="190" cy="112" r="2.4" fill="#fffaff"/>
</svg>
""",
        encoding="utf-8",
    )


def main():
    ASSETS.mkdir(parents=True, exist_ok=True)
    rendered = [(size, draw_icon(size)) for size in ICON_SIZES]
    write_svg(SVG_OUT)
    write_ico(ICO_OUT, rendered)
    size64, pixels64 = next(item for item in rendered if item[0] == 64)
    write_bmp(BMP_OUT, pixels64, size64)
    print(f"wrote {SVG_OUT}")
    print(f"wrote {ICO_OUT}")
    print(f"wrote {BMP_OUT}")


if __name__ == "__main__":
    main()
