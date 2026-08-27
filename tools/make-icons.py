"""Draws the XCam mark and writes every icon the project needs.

The geometry lives in three places -- brand/xcam-mark.svg, the Android vector
drawables, and here -- because each of the three consumers wants a different
format and none of them can read the others. They are kept in step by all three
copying the numbers in GEOMETRY below, which is the only reason those numbers
are written out so explicitly rather than being derived.

Everything is drawn at 8x and downsampled. Round caps on a 45-degree bar are
exactly the shape that falls apart when rasterised at 16 pixels, and 16 pixels
is the size the mark was designed for.

    py tools/make-icons.py
"""

import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("This needs Pillow:  py -m pip install pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---- the mark ---------------------------------------------------------------

GRID = 1024
BAR_WIDTH = 146
OUTER_LOW = 176
OUTER_HIGH = 848
CENTRE_GAP = 120          # how far each arm stops short of the middle

INK = (14, 17, 20, 255)          # #0E1114
TEXT = (242, 245, 247, 255)      # #F2F5F7
SIGNAL = (255, 176, 32, 255)     # #FFB020

# The arms, outer point to inner point. The last one is the tally.
_OFF = CENTRE_GAP / (2 ** 0.5)
_MID = GRID / 2
ARMS = [
    ((OUTER_LOW, OUTER_LOW), (_MID - _OFF, _MID - _OFF), TEXT),      # top left
    ((OUTER_LOW, OUTER_HIGH), (_MID - _OFF, _MID + _OFF), TEXT),     # bottom left
    ((OUTER_HIGH, OUTER_HIGH), (_MID + _OFF, _MID + _OFF), TEXT),    # bottom right
    ((OUTER_HIGH, OUTER_LOW), (_MID + _OFF, _MID - _OFF), SIGNAL),   # top right
]


def draw_mark(size, tile=True, supersample=8):
    """The mark at `size` pixels, on an Ink tile unless `tile` is False."""
    hi = size * supersample
    scale = hi / GRID

    image = Image.new("RGBA", (hi, hi), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    if tile:
        # A rounded square rather than a circle: this is a device, and every
        # platform that shows it will apply its own mask on top anyway.
        radius = 0.22 * hi
        draw.rounded_rectangle([0, 0, hi - 1, hi - 1], radius=radius, fill=INK)

    width = BAR_WIDTH * scale
    for (x0, y0), (x1, y1), colour in ARMS:
        a = (x0 * scale, y0 * scale)
        b = (x1 * scale, y1 * scale)
        draw.line([a, b], fill=colour, width=int(round(width)))
        # PIL has no line caps, so the round ends are drawn as discs. Without
        # them the arms end in a flat 45-degree chisel and the mark reads as
        # four wedges rather than one shape.
        for cx, cy in (a, b):
            draw.ellipse([cx - width / 2, cy - width / 2,
                          cx + width / 2, cy + width / 2], fill=colour)

    return image.resize((size, size), Image.LANCZOS)


# ---- outputs ----------------------------------------------------------------

def write(path, image):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image.save(path)
    print("  %-58s %5d bytes" % (os.path.relpath(path, ROOT), os.path.getsize(path)))


def main():
    print("XCam icons")

    # Windows. One .ico carrying every size Explorer, the taskbar and Alt-Tab
    # ask for; Windows picks rather than scaling, which is the whole reason to
    # render each one separately instead of shipping a single 256.
    ico_sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [draw_mark(s) for s in ico_sizes]
    ico_path = os.path.join(ROOT, "windows", "app", "xcam.ico")
    os.makedirs(os.path.dirname(ico_path), exist_ok=True)
    frames[-1].save(ico_path, format="ICO",
                    sizes=[(s, s) for s in ico_sizes],
                    append_images=frames[:-1])
    print("  %-58s %5d bytes" % (os.path.relpath(ico_path, ROOT),
                                 os.path.getsize(ico_path)))

    # Android. minSdk 29 means the adaptive icon in mipmap-anydpi-v26 always
    # wins, so these are only here to keep aapt from complaining about a
    # missing default -- and to be what shows up in anything that ignores
    # adaptive icons entirely.
    for folder, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96),
                       ("xxhdpi", 144), ("xxxhdpi", 192)):
        base = os.path.join(ROOT, "android", "app", "src", "main", "res",
                            "mipmap-" + folder)
        write(os.path.join(base, "ic_launcher.png"), draw_mark(px))
        write(os.path.join(base, "ic_launcher_round.png"), draw_mark(px))

    # For anything outside the build: a slide, a readme, a store listing.
    for px in (512, 1024):
        write(os.path.join(ROOT, "brand", "xcam-mark-%d.png" % px), draw_mark(px))
    write(os.path.join(ROOT, "brand", "xcam-mark-1024-bare.png"),
          draw_mark(1024, tile=False))

    print("\nCheck the 16px frame before shipping: it is the size the geometry\n"
          "was chosen for, and the only one where the amber arm can be lost.")


if __name__ == "__main__":
    main()
