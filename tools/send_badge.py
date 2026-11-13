#!/usr/bin/env python3
"""Send one page to the badge over USB (or to the simulator): fire and forget.

The badge draws the page and keeps it on the e-paper (no storage on the badge: the next
KEY press goes back to the built-in pages). Needs: pip install pillow segno pyserial

Examples:
    python3 tools/send_badge.py --text "Back in 5 minutes"
    python3 tools/send_badge.py --text "Meeting room 2\\nTalk starts at 14:00" --align left
    python3 tools/send_badge.py --image logo.png
    python3 tools/send_badge.py --image photo.jpg --dither floyd --fit cover
    python3 tools/send_badge.py --image portrait.jpg --rotate 90 --fit cover   # portrait photo
    python3 tools/send_badge.py --text "Hello" --preview hello.png      # no badge needed
    python3 tools/send_badge.py --info                                    # ask the badge its size

    --port     serial port (default /dev/ttyACM0; the simulator prints its /dev/pts/N at start)
    --size WxH size used for --preview only; when sending, the badge reports its own size

The page is drawn with the same code as tools/make_badge.py, at the size the badge reports.
Protocol: see src/remote.h.
"""

import argparse
import re
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_badge as mb  # noqa: E402
from PIL import Image  # noqa: E402

MAGIC = b"BDG1"
CHUNK = 512


def packet(cmd, payload=b""):
    return MAGIC + cmd + struct.pack("<I", len(payload)) + payload + struct.pack("<I", zlib.crc32(payload))


class Badge:
    def __init__(self, port):
        try:
            import serial
        except ImportError:
            raise SystemExit("pyserial is missing: pip install pyserial")
        try:
            self.ser = serial.Serial(port, 115200, timeout=0.2)
        except (OSError, serial.SerialException) as e:
            raise SystemExit(
                f"cannot open {port}: {e}\n"
                "Is the badge plugged in and running, and is nothing else (screen, picocom) using the port? "
                "On Linux you may need the 'dialout' group."
            )
        self.ser.reset_input_buffer()

    def wait(self, wanted, timeout):
        """Read lines until one 'BADGE <OK|ERR> ...' arrives. Log lines are ignored."""
        end = time.time() + timeout
        buf = b""
        while time.time() < end:
            buf += self.ser.read(256)
            for m in re.finditer(rb"BADGE (OK|ERR) ([^\r\n]*)", buf):
                status, text = m.group(1).decode(), m.group(2).decode().strip()
                if status == "ERR":
                    raise SystemExit(f"badge says: {text}")
                if text.startswith(wanted):
                    return text
            buf = buf[-256:]
        raise SystemExit(f"no '{wanted}' reply from the badge within {timeout:.0f} s")

    def info(self):
        self.ser.write(packet(b"I"))
        text = self.wait("info", 5)
        m = re.match(r"info (\d+)x(\d+) row=(\d+) size=(\d+)", text)
        if not m:
            raise SystemExit(f"unexpected info reply: {text!r}")
        return tuple(int(v) for v in m.groups())

    def send_frame(self, data):
        pkt = packet(b"F", data)
        for i in range(0, len(pkt), CHUNK):
            self.ser.write(pkt[i : i + CHUNK])
        self.ser.flush()
        self.wait("received", 10)
        print("received by the badge, refreshing the panel (can take 15 s)...")
        self.wait("shown", 90)


def build_page(args, width, height):
    mb.BASE_DIR = Path(".")
    rotate = int(args.rotate)
    # Draw on a canvas of the shape you will read, then turn it onto the panel.
    cw, ch = (height, width) if rotate in (90, 270) else (width, height)
    mb.SCALE = min(cw, ch) / 128
    mb.MARGIN = mb.sc(6)
    if args.text is not None:
        page = {"type": "text", "text": args.text.replace("\\n", "\n"), "align": args.align}
    else:
        page = {"type": "image", "image": args.image, "fit": args.fit, "dither": args.dither}
    img = mb.RENDERERS[page["type"]](page, cw, ch, args.font)
    if rotate:
        img = img.rotate(rotate, expand=True)  # counter-clockwise
    return mb.to_mono(img)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    what = ap.add_mutually_exclusive_group()
    what.add_argument("--text", help='text to show; "\\n" starts a new line')
    what.add_argument("--image", help="picture to show: file path or http(s) URL")
    what.add_argument("--info", action="store_true", help="print the badge's panel size and exit")
    ap.add_argument("--align", default="center", choices=["left", "center", "right"])
    ap.add_argument("--fit", default="contain", choices=["contain", "cover"])
    ap.add_argument("--dither", default="threshold", choices=["threshold", "floyd"])
    ap.add_argument("--rotate", default="0", choices=["0", "90", "180", "270"],
                    help="turn the page: 90 puts the top of the page on the LEFT side of the panel "
                         "(portrait, turn the badge clockwise to read it), 270 on the right side, "
                         "180 upside down")
    ap.add_argument("--font", help="TrueType font file")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--preview", metavar="PNG", help="write the page to this PNG and do not send it")
    ap.add_argument("--size", default="800x480", help="panel size for --preview (default 800x480)")
    args = ap.parse_args()

    if not (args.text is not None or args.image or args.info):
        ap.error("give --text, --image or --info")

    if args.preview:
        if args.info:
            ap.error("--info needs the badge, not --preview")
        w, h = (int(v) for v in args.size.lower().split("x"))
        img = build_page(args, w, h)
        img.save(args.preview)
        print(f"wrote {args.preview} ({w}x{h}); not sent")
        return

    badge = Badge(args.port)
    w, h, row, size = badge.info()
    print(f"badge panel: {w}x{h}")
    if args.info:
        return
    img = build_page(args, w, h)
    data = mb.pack(img)
    if len(data) != size or (w + 7) // 8 != row:
        raise SystemExit(f"internal error: frame is {len(data)} bytes, badge expects {size}")
    badge.send_frame(data)
    print("done")


if __name__ == "__main__":
    main()
