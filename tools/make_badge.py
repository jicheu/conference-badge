#!/usr/bin/env python3
"""Render conference-badge pages and emit a C header for the Zephyr firmware.

Each page is drawn at the panel's native size (800x480 for the Seeed 4.26" SSD1677 panel)
and packed the way the firmware's ssd1677 driver expects it:

    pixel format  MONO01      (bit 1 = white, bit 0 = black)
    layout        row by row  (width / 8 bytes per row)
    bit order     MSB first   (bit 7 = leftmost pixel of the byte)
    index         y * (width / 8) + x / 8

Usage:
    python3 tools/make_badge.py badges.json -o src/badge_images.h --preview preview

    -o FILE       C header to write (default src/badge_images.h); the firmware includes it
    --preview DIR also write one PNG per page (page0.png, page1.png, ...), at the panel's native size (800x480),
                  so you can check the layout and scan the QR code without a board
    --font FILE   use this TrueType font for all text

Input JSON (see badges.example.json):

    {
      "width": 800, "height": 480,          // optional; panel size (2.13": 250 x 120)
      "pages": [ ... ]                      // shown in this order; KEY1/KEY2 move through them
    }

(The // comments are only explanation: real JSON files cannot contain comments.)

Each entry in "pages" has a "type":

  "card" (the default): name, title and a QR code on the right
      name     required  your name; wraps to 2 lines, font shrinks to fit
      title    optional  job title / function; wraps to 2 lines
      company  optional  one line under the title
      qr       the QR source, ONE of these two (not both):
                 "qr":       text to encode, normally a URL
                 "qr_image": a picture of a QR code you already have: a file path
                             (relative to the JSON file) or an http(s) URL
                             (PNG, JPEG, GIF or BMP, max 5 MB)
      label    optional  name for the page in the generated header and firmware log

  "qr": a big QR code on the left with a caption
      qr       the QR source: "qr" or "qr_image", exactly as for "card"
      caption  optional  text next to the QR (default "Scan to connect")
      footer   optional  small line at the bottom, e.g. the short URL
      label    optional  as above

  "text": free text, as large as it fits
      text     required  the text; "\\n" starts a new line, long lines are word-wrapped
      align    optional  "center" (default), "left" or "right"
      bold     optional  true (default) or false

  "image": any picture (PNG, JPEG, GIF, BMP), a file path or an http(s) URL, max 5 MB
      image    required  the picture
      fit      optional  "contain" (default, whole picture, white bars) or "cover" (fills, crops)
      dither   optional  "threshold" (default, logos and line art) or "floyd" (photos)

With "qr_image" the picture is cropped to the code, its module grid is read back and the QR
is redrawn sharp at the panel's scale, so a large or slightly blurry export is fine. It must be
a dark-on-light code (rotation is fine, mirrored or inverted is not), and not a logo or styled QR. If it cannot be read the script
stops with a message. Check preview/pageN.png and scan it.

Keep the QR text short (a URL, not a vCard): it is shown as a 1-bit image, and the script
warns when a module gets smaller than 3 px and refuses below 2.
The page order in the JSON is the order of page0.png, page1.png, ... and of the firmware pages.

Needs: pillow and segno (in a venv: python3 -m venv .venv && . .venv/bin/activate && pip install pillow segno)
"""

import argparse
import io
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

import segno
from PIL import Image, ImageDraw, ImageFont

SCALE = 1.0  # layout was designed on 296x128; set from the panel height in main()


def sc(n):
    """Scale a design size (pixels or font size) to the panel."""
    return max(1, round(n * SCALE))


PREVIEW_ZOOM = 1  # the 800x480 PNG is already big


MARGIN = 6  # scaled in main()
QUIET_MODULES = 2  # QR quiet zone; the spec says 4, but 2 scans fine on e-paper
MIN_QR_SCALE = 2  # below this a QR is unreliable; below 3 we warn

BOLD_FONTS = [
    "DejaVuSans-Bold.ttf",
    "LiberationSans-Bold.ttf",
    "Arial Bold.ttf",
    "arialbd.ttf",
    "Helvetica-Bold.ttc",
]
REGULAR_FONTS = [
    "DejaVuSans.ttf",
    "LiberationSans-Regular.ttf",
    "Arial.ttf",
    "arial.ttf",
    "Helvetica.ttc",
]


# ----------------------------------------------------------------------------
# Fonts and text layout
# ----------------------------------------------------------------------------

