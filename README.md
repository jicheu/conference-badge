# E-paper conference badge (Zephyr)

Name, title and QR code on a reusable e-paper badge running Zephyr, plus any text or picture you send to it over USB.

- **Hardware:** Seeed XIAO ePaper Display Board **EN04** (XIAO nRF52840) + a Seeed monochrome panel: **4.26" 800x480** (SSD1677, ref MOB260722009, default) or **2.13" 122x250** (SSD1680). See section 1.
- **Firmware:** Zephyr. Zephyr 4.4 has **no SSD1677 driver**, so this project ships its own small one (`src/ssd1677.c`, binding in `dts/bindings/seeed,ssd1677.yaml`). Full refresh only (no ghosting), controller put in deep sleep after each refresh.
- **Built-in pages:** pre-rendered from a JSON file (name card, big QR, free text, picture). On the board KEY1 = previous, KEY2 = next, KEY3 = redraw. In the simulator pages advance by themselves every 4 s.
- **Remote pages:** `tools/send_badge.py` sends a text or picture over USB (section 5), or `tools/badge_ble.html` over Bluetooth LE from a phone (section 6): no rebuild, nothing stored.
- **Tested with:** Canonical Zephyr 26.04 LTS (Zephyr 4.4) in Workshop (`zephyr-26-04`), project folder at `/project/conference-badge`.

