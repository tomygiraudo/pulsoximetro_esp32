"""Generates the PWA/app icons for PulsOx from plain shapes (no external
assets). Run once with `python generate_icons.py`; outputs live alongside
this script and are committed as static PNGs, so the script itself is a
build tool, not a runtime dependency."""

from PIL import Image, ImageDraw

TEAL = (8, 145, 178, 255)      # --color-primary
WHITE = (255, 255, 255, 255)
GREEN_DOT = (22, 163, 74, 255)  # --color-accent


def pulse_points(w, h):
    """A simple heartbeat/PPG waveform, scaled to the icon box."""
    midy = h * 0.56
    pts = [
        (w * 0.12, midy),
        (w * 0.30, midy),
        (w * 0.38, midy - h * 0.10),
        (w * 0.44, midy + h * 0.22),
        (w * 0.50, midy - h * 0.34),
        (w * 0.56, midy + h * 0.10),
        (w * 0.62, midy),
        (w * 0.88, midy),
    ]
    return pts


def draw_icon(size, radius_ratio=0.22, padding_ratio=0.0):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    r = int(size * radius_ratio)
    draw.rounded_rectangle([0, 0, size - 1, size - 1], radius=r, fill=TEAL)

    stroke_w = max(2, round(size * 0.045))
    pts = pulse_points(size, size)
    draw.line(pts, fill=WHITE, width=stroke_w, joint="curve")

    # rounded caps at each vertex so the line reads as one continuous stroke
    cap_r = stroke_w / 2
    for x, y in pts:
        draw.ellipse([x - cap_r, y - cap_r, x + cap_r, y + cap_r], fill=WHITE)

    # small accent dot marking the "live" pulse peak
    peak_x, peak_y = pts[4]
    dot_r = size * 0.045
    draw.ellipse(
        [peak_x - dot_r, peak_y - dot_r, peak_x + dot_r, peak_y + dot_r],
        fill=GREEN_DOT,
        outline=WHITE,
        width=max(1, round(size * 0.012)),
    )

    return img


def draw_maskable(size):
    """Maskable icons need extra safe-area padding (~10%) since OS shells
    crop to a circle/squircle without warning."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, 0, size, size], fill=TEAL)

    inner = draw_icon(int(size * 0.7), radius_ratio=0)
    pad = (size - inner.width) // 2
    img.paste(inner, (pad, pad), inner)
    return img


sizes = [16, 32, 180, 192, 512]
for s in sizes:
    draw_icon(s).save(f"icon-{s}.png")

draw_maskable(512).save("icon-512-maskable.png")

print("done:", ", ".join(f"icon-{s}.png" for s in sizes) + ", icon-512-maskable.png")