def load_font(size, bold, override=None):
    candidates = [override] if override else (BOLD_FONTS if bold else REGULAR_FONTS)
    for name in candidates:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    # Pillow >= 10.1 can scale its built-in font when FreeType is available.
    return ImageFont.load_default(size)


def line_height(font):
    ascent, descent = font.getmetrics()
    return ascent + descent


def text_width(draw, text, font):
    left, _, right, _ = draw.textbbox((0, 0), text, font=font)
    return right - left


def wrap(draw, text, font, max_width):
    """Greedy word wrap; words wider than max_width are split by character."""
    lines, current = [], ""
    for word in text.split():
        trial = word if not current else current + " " + word
        if text_width(draw, trial, font) <= max_width:
            current = trial
            continue
        if current:
            lines.append(current)
            current = ""
        while text_width(draw, word, font) > max_width and len(word) > 1:
            cut = len(word) - 1
            while cut > 1 and text_width(draw, word[:cut], font) > max_width:
                cut -= 1
            lines.append(word[:cut])
            word = word[cut:]
        current = word
    if current:
        lines.append(current)
    return lines


def ellipsize(draw, text, font, max_width):
    if text_width(draw, text, font) <= max_width:
        return text
    while len(text) > 1 and text_width(draw, text + "...", font) > max_width:
        text = text[:-1]
    return text.rstrip() + "..."


def layout(draw, text, bold, max_width, max_lines, sizes, font_path):
    """Largest font size from `sizes` whose wrapped text fits in max_lines."""
    for size in sizes:
        font = load_font(size, bold, font_path)
        lines = wrap(draw, text, font, max_width)
        if len(lines) <= max_lines:
            return font, lines
    font = load_font(sizes[-1], bold, font_path)
    all_lines = wrap(draw, text, font, max_width)
    lines = all_lines[:max_lines]
    if lines:
        # Text was cut: join what was dropped so the last line ends with "..."
        tail = " ".join(all_lines[max_lines - 1:])
        lines[-1] = ellipsize(draw, tail + ("" if len(all_lines) <= max_lines else " ..."), font, max_width)
    return font, lines


def draw_lines(draw, x, y, lines, font, gap=None):
    gap = sc(1) if gap is None else gap
    lh = line_height(font)
    for line in lines:
        draw.text((x, y), line, font=font, fill=0)
        y += lh + gap
    return y


def block_height(lines, font, gap=None):
    gap = sc(1) if gap is None else gap
    return len(lines) * (line_height(font) + gap) - (gap if lines else 0)


# ----------------------------------------------------------------------------
# QR codes
# ----------------------------------------------------------------------------

BASE_DIR = Path(".")  # image paths in the JSON are relative to the JSON file


MAX_IMAGE_BYTES = 5 * 1024 * 1024
FETCH_TIMEOUT_S = 15


def open_image(source, label="qr_image"):
    """Open a picture from a local path or an http(s) URL."""
    if str(source).lower().startswith(("http://", "https://")):
        req = urllib.request.Request(str(source), headers={"User-Agent": "make_badge.py"})
        try:
            with urllib.request.urlopen(req, timeout=FETCH_TIMEOUT_S) as resp:
                data = resp.read(MAX_IMAGE_BYTES + 1)
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise SystemExit(f"{label} {str(source)!r}: download failed ({e})")
        if len(data) > MAX_IMAGE_BYTES:
            raise SystemExit(f"{label} {str(source)!r}: picture is larger than {MAX_IMAGE_BYTES // 1024 // 1024} MB")
        try:
            im = Image.open(io.BytesIO(data))
            im.load()
        except (OSError, ValueError):
            raise SystemExit(
                f"{label} {str(source)!r}: the URL did not return a PNG/JPEG/GIF/BMP picture "
                "(SVG and web pages are not supported)"
            )
        return im
    path = Path(source)
    if not path.is_absolute():
        path = BASE_DIR / path
    try:
        im = Image.open(path)
        im.load()
    except (OSError, ValueError) as e:
        raise SystemExit(f"{label} {str(source)!r}: cannot open picture ({e})")
    return im