| Path | What it is |
|---|---|
| `tools/make_badge.py` | JSON to `src/badge_images.h` (built-in pages) and preview PNGs |
| `tools/send_badge.py` | send one text or picture to a running badge over USB |
| `tools/badge_ble.html` | same, over Bluetooth LE from a browser (Web Bluetooth); published by `.github/workflows/pages.yml` |
| `badges.example.json`, `badges.basic.json`, `badges.2in13.json` | example inputs (full, minimal, 2.13" panel) |
| `src/main.c`, `src/ssd1677.c`, `src/remote.c`, `src/ble.c` | firmware: pages and keys, display driver, packet receiver, Bluetooth transport |
| `ble.conf` | Bluetooth build options (`-DEXTRA_CONF_FILE=ble.conf`) |
| `boards/` | devicetree overlays and config per board / panel / simulator |

## 1. Choose your panel (4.26" or 2.13")

Two panels are supported. **Three things must match, every time you switch:** the JSON size, the generated header, and the build flags. Mixing them gives `display layout does not match badge images` in the serial log and nothing is drawn.

| | 4.26" (default) | 2.13" |
|---|---|---|
| Panel | Seeed 4.26" mono, SSD1677 | Seeed 2.13" mono, SSD1680 (GDEY0213B74) |
| Pixels used | 800x480 | 250x120 (landscape) |
| Driver | this project's `src/ssd1677.c` | Zephyr's `ssd16xx` |
| JSON | `badges.json` without `width`/`height`, or `"width": 800, "height": 480` | `"width": 250, "height": 120` (start from `badges.2in13.json`) |
| Extra build flags | none | `-DEXTRA_DTC_OVERLAY_FILE=boards/panel_2in13.overlay -DEXTRA_CONF_FILE=boards/panel_2in13.conf` |
| Simulator | yes (800x480) | no |

**Switch to the 4.26" (`badges.json` is the file you create in section 2):**

```sh
python3 tools/make_badge.py badges.json -o src/badge_images.h --preview preview
workshop run zephyr-26-04 -- build -p always -b xiao_ble/nrf52840 /project/conference-badge
```

**Switch to the 2.13":**

```sh
python3 tools/make_badge.py badges.2in13.json -o src/badge_images.h --preview preview
workshop run zephyr-26-04 -- build -p always -b xiao_ble/nrf52840 /project/conference-badge -- \
  -DEXTRA_DTC_OVERLAY_FILE=boards/panel_2in13.overlay -DEXTRA_CONF_FILE=boards/panel_2in13.conf
```

Always use `-p always` (clean build) when you switch, otherwise the old panel configuration stays in the build directory. Then flash `zephyr/build/zephyr/zephyr.uf2` as usual. Keep one JSON file per panel (for example `badges.json` and `badges.2in13.json`) with the same text, so you can regenerate either one.

## 2. Make your badge pages

```sh
python3 -m venv .venv                   # Ubuntu refuses a system-wide pip install
source .venv/bin/activate
pip install pillow segno pyserial
cp badges.example.json badges.json      # then edit it (badges.basic.json: the minimal version that produced the committed header)
python3 tools/make_badge.py badges.json -o src/badge_images.h --preview preview
```

If `python3 -m venv` fails with "ensurepip is not available", install it first: `sudo apt install python3-venv`. Run `source .venv/bin/activate` again in each new terminal. Add `.venv/` to your `.gitignore`.

This writes two things:

- `src/badge_images.h`: the bitmaps the firmware shows. Rebuild the firmware after changing it.
- `preview/page0.png`, `page1.png`, ...: **one PNG per page, at the panel's native size (800x480 or 250x120), in the same order as in the JSON.** Open them to check the layout and scan the QR code with your phone before you build or flash anything.

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
| `type` | all | no | `card` (default), `qr`, `text` or `image` |
| `name` | card | yes | your name; wraps to 2 lines, the font shrinks to fit |
| `title` | card | no | job title / function; wraps to 3 lines, the font shrinks to fit |
| `company` | card | no | run along the bottom of the badge across the full width, one line (smaller font if needed) or two; on the small 2.13" panel it stays one line under the title |
| `qr` | both | one of `qr` / `qr_image` | text encoded in the QR code, normally a URL |
| `qr_image` | both | one of `qr` / `qr_image` | a picture of an existing QR code (PNG, JPEG, GIF, BMP): a file path relative to the JSON file, **or** an `http(s)://` URL |
| `caption` | qr | no | text next to the QR (default "Scan to connect") |
| `footer` | qr | no | small line at the bottom, e.g. the short URL |
| `text` | text | yes | the text; `\n` starts a new line, long lines wrap, the font is as large as fits |
| `align`, `bold` | text | no | `center` (default), `left`, `right`; bold on by default |
| `image` | image | yes | a picture (PNG, JPEG, GIF, BMP): file path relative to the JSON, or `http(s)://` URL |
| `fit` | image | no | `contain` (default: whole picture, white bars) or `cover` (fills the panel, crops) |
| `dither` | image | no | `threshold` (default, logos and line art) or `floyd` (photos) |
| `label` | all | no | name of the page in the generated header and the firmware log |
| `width`, `height` | top level | no | panel size, default 800x480 (2.13": 250 x 120). Must match the panel you build for, see section 1. Fonts and margins scale with the height. |

**QR from a picture:** use `"qr_image": "my-qr.png"` or `"qr_image": "https://example.com/my-qr.png"` instead of `"qr"` (one or the other, not both) when you already have the QR code, for example exported from a link shortener or a vCard generator. A URL is downloaded when you run the script (15 s timeout, 5 MB maximum) and must point straight at the picture, not at a web page. The picture is not stored in the firmware, only the redrawn QR. The script crops the picture, reads its module grid and redraws it crisp at the panel's scale, so the file can be any size. It must be a dark code on a light background (rotation is fine, mirrored or inverted is not) and a plain QR (no logo in the middle, no rounded or coloured styling). If it cannot read the picture it stops with a message. Check the preview PNG and scan it.

Pages appear in the order of the `pages` list. Keep the QR text short (a URL, not a vCard): the script warns when a QR module gets smaller than 3 px and refuses below 2. `python3 tools/make_badge.py --help` shows the same information.

## 3. Run it in the simulator (Zephyr `native_sim`)

The simulator draws the panel in an SDL window with the same pixel layout as the driver reports (800x480, 1 bit per pixel, row by row), so it tests the pages and the app logic without a board. It does **not** test the display wiring or the real driver.

`boards/native_sim_native_64.overlay` and `.conf` set the window to 800x480 mono and enable `CONFIG_BADGE_AUTO_CYCLE_MS` (next page every 4 s, since there are no buttons).

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

An 800x480 window shows page 1, then changes page every 4 s. Stop with Ctrl+C. The terminal prints `display 800x480 ...` and `showing page N/M: ...`. If it says `display layout does not match badge images`, `native_sim_native_64.conf` was not applied.

**If the window crashes inside the workshop** with `X Error ... BadShmSeg (MIT-SHM)`, run the binary on the host instead (build in the workshop as above). This is the likely cause: SDL's X11 shared memory cannot cross the container boundary (not confirmed). The binary is a normal Linux program despite the name:

```sh
./zephyr/build/zephyr/zephyr.exe       # from your workshop folder, on the host
```

The simulator also creates a serial pseudo-terminal (`uart connected to pseudotty: /dev/pts/N`), which section 5 uses to test `send_badge.py` without a board.

Notes:

- `qemu_x86` is not used: it has no display, so use `native_sim`.
- Board overlay and conf files must be named after the full board target. For `native_sim/native/64` that is `boards/native_sim_native_64.*` (or the short form `native_sim_64.*`). A plain `native_sim.overlay` is silently ignored in this Zephyr.
- After adding or renaming overlay files, delete the build directory (`rm -rf /project/zephyr/build`) or use `-p always`: an existing build directory does not notice new overlay files.

## 4. Build and flash on the board

The Canonical 26.04 LTS tree (Zephyr 4.4) does **not** have the `xiao_nrf52840_plus` board, so use `xiao_ble` (same nRF52840 and UF2 bootloader). `boards/xiao_ble.overlay` wires the panel to the EN04 pins and addresses DC/RST as raw `gpio0` pins. `boards/xiao_ble.conf` enables SPI and MIPI DBI for the out-of-tree driver.

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

Do not use the `/sense` variant. Flash by double-tapping reset (the XIAO appears as a USB drive) and copying `zephyr/build/zephyr/zephyr.uf2` (in your workshop folder; `/project/zephyr/build/...` inside it) to it from the host. This avoids Workshop's USB passthrough, which Canonical lists as a known issue in 26.04.

Without Workshop, the same commands work with plain `west build -b <board> conference-badge -p`.

### 4b. Notes on the 2.13" panel

Commands are in section 1. This panel runs in landscape, 250x120 usable pixels (Zephyr drops 2 of the 122 rows, since it needs a multiple of 8), with Zephyr's own `ssd16xx` driver.

- `boards/panel_2in13.overlay` replaces the 4.26" panel node (driver, size, no mirroring); `boards/panel_2in13.conf` makes the firmware convert each frame to the layout `ssd16xx` wants.
- Check the FPC connector type and the jumper on the EN04 for this panel (Seeed wiki). If the image is mirrored or upside down, add `rotation = <180>;` or similar to the `&epd` node in `boards/panel_2in13.overlay`.
- **Untested:** this variant has not been compiled or run on hardware. The devicetree validates against the Zephyr bindings, and the generated 250x120 previews look right.

## 5. Send a page over USB (fire and forget)

Besides the pages built into the firmware, the badge can show any text or picture you send from a computer, with no rebuild and no reflash. The page is drawn and **not stored**: the e-paper keeps it without power, and the next KEY1/KEY2/KEY3 press goes back to the built-in pages.

```sh
source .venv/bin/activate
pip install pillow segno pyserial
python3 tools/send_badge.py --text "Back in 5 minutes"
python3 tools/send_badge.py --text "Room 2\nTalk starts at 14:00" --align left
python3 tools/send_badge.py --image logo.png
python3 tools/send_badge.py --image photo.jpg --dither floyd --fit cover
python3 tools/send_badge.py --image portrait.jpg --rotate 90 --fit cover   # portrait: turn the badge to read it
python3 tools/send_badge.py --text "Hello" --preview hello.png      # only writes a PNG, needs no badge
python3 tools/send_badge.py --info                                 # asks the badge its panel size
```

- The script asks the badge for its panel size (800x480 or 250x120) and draws the page at that size, so it works with either panel. `--preview` draws without a badge (`--size 250x120` for the small panel).
- Default port is `/dev/ttyACM0` (the same port as the serial console, so log lines and replies are mixed; the script ignores the logs). Use `--port` to change it. Close `picocom` or `screen` first: only one program can use the port.
- **"Permission denied" on the port:** add yourself to the `dialout` group (`sudo usermod -aG dialout $USER`) and log in again, or for the current terminal only run `newgrp dialout`. With `sudo`, use the virtual environment's Python: `sudo .venv/bin/python tools/send_badge.py ...`.
- `--rotate 90|180|270` turns the page: 90 puts the top of the page on the left side of the panel (portrait; turn the badge clockwise to read it). Use `--preview` to check the direction.
- The refresh takes several seconds. The script prints "received" when the badge has the data, then "done" when the panel has refreshed.
- If you send a second page before the first has finished refreshing, the badge answers "busy": wait and send again.
- **In the simulator:** `zephyr.exe` prints a line such as `uart connected to pseudotty: /dev/pts/5`; run `send_badge.py --port /dev/pts/5 --text "Hello"`.
- The protocol is documented in `src/remote.h` (a small framed packet with a CRC32), so another sender (a Python service, a phone, an LTE gateway) only has to write the same bytes to the serial port. LTE senders are not implemented; Bluetooth is in section 6.
- Status: the receiver logic was checked on a computer against a stand-in for the Zephyr APIs (good frame, bad CRC, wrong size, busy, timeout, resync after garbage) and runs in the simulator. Text, image and rotate on the real board's USB port have had little testing. If a send times out, check that no other program holds the port and look at the serial log.

## 6. Send a page over Bluetooth LE (from a phone or computer)

The same packets as on USB, carried by a GATT service that uses the Nordic UART Service UUIDs (defined in `src/ble.c`; Zephyr 4.4's own `bt_nus` has no security option). The page `tools/badge_ble.html` is the sender: pick text or a picture, see the exact preview, tap Send. It runs in the browser with Web Bluetooth: **Chrome or Edge on Android or desktop; not Safari / iPhone.** On Linux desktop Chrome, Web Bluetooth may need enabling in `chrome://flags`.

**Build the firmware with Bluetooth** (opt-in; the simulator has no Bluetooth):

```sh
# 4.26" panel
workshop run zephyr-26-04 -- build -p always -b xiao_ble/nrf52840 /project/conference-badge -- \
  -DEXTRA_CONF_FILE=ble.conf

# 2.13" panel (two conf files, separated by a semicolon)
workshop run zephyr-26-04 -- build -p always -b xiao_ble/nrf52840 /project/conference-badge -- \
  -DEXTRA_DTC_OVERLAY_FILE=boards/panel_2in13.overlay "-DEXTRA_CONF_FILE=boards/panel_2in13.conf;ble.conf"
```

**Use it**

1. Open the hosted page (see "Hosting the page" below) or `tools/badge_ble.html` in Chrome (a file on your phone, or `python3 -m http.server` in `tools/` on your computer and open `http://localhost:8000/badge_ble.html`).
2. Tap Connect and choose the device called `Badge`.
3. The first time, the phone or computer asks for a passkey: **123456** (see below). The page then reads the panel size from the badge.
4. Choose Text or Picture, check the preview, tap Send. The panel refreshes after a few seconds.

**Security: change the passkey.** The badge requires an authenticated link (`BT_GATT_PERM_WRITE_AUTHEN`) with the fixed passkey `CONFIG_BADGE_BLE_PASSKEY` (default **123456**, public in this repository). Change it before wearing the badge in public: add `-DCONFIG_BADGE_BLE_PASSKEY=654321` to the build, or edit the default in `Kconfig`. A fixed passkey is a deterrent for pranksters, not strong security. Without it (`CONFIG_BADGE_BLE_PASSKEY=0`), anyone in radio range can overwrite the page. Only one sender at a time (USB or BLE).

**Hosting the page (GitHub Pages).** Web Bluetooth only works on https, so on a phone the easiest way is to host the page. The repository contains `.github/workflows/pages.yml`, which publishes `tools/badge_ble.html` as the site's `index.html` (one file, no build, no secrets, nothing is sent to the server: the picture and the Bluetooth link stay in your browser).

1. Push to `main`.
2. In the GitHub repository: Settings, Pages, Source = **GitHub Actions** (once).
3. The workflow runs (Actions tab); the address is `https://<your-user>.github.io/<your-repo>/`.
4. Open it on the phone (Chrome on Android; **Bluefy** on iPhone, untested), and bookmark it.

If the repository is private, Pages needs a paid plan; otherwise use `python3 -m http.server` as above. Anyone who knows the address can open the page, but it can only talk to a badge within Bluetooth range that accepts their passkey.

**Own sender:** write the packets of `src/remote.h` to the RX characteristic (`6e400002-b5a3-f393-e0a9-e50e24dcca9e`) and read the reply lines from the TX characteristic (`6e400003-...`, notifications, in 20-byte pieces).

**Status:** the page was tested in a headless browser against a mock Bluetooth device, and the bytes it produced were accepted by the firmware's receiver code compiled on a computer (CRC and format). **Not tested: Zephyr's Bluetooth build, pairing, the real radio link, speed.** If a Kconfig symbol in `ble.conf` is rejected by your Zephyr, send the error.

## What is verified and what is not

Confirmed by running it:

- The simulator build and run (800x480 window, pages cycling).
- The 4.26" board build, flashing, and the test patterns on the real panel.
- The USB receiver against the simulator's pseudo-terminal, and its error paths on a computer (see section 5).

Checked offline only:

- The overlays parse and satisfy the real Zephyr bindings, including the new `seeed,ssd1677` binding (mipi-dbi-spi, gpio-keys, gpio-hog); the pins resolve through Zephyr's XIAO connector tables.
- `make_badge.py` and `send_badge.py` output: QR codes decode, the pixel packing round-trips (row by row, MSB = leftmost pixel, 1 = white), previews checked by eye.

Not verified:

- The **2.13" variant** (never compiled or run).
- `--rotate` and picture pages on the real panel, and how a photo looks after dithering.
- Idle power and battery life.
- **Bluetooth** (section 6): pairing, the real link, transfer speed, RAM use with the Bluetooth stack.
- The example `qr_image` URL in `badges.example.json` (api.qrserver.com): it needs network access and was never fetched.

## EN04 pin map (see `boards/xiao_ble.overlay`)

| Signal | XIAO pad | MCU pin | Source |
|---|---|---|---|
| CS | D7 | P1.12 | Seeed_GFX2 EN04 config |
| SCK | D8 | P1.13 | Seeed_GFX2, = Zephyr `xiao_spi` |
| MOSI | D10 | P1.15 | Seeed_GFX2, = Zephyr `xiao_spi` |
| BUSY | D3 | P0.29 | Seeed_GFX2 |
| Panel power | D6 | P1.11 | Seeed_GFX2 ("enable", active high) |
| DC | D16 | P0.31 | Seeed_GFX2 comment: "D16 (raw GPIO 35)"; works on the real panel |
| RST | D11 | P0.15 | Seeed_GFX2 comment: "D11 (raw GPIO 30)"; works on the real panel |
| KEY1/2/3 | D1/D2/D4 | P0.03/P0.28/P0.04 | Seeed wiki |

The "raw GPIO" numbers in Seeed's library are Arduino-core indices, so the overlay uses the D-names. If the panel stays blank on another board revision, check DC/RST against the EN04 schematic (Seeed wiki, Resources) first.

The overlay disables `uart0` and `i2c1` because the EN04 reuses their pins.

## Known gaps

- **Power:** the driver puts the SSD1677 in deep sleep after each refresh, but the panel supply (D6) is held on by a gpio-hog. Measure idle current before promising battery life.
- **Battery gauge:** not implemented. The EN04 has its own divider (A0 = P0.02, enable on A5 = P0.05; Seeed's Arduino example uses a x7.16 factor).
- **Panel tuning:** the driver uses the controller's built-in waveform (OTP) and Seeed's init values. No temperature compensation beyond the internal sensor (command 0x18).
- **Built-in pages** change only by regenerating the header and re-flashing. Pages sent over USB are not stored (lost at the next key press or reset of the content shown; the panel itself keeps what it displays). LTE senders are not implemented; Bluetooth is optional (section 6).

## Debugging the display

Build with `-DCONFIG_BADGE_TEST_PATTERN=y` to show four test pages instead of the badge: all white, all black, an orientation marker (big black square top-left, smaller one top-right, border, diagonal) and 16 px vertical stripes. KEY1/KEY2 step through them. The serial console (`/dev/ttyACM0`, 115200) prints which pattern is shown. Test patterns and USB pages can coexist: a received page replaces the pattern until the next key press.

```sh
workshop run zephyr-26-04 -- build -p always -b xiao_ble/nrf52840 /project/conference-badge -- -DCONFIG_BADGE_TEST_PATTERN=y
```

### If the screen is wrong

| Symptom | Likely cause | Change |
|---|---|---|
| Image correct but mirrored left-right | glass orientation differs from Seeed's config | remove `mirror-x;` from `boards/xiao_ble.overlay` |
| Black and white swapped | polarity | in `src/ssd1677.c`, invert the data before writing it to RAM (command 0x24) |
| Image upside down | gate scan direction | change driver output byte `0x02` (command 0x01) or data entry mode `0x03` in `ssd1677_wake()` |
| Nothing changes, log says "BUSY stuck high" | BUSY pin, panel power or reset | check D3 (BUSY), D6 (power hog), RST |
| Noise or garbage | wrong controller or panel size | check the panel reference against the table in section 1, and the serial log |
