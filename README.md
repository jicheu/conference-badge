# E-paper conference badge (Zephyr)

Name, title and QR code on a reusable e-paper badge, running Zephyr.

- **Hardware:** Seeed XIAO ePaper Display Board **EN04** (XIAO nRF52840 Plus) + 2.9" monochrome 296x128 panel (SSD1680), 24-pin FPC, jumper on "24 Pin".
- **Firmware:** Zephyr, upstream `ssd16xx` display driver. Full refresh only (no ghosting).
- **Pages:** several pre-rendered pages (name card, speaker card, big QR). On the board KEY1 = previous, KEY2 = next, KEY3 = redraw. In the simulator pages advance by themselves every 4 s.
- **Tested with:** Canonical Zephyr 26.04 LTS (Zephyr 4.4) in Workshop (`zephyr-26-04`), project folder at `/project/conference-badge`.

## 1. Make your badge pages

```sh
python3 -m venv .venv                   # Ubuntu refuses a system-wide pip install
source .venv/bin/activate
pip install pillow segno
cp badges.example.json badges.json      # then edit it
python3 tools/make_badge.py badges.json -o src/badge_images.h --preview preview
```

If `python3 -m venv` fails with "ensurepip is not available", install it first: `sudo apt install python3-venv`. Run `source .venv/bin/activate` again in each new terminal. Add `.venv/` to your `.gitignore`.

This writes two things:

- `src/badge_images.h`: the bitmaps the firmware shows. Rebuild the firmware after changing it.
- `preview/page0.png`, `page1.png`, ...: **one PNG per page, enlarged 3x, in the same order as in the JSON.** Open them to check the layout and scan the QR code with your phone before you build or flash anything.

### The JSON file

```json
{
  "pages": [
    { "type": "card", "name": "Alex Example", "title": "Principal Engineer",
      "company": "Example Corp", "qr": "https://example.com/alex" },
    { "type": "qr", "qr": "https://example.com/alex",
      "caption": "Scan to connect", "footer": "example.com/alex" }
  ]
}
```

| Field | Page type | Required | Meaning |
|---|---|---|---|
| `type` | both | no | `card` (default) or `qr` |
| `name` | card | yes | your name; wraps to 2 lines, the font shrinks to fit |
| `title` | card | no | job title / function; wraps to 2 lines |
| `company` | card | no | one line under the title |
| `qr` | both | one of `qr` / `qr_image` | text encoded in the QR code, normally a URL |
| `qr_image` | both | one of `qr` / `qr_image` | a picture of an existing QR code (PNG, JPEG, GIF, BMP): a file path relative to the JSON file, **or** an `http(s)://` URL |
| `caption` | qr | no | text next to the QR (default "Scan to connect") |
| `footer` | qr | no | small line at the bottom, e.g. the short URL |
| `label` | both | no | name of the page in the generated header and the firmware log |
| `width`, `height` | top level | no | panel size, default 296x128 (height must be a multiple of 8) |

**QR from a picture:** use `"qr_image": "my-qr.png"` or `"qr_image": "https://example.com/my-qr.png"` instead of `"qr"` (one or the other, not both) when you already have the QR code, for example exported from a link shortener or a vCard generator. A URL is downloaded when you run the script (15 s timeout, 5 MB maximum) and must point straight at the picture, not at a web page. The picture is not stored in the firmware, only the redrawn QR. The script crops the picture, reads its module grid and redraws it crisp at the panel's scale, so the file can be any size. It must be a dark code on a light background (rotation is fine, mirrored or inverted is not) and a plain QR (no logo in the middle, no rounded or coloured styling). If it cannot read the picture it stops with a message. Check the preview PNG and scan it.

Pages appear in the order of the `pages` list. Keep the QR text short (a URL, not a vCard): the script warns when a QR module gets smaller than 3 px and refuses below 2. `python3 tools/make_badge.py --help` shows the same information.

## 2. Run it in the simulator (Zephyr `native_sim`)

The simulator draws the panel in an SDL window with the same pixel layout as the SSD1680, so it tests the pages and the app logic without a board. It does **not** test the display wiring or the real driver.

`boards/native_sim_native_64.overlay` and `.conf` set the window to 296x128 mono and enable `CONFIG_BADGE_AUTO_CYCLE_MS` (next page every 4 s, since there are no buttons).

### Give the Workshop access to your screen

Workshop containers have no display by default. Add a small project SDK with the `desktop` interface once:

```sh
mkdir -p .workshop/gui
cat > .workshop/gui/sdk.yaml <<'EOT'
name: gui
plugs:
  desktop:
    interface: desktop
EOT
```

Add it to `.workshop/zephyr-26-04.yaml`:

```yaml
sdks:
  # existing entries...
  - name: project-gui
```

Apply and connect (the connection is **not** automatic: repeat `workshop connect` after each launch or refresh):

```sh
workshop refresh zephyr-26-04
workshop connect zephyr-26-04/gui:desktop
workshop connections zephyr-26-04      # slot ws/system:desktop, note "manual"
```

Check inside the workshop that `$DISPLAY` or `$WAYLAND_DISPLAY` is set (`workshop shell zephyr-26-04`). The container also needs the SDL2 runtime library; if the program exits with an SDL error, install `libsdl2-2.0-0` there.