def matrix_from_image(source):
    """Read the module matrix of a QR code from a picture (PNG, JPEG, GIF, BMP).

    The picture is a local file or an http(s) URL. It is cropped to the dark
    pixels, the module size is taken from the top-left finder pattern (7 modules
    wide), and every module is sampled at its centre. That gives back the exact
    module grid, so the QR is redrawn crisp at an integer scale whatever the size
    or blur of the source picture.
    Dark modules on a light background only; SVG is not supported.
    """
    im = open_image(source)
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
        im = Image.alpha_composite(bg, im)
    gray = im.convert("L")
    dark = gray.point(lambda v: 255 if v < 128 else 0)
    box = dark.getbbox()
    if box is None:
        raise SystemExit(f"{label} {str(source)!r}: no dark pixels (dark modules on a light background expected)")
    dark = dark.crop(box)
    w, h = dark.size
    if abs(w - h) > max(2, 0.03 * max(w, h)):
        raise SystemExit(
            f"qr_image {str(source)!r}: the QR is not square after cropping ({w}x{h} px); "
            "crop the picture to the QR code or give a cleaner export"
        )
    n = (w + h) // 2
    px = dark.load()
    run = 0
    while run < w and px[run, 0] == 255:
        run += 1
    module = run / 7.0
    modules = round(n / module) if module >= 1 else 0
    if module < 1 or modules < 21 or modules > 177 or (modules - 17) % 4:
        raise SystemExit(
            f"qr_image {str(source)!r}: cannot find the module grid (estimated {modules} modules). "
            "Use a sharp picture of the QR code with a dark-on-light code, or give the link with \"qr\""
        )
    step = n / modules
    matrix = tuple(
        tuple(1 if px[min(w - 1, int((c + 0.5) * step)), min(h - 1, int((r + 0.5) * step))] == 255 else 0
              for c in range(modules))
        for r in range(modules)
    )
    # Top-left finder pattern: 7x7 dark ring, light ring, 3x3 dark centre.
    finder_ok = all(
        matrix[r][c] == (1 if max(abs(r - 3), abs(c - 3)) in (0, 1, 3) else 0)
        for r in range(7) for c in range(7)
    )
    if not finder_ok:
        raise SystemExit(
            f"qr_image {str(source)!r}: no QR finder pattern at the top-left corner after cropping; "
            "is the picture mirrored, inverted or not a QR code?"
        )
    return matrix


def qr_matrix(page):
    """Module matrix (and a name for messages) for a page: from 'qr' text or 'qr_image'."""
    has_text, has_image = "qr" in page, "qr_image" in page
    if has_text == has_image:
        raise SystemExit(
            f"page {page.get('label') or page.get('name') or page.get('caption') or '?'}: "
            "give exactly one of \"qr\" (text/link) or \"qr_image\" (picture file)"
        )
    if has_text:
        return segno.make(page["qr"], error="m", boost_error=False).matrix, repr(page["qr"])
    return matrix_from_image(page["qr_image"]), repr(str(page["qr_image"]))


