"""Generates minflyout.ico (variant A) with its own geometry per size.

Small sizes are simplified and snapped to whole target pixels so that the edges
stay crisp after downscaling.
"""
import struct
from io import BytesIO

from PIL import Image, ImageDraw

SS = 8                       # supersampling
BG_TOP = (59, 130, 246)      # #3B82F6
BG_BOTTOM = (29, 78, 216)    # #1D4ED8
WHITE = (255, 255, 255)
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def tile(s, radius):
    """Rounded tile with a vertical gradient."""
    grad = Image.new("RGB", (1, s))
    for y in range(s):
        t = y / max(1, s - 1)
        grad.putpixel((0, y), tuple(
            int(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t) for i in range(3)))
    grad = grad.resize((s, s))

    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, s - 1, s - 1], radius=int(radius * s), fill=255)
    out = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    return out


def make(size):
    """Draws the icon for a target size."""
    s = size * SS

    def snap(v):
        """Snaps a relative coordinate to whole target pixels."""
        return round(v * s / SS) * SS

    def rr(d, x0, y0, x1, y1, fill, radius=0.0, hard=False):
        box = ([snap(x0), snap(y0), snap(x1), snap(y1)] if hard else
               [x0 * s, y0 * s, x1 * s, y1 * s])
        d.rounded_rectangle(box, radius=radius * s, fill=fill)

    if size <= 20:        # very small: thick stroke, two rows, more angular
        img = tile(s, 0.18)
        d = ImageDraw.Draw(img)
        rr(d, 0.25, 0.19, 0.75, 0.31, WHITE, 0.05, hard=True)
        rr(d, 0.13, 0.44, 0.87, 0.87, WHITE, 0.10, hard=True)
        rr(d, 0.25, 0.56, 0.75, 0.66, BG_BOTTOM, 0.0, hard=True)
        rr(d, 0.25, 0.72, 0.60, 0.82, BG_BOTTOM, 0.0, hard=True)
    elif size <= 32:      # small: two rows, but already rounded
        img = tile(s, 0.20)
        d = ImageDraw.Draw(img)
        rr(d, 0.27, 0.20, 0.73, 0.30, WHITE, 0.04, hard=True)
        rr(d, 0.15, 0.43, 0.85, 0.85, WHITE, 0.09, hard=True)
        rr(d, 0.25, 0.54, 0.75, 0.63, BG_BOTTOM, 0.02, hard=True)
        rr(d, 0.25, 0.70, 0.61, 0.79, BG_BOTTOM, 0.02, hard=True)
    else:                 # large: full drawing with three menu rows
        img = tile(s, 0.22)
        d = ImageDraw.Draw(img)
        rr(d, 0.30, 0.235, 0.70, 0.295, WHITE, 0.03)
        rr(d, 0.17, 0.42, 0.83, 0.83, WHITE, 0.07)
        rr(d, 0.25, 0.505, 0.75, 0.560, BG_BOTTOM, 0.027)
        rr(d, 0.25, 0.620, 0.66, 0.675, BG_BOTTOM, 0.027)
        rr(d, 0.25, 0.735, 0.71, 0.790, BG_BOTTOM, 0.027)

    return img.resize((size, size), Image.LANCZOS)


def bmp_entry(img):
    """Packs an image as a BMP entry (BITMAPINFOHEADER + BGRA + AND mask)."""
    w, h = img.size
    px = img.load()

    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)

    body = bytearray()
    for y in range(h - 1, -1, -1):          # BMP is stored bottom-up
        for x in range(w):
            r, g, b, a = px[x, y]
            body += bytes((b, g, r, a))

    # AND mask: effectively unused thanks to the alpha channel, but required.
    row = ((w + 31) // 32) * 4
    mask = bytearray(row * h)
    return header + bytes(body) + bytes(mask)


def png_entry(img):
    """Packs an image as a PNG entry (customary for 256 px)."""
    buf = BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def write_ico(path, images):
    """Writes an ICO file with one entry per image."""
    entries = [(im, png_entry(im) if im.width >= 256 else bmp_entry(im))
               for im in images]

    out = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for im, data in entries:
        out += struct.pack("<BBBBHHII",
                           im.width % 256, im.height % 256, 0, 0, 1, 32,
                           len(data), offset)
        offset += len(data)
    for _, data in entries:
        out += data

    with open(path, "wb") as f:
        f.write(bytes(out))


if __name__ == "__main__":
    import os
    os.makedirs("out", exist_ok=True)

    images = [make(s) for s in SIZES]
    write_ico("out/minflyout.ico", images)
    images[-1].save("out/minflyout-256.png")

    # preview: all sizes side by side, light and dark
    cell = 140
    sheet = Image.new("RGB", (len(SIZES) * cell, 2 * cell + 24), (250, 250, 250))
    d = ImageDraw.Draw(sheet)
    for col, (s, im) in enumerate(zip(SIZES, images)):
        d.text((col * cell + 8, 6), f"{s} px", fill=(60, 60, 60))
        for row, bg in enumerate([(243, 243, 243), (32, 32, 32)]):
            y = 24 + row * cell
            d.rectangle([col * cell, y, col * cell + cell - 2, y + cell - 2], fill=bg)
            view = im if s <= 128 else im.resize((128, 128), Image.LANCZOS)
            sheet.paste(view, (col * cell + (cell - view.width) // 2,
                               y + (cell - view.height) // 2), view)
    sheet.save("out/preview.png")

    print("written:", os.path.getsize("out/minflyout.ico"), "bytes")