### Build and run

```sh
workshop run zephyr-26-04 -- build -p always -b native_sim/native/64 /project/conference-badge
workshop run zephyr-26-04 -- build -t run
```

A 296x128 window (2x zoom) shows page 1, then changes page every 4 s. Stop with Ctrl+C. The terminal prints `display 296x128 ...` and `showing page N/M: ...`. If it says `display layout does not match badge images`, `native_sim_native_64.conf` was not applied.

You can also start the binary yourself from a host terminal with a display: `build/zephyr/zephyr.exe` (it is a normal Linux program despite the name).

Notes:

- `qemu_x86` is not used: it has no display, so use `native_sim`.
- Board overlay and conf files must be named after the full board target. For `native_sim/native/64` that is `boards/native_sim_native_64.*` (or the short form `native_sim_64.*`). A plain `native_sim.overlay` is silently ignored in this Zephyr.
- After adding or renaming overlay files, delete the build directory (`rm -rf /project/zephyr/build`) or use `-p always`: an existing build directory does not notice new overlay files.

## 3. Build and flash on the board

The Canonical 26.04 LTS tree (Zephyr 4.4) does **not** have the `xiao_nrf52840_plus` board, so use `xiao_ble` (same nRF52840 and UF2 bootloader). `boards/xiao_ble.overlay` addresses DC/RST as raw `gpio0` pins, since the Plus-only connector pads do not exist on `xiao_ble`.

Add the Arm toolchain SDK to `.workshop/zephyr-26-04.yaml` (once):

```yaml
sdks:
  # existing entries...
  - name: zephyr-arm
    channel: 26.04.0/stable

connections:
  # existing entries...
  - plug: zephyr-sdk-ng:arm
    slot: zephyr-arm:toolchain
```

```sh
workshop refresh zephyr-26-04
workshop run zephyr-26-04 -- build -p always -b xiao_ble/nrf52840 /project/conference-badge
```

Do not use the `/sense` variant. Flash by double-tapping reset (the XIAO appears as a USB drive) and copying `build/zephyr/zephyr.uf2` to it from the host. This avoids Workshop's USB passthrough, which Canonical lists as a known issue in 26.04.

On a newer Zephyr that has the Plus board, use `-b xiao_nrf52840_plus/nrf52840` and `boards/xiao_nrf52840_plus.overlay` instead.

Without Workshop, the same commands work with plain `west build -b <board> conference-badge -p`.

## What is verified and what is not

Verified:

- The overlay parses and satisfies the real Zephyr bindings (ssd1680, mipi-dbi-spi, gpio-keys, gpio-hog), and every pin resolves through Zephyr's XIAO connector tables to the MCU pin in the table below (checked for the Plus overlay; the `xiao_ble` one differs only in DC/RST).
- `make_badge.py` output decodes as the right QR codes, and the pixel packing round-trips (1 = white, vertical 8-pixel tiles, MSB at the top, as `ssd16xx` reports via `display_get_capabilities`).
- The `native_sim` simulator build compiles and links on Zephyr 26.04 LTS.

Not done: no run on real hardware yet. The simulator is also not confirmed visually by me.

## EN04 pin map (see `boards/xiao_nrf52840_plus.overlay`)

| Signal | XIAO pad | MCU pin | Source |
|---|---|---|---|
| CS | D7 | P1.12 | Seeed_GFX2 EN04 config |
| SCK | D8 | P1.13 | Seeed_GFX2, = Zephyr `xiao_spi` |
| MOSI | D10 | P1.15 | Seeed_GFX2, = Zephyr `xiao_spi` |
| BUSY | D3 | P0.29 | Seeed_GFX2 |
| Panel power | D6 | P1.11 | Seeed_GFX2 ("enable", active high) |
| **DC** | D16 | P0.31 | Seeed_GFX2 comment: "D16 (raw GPIO 35)", **confirm** |
| **RST** | D11 | P0.15 | Seeed_GFX2 comment: "D11 (raw GPIO 30)", **confirm** |
| KEY1/2/3 | D1/D2/D4 | P0.03/P0.28/P0.04 | Seeed wiki |

If the panel stays blank, check DC/RST against the EN04 schematic (Seeed wiki, Resources) first. The "raw GPIO" numbers in the library are Arduino-core indices, so I matched on the D-names instead.

The overlays disable `uart0` and `i2c1` (and `vbatt` on the Plus board) because the EN04 reuses their pins.

## Known gaps

- **Power:** Zephyr's `ssd16xx` driver has no deep-sleep support, and the panel supply is held on. Measure idle current before promising battery life; the fix is a small driver addition or switching the panel supply (D6) around refreshes.
- **Battery gauge:** not implemented. The EN04 has its own divider (A0 = P0.02, enable on A5 = P0.05; Seeed's Arduino example uses a x7.16 factor).
- **Panel tuning:** `tssv` and the border waveform are copied from Zephyr's SSD1680 shield overlay (a 2.13" panel). If you see ghosting or a dark border, tune them from the panel datasheet.
- **Updating a badge** means regenerating the header and re-flashing. Over-the-air updates (BLE/MCUmgr or NFC) are the natural next step on this chip.