def draw_qr(draw, page, box_height, x_anchor, align, max_height=None, v_area=None):
    """Draw a QR code as crisp integer-scaled modules.

    Returns (x, size). x_anchor is the right edge when align == "right" and the
    left edge when align == "left".
    """
    matrix, text = qr_matrix(page)
    modules = len(matrix)
    total = modules + 2 * QUIET_MODULES
    scale = box_height // total
    if scale < 3 and max_height:  # small panel: trade text room for a scannable QR
        scale = max(scale, max_height // total)
    if scale < MIN_QR_SCALE:
        raise SystemExit(
            f"QR payload too long for the panel ({modules}x{modules} modules, scale {scale}px): "
            f"{text}. Shorten it (use a URL, not a full vCard), or use a smaller QR."
        )
    if scale < 3:
        print(
            f"warning: QR is {modules}x{modules} modules at {scale}px per module; "
            "test scanning it on the real panel",
            file=sys.stderr,
        )
    size = total * scale
    x = x_anchor - size if align == "right" else x_anchor
    top, bottom = v_area if v_area else (0, draw.im.size[1])
    y = top + (bottom - top - size) // 2
    draw.rectangle([x, y, x + size - 1, y + size - 1], fill=255)  # quiet zone
    ox = x + QUIET_MODULES * scale
    oy = y + QUIET_MODULES * scale
    for row, bits in enumerate(matrix):
        for col, bit in enumerate(bits):
            if bit:
                px, py = ox + col * scale, oy + row * scale
                draw.rectangle([px, py, px + scale - 1, py + scale - 1], fill=0)
    return x, size


# ----------------------------------------------------------------------------
# Pages
# ----------------------------------------------------------------------------

def new_canvas(width, height):
    img = Image.new("L", (width, height), 255)
    return img, ImageDraw.Draw(img)


def qr_modules(page):
    return len(qr_matrix(page)[0]) + 2 * QUIET_MODULES


def render_card(page, width, height, font_path):
    """Name, title and QR code; the company line runs along the bottom, full width.

    The QR code and the name/title stack share the area above the company line. On a small
    panel, where that would make the QR too small to scan, the company goes back under the title.
    """
    img, draw = new_canvas(width, height)
    title_text = page.get("title", "")
    company_text = page.get("company", "")
    full_w = width - 2 * MARGIN
    comp_sizes = [sc(14), sc(13), sc(12), sc(11), sc(10), sc(9), sc(8)]

    # Company: prefer one line (shrinking down to sc(10)), else up to two lines.
    comp_font, comp_lines = (None, [])
    if company_text:
        for size in comp_sizes[:5]:
            f = load_font(size, True, font_path)
            if text_width(draw, company_text, f) <= full_w:
                comp_font, comp_lines = f, [company_text]
                break
        else:
            comp_font, comp_lines = layout(draw, company_text, True, full_w, 2, comp_sizes, font_path)

    cap = int(height * 0.70)
    old_box = min(height - 2 * MARGIN, cap)
    comp_h = (block_height(comp_lines, comp_font) + sc(8)) if comp_lines else 0
    top_h = height - 2 * MARGIN - comp_h
    modules = qr_modules(page)
    def qr_scale(box, limit):  # same rule as draw_qr: small panels may use up to `limit`
        sc_ = box // modules
        return max(sc_, limit // modules) if sc_ < 3 else sc_

    old_scale = qr_scale(old_box, height - 2 * MARGIN)
    new_scale = qr_scale(min(top_h, cap), top_h)
    footer_mode = bool(comp_lines) and new_scale >= min(3, old_scale) and new_scale >= old_scale

    if footer_mode:
        area = (MARGIN, MARGIN + top_h)
        qr_x, _ = draw_qr(draw, page, min(top_h, cap), width - MARGIN, "right",
                          max_height=top_h, v_area=area)
        avail = top_h
        stack_comp = (None, [])
    else:
        area = (MARGIN, height - MARGIN)
        qr_x, _ = draw_qr(draw, page, old_box, width - MARGIN, "right",
                          max_height=height - 2 * MARGIN, v_area=None)
        avail = height - 2 * MARGIN
        stack_comp = (None, [])

    x0 = MARGIN + sc(2)
    max_w = qr_x - x0 - MARGIN
    if not footer_mode and company_text:  # back under the title: one line, text column width
        stack_comp = layout(draw, company_text, True, max_w, 1, comp_sizes, font_path)

    # Shrink the name until the whole stack fits.
    for name_max in range(sc(30), sc(13), -1):
        name_font, name_lines = layout(
            draw, page["name"], True, max_w, 2, list(range(name_max, sc(13), -1)), font_path
        )
        title_font, title_lines = layout(
            draw, title_text, False, max_w, 3, [sc(13), sc(12), sc(11), sc(10), sc(9), sc(8)], font_path
        ) if title_text else (None, [])
        sc_font, sc_lines = stack_comp
        total = block_height(name_lines, name_font)
        if title_lines:
            total += sc(10) + block_height(title_lines, title_font)
        if sc_lines:
            total += sc(8) + block_height(sc_lines, sc_font)
        if total <= avail:
            break
    else:
        raise SystemExit(f"text does not fit on the badge: {page['name']!r}")

    y = area[0] + (avail - total) // 2
    y = draw_lines(draw, x0, y, name_lines, name_font)
    if title_lines:
        y += sc(4)
        draw.line([x0, y, x0 + min(max_w, sc(60)), y], fill=0, width=sc(2))
        y += sc(6)
        y = draw_lines(draw, x0, y, title_lines, title_font)
    if sc_lines:
        y += sc(8)
        draw_lines(draw, x0, y, sc_lines, sc_font)
    if footer_mode:
        draw_lines(draw, x0, height - MARGIN - block_height(comp_lines, comp_font), comp_lines, comp_font)
    return img


def render_qr_page(page, width, height, font_path):
    img, draw = new_canvas(width, height)
    qr_x, qr_size = draw_qr(draw, page, int(height * 0.80), MARGIN, "left", max_height=height - 2 * MARGIN)
    x0 = qr_x + qr_size + sc(12)
    max_w = width - x0 - MARGIN
    caption = page.get("caption", "Scan to connect")
    cap_font, cap_lines = layout(draw, caption, True, max_w, 3, [sc(26), sc(24), sc(22), sc(20), sc(18), sc(16), sc(14)], font_path)
    foot = page.get("footer", "")
    foot_font, foot_lines = layout(draw, foot, False, max_w, 2, [sc(12), sc(11), sc(10), sc(9), sc(8), sc(7)], font_path) if foot else (None, [])
    total = block_height(cap_lines, cap_font) + (sc(8) + block_height(foot_lines, foot_font) if foot_lines else 0)
    y = (height - total) // 2
    y = draw_lines(draw, x0, y, cap_lines, cap_font)
    if foot_lines:
        draw_lines(draw, x0, y + sc(8), foot_lines, foot_font)
    return img


def render_text_page(page, width, height, font_path):
    """Free text, as large as it fits. Lines break at "\\n" and are word-wrapped."""
    img, draw = new_canvas(width, height)
    avail_w, avail_h = width - 2 * MARGIN, height - 2 * MARGIN
    text = str(page["text"])
    bold = page.get("bold", True)
    align = page.get("align", "center")
    if align not in ("left", "center", "right"):
        raise SystemExit(f"text page: align must be left, center or right, not {align!r}")
    gap = sc(2)

    def wrapped(font):
        lines = []
        for para in text.split("\n"):
            lines += wrap(draw, para, font, avail_w) or [""]
        return lines

    chosen = None
    for size in range(int(height * 0.6), sc(7) - 1, -2):
        font = load_font(size, bold, font_path)
        lines = wrapped(font)
        if block_height(lines, font, gap) <= avail_h:
            chosen = (font, lines)
            break
    if chosen is None:  # too much text: keep what fits and end with "..."
        font = load_font(sc(7), bold, font_path)
        lines = wrapped(font)
        fit = max(1, (avail_h + gap) // (line_height(font) + gap))
        if len(lines) > fit:
            lines = lines[:fit]
            lines[-1] = ellipsize(draw, lines[-1] + " ...", font, avail_w)
        chosen = (font, lines)
    font, lines = chosen

    y = MARGIN + (avail_h - block_height(lines, font, gap)) // 2
    for line in lines:
        w = text_width(draw, line, font)
        x = MARGIN if align == "left" else (width - MARGIN - w if align == "right" else (width - w) // 2)
        draw.text((x, y), line, font=font, fill=0)
        y += line_height(font) + gap
    return img


def render_image_page(page, width, height, font_path):
    """Any picture, fitted to the panel and reduced to black and white.

    fit:    "contain" (default, whole picture, white bars) or "cover" (fills, crops)
    dither: "threshold" (default, logos and line art), "floyd" (photos) or "auto"
            (picture is already pure black and white: threshold)
    """
    im = open_image(page["image"], "image")
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        white = Image.new("RGBA", im.size, (255, 255, 255, 255))
        im = Image.alpha_composite(white, im)
    gray = im.convert("L")
    fit = page.get("fit", "contain")
    if fit not in ("contain", "cover"):
        raise SystemExit(f"image page: fit must be contain or cover, not {fit!r}")
    iw, ih = gray.size
    scale = max(width / iw, height / ih) if fit == "cover" else min(width / iw, height / ih)
    new = (max(1, round(iw * scale)), max(1, round(ih * scale)))
    gray = gray.resize(new, Image.Resampling.LANCZOS)
    canvas = Image.new("L", (width, height), 255)
    canvas.paste(gray, ((width - new[0]) // 2, (height - new[1]) // 2))
    mode = page.get("dither", "threshold")
    if mode not in ("threshold", "floyd", "auto"):
        raise SystemExit(f"image page: dither must be threshold, floyd or auto, not {mode!r}")
    if mode == "floyd":
        canvas = canvas.convert("1", dither=Image.Dither.FLOYDSTEINBERG).convert("L")
    return canvas


RENDERERS = {"card": render_card, "qr": render_qr_page, "text": render_text_page, "image": render_image_page}


# ----------------------------------------------------------------------------
# Packing and output
# ----------------------------------------------------------------------------

def to_mono(img, threshold=160):
    return img.point(lambda v: 255 if v >= threshold else 0, mode="L")


def pack(img):
    """Pack an 'L' image (0 = black, 255 = white): rows of ceil(width/8) bytes, MSB = leftmost, 1 = white."""
    width, height = img.size
    rb = (width + 7) // 8  # padding bits at the end of a row stay 0
    px = img.load()
    out = bytearray(rb * height)
    for y in range(height):
        for x in range(width):
            if px[x, y]:
                out[y * rb + x // 8] |= 0x80 >> (x % 8)
    return bytes(out)


def unpack(data, width, height):
    """Inverse of pack(), used to prove the layout round-trips."""
    img = Image.new("L", (width, height), 0)
    px = img.load()
    for y in range(height):
        for x in range(width):
            px[x, y] = 255 if data[y * ((width + 7) // 8) + x // 8] & (0x80 >> (x % 8)) else 0
    return img


def c_array(name, data):
    rows = []
    for i in range(0, len(data), 16):
        rows.append("\t" + ", ".join(f"0x{b:02x}" for b in data[i : i + 16]) + ",")
    return f"static const uint8_t {name}[BADGE_BUF_SIZE] = {{\n" + "\n".join(rows) + "\n};\n"


def write_header(path, pages, width, height):
    parts = [
        "/* Generated by tools/make_badge.py. Do not edit; change the JSON and re-run. */\n",
        "#ifndef BADGE_IMAGES_H_\n#define BADGE_IMAGES_H_\n\n",
        "#include <stddef.h>\n#include <stdint.h>\n\n",
        f"#define BADGE_WIDTH {width}\n#define BADGE_HEIGHT {height}\n",
        "#define BADGE_ROW_BYTES ((BADGE_WIDTH + 7) / 8)\n#define BADGE_BUF_SIZE (BADGE_ROW_BYTES * BADGE_HEIGHT)\n",
        f"#define BADGE_PAGE_COUNT {len(pages)}\n\n",
        "struct badge_page {\n\tconst char *name;\n\tconst uint8_t *data;\n};\n\n",
    ]
    for i, (_, data) in enumerate(pages):
        parts.append(c_array(f"badge_page_{i}", data) + "\n")
    parts.append("static const struct badge_page badge_pages[BADGE_PAGE_COUNT] = {\n")
    for i, (label, _) in enumerate(pages):
        parts.append(f'\t{{ "{label}", badge_page_{i} }},\n')
    parts.append("};\n\n#endif /* BADGE_IMAGES_H_ */\n")
    Path(path).write_text("".join(parts))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", help="JSON file describing the pages")
    ap.add_argument("-o", "--out", default="src/badge_images.h", help="C header to write")
    ap.add_argument("--preview", metavar="DIR", help="also write PNG previews (native size)")
    ap.add_argument("--font", help="TrueType font to use for all text (default: DejaVu/Liberation/Arial)")
    args = ap.parse_args()

    global BASE_DIR
    BASE_DIR = Path(args.config).resolve().parent
    cfg = json.loads(Path(args.config).read_text())
    width, height = cfg.get("width", 800), cfg.get("height", 480)
    if height % 8:
        print("warning: height is not a multiple of 8; the 2.13\" ssd16xx path needs one (use 120)", file=sys.stderr)
    global SCALE, MARGIN
    SCALE = height / 128
    MARGIN = sc(6)

    rendered = []
    for i, page in enumerate(cfg["pages"]):
        kind = page.get("type", "card")
        if kind not in RENDERERS:
            raise SystemExit(f"page {i}: unknown type {kind!r} (use {', '.join(RENDERERS)})")
        img = to_mono(RENDERERS[kind](page, width, height, args.font))
        data = pack(img)
        if unpack(data, width, height).tobytes() != img.tobytes():
            raise SystemExit(f"page {i}: pack/unpack round trip failed")
        label = page.get("label") or page.get("name") or page.get("caption") or (str(page["text"]).split("\n")[0][:24] if "text" in page else None) or f"page {i + 1}"
        rendered.append((label.replace('"', "'"), data))
        if args.preview:
            out_dir = Path(args.preview)
            out_dir.mkdir(parents=True, exist_ok=True)
            img.convert("1", dither=Image.Dither.NONE).resize(
                (width * PREVIEW_ZOOM, height * PREVIEW_ZOOM), Image.Resampling.NEAREST
            ).save(out_dir / f"page{i}.png")

    write_header(args.out, rendered, width, height)
    print(f"wrote {args.out}: {len(rendered)} page(s), {width}x{height}, {len(rendered[0][1])} bytes each")


if __name__ == "__main__":
    main()
