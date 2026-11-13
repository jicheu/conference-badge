# HOWTO: conference badge in 5 minutes

Short recipes. Details, pin maps and troubleshooting are in `README.md`.

All commands run from the project folder (the one containing `tools/` and `boards/`). The firmware is built in the Canonical Workshop (`zephyr-26-04`), where the project is mounted at `/project/conference-badge`.

## 0. One-time setup

```sh
python3 -m venv .venv                      # Ubuntu refuses a system-wide pip install
source .venv/bin/activate                  # repeat in every new terminal
pip install pillow segno pyserial
```

For the real board, the Arm toolchain must be in `.workshop/zephyr-26-04.yaml` (see README section 4): the `zephyr-arm` SDK and its `zephyr-sdk-ng:arm` connection, then `workshop refresh zephyr-26-04`.

## 1. Create your badge pages (JSON)

```sh
cp badges.example.json badges.json         # 2.13" panel: cp badges.2in13.json badges.json
```

Edit `badges.json`. It is a list of pages, shown in order:

```json
{
  "pages": [
    { "type": "card", "name": "Alex Example", "title": "Principal Engineer",
      "company": "Example Corp", "qr": "https://example.com/alex" },
    { "type": "qr", "qr": "https://example.com/alex",
      "caption": "Scan to connect", "footer": "example.com/alex" },
    { "type": "text", "text": "Ask me about\nZephyr" },
    { "type": "image", "image": "logo.png", "dither": "floyd" }
  ]
}
```

| Page `type` | Fields |
|---|---|
| `card` | `name` (required), `title`, `company`, and a QR: `qr` (text/URL) **or** `qr_image` (picture or URL of a QR code) |
| `qr` | a QR as above, plus `caption`, `footer` |
| `text` | `text` (`\n` = new line), `align` (`center`/`left`/`right`), `bold` |
| `image` | `image` (file or URL), `fit` (`contain`/`cover`), `dither` (`threshold` for logos, `floyd` for photos) |

Panel size: add `"width": 250, "height": 120` at the top level for the 2.13" panel. Leave it out for the 4.26" (800x480).

Generate the bitmaps and the previews:

```sh
python3 tools/make_badge.py badges.json -o src/badge_images.h --preview preview
```

Open `preview/page0.png`, `page1.png`, ... and scan the QR codes with your phone **before** building. Every time you change `badges.json` or switch panel, run this again and rebuild.

## 2. Build and flash

Always use a clean build (`-p always`) when you change panel or board.

### Simulator (no board)

```sh
workshop run zephyr-26-04 -- build -p always -b native_sim/native/64 /project/conference-badge
./zephyr/build/zephyr/zephyr.exe           # on the host, in a terminal with a display
```

An 800x480 window opens and cycles through the pages every 4 s. Stop with Ctrl+C. It prints a serial port line (`uart connected to pseudotty: /dev/pts/N`) used in section 3.

(If you start it with `workshop run ... build -t run` and it crashes with an X `BadShmSeg` error, run the binary on the host as above.)

### Board with the 4.26" panel (800x480, default)

```sh
python3 tools/make_badge.py badges.json -o src/badge_images.h --preview preview
workshop run zephyr-26-04 -- build -p always -b xiao_ble/nrf52840 /project/conference-badge
```

### Board with the 2.13" panel (250x120)

```sh
python3 tools/make_badge.py badges.2in13.json -o src/badge_images.h --preview preview
workshop run zephyr-26-04 -- build -p always -b xiao_ble/nrf52840 /project/conference-badge -- \
  -DEXTRA_DTC_OVERLAY_FILE=boards/panel_2in13.overlay -DEXTRA_CONF_FILE=boards/panel_2in13.conf
```

The JSON size, the generated header and the build must all match the panel. If they don't, nothing is drawn and the serial log says `display layout does not match`. (The 2.13" variant has not been tested on hardware yet.)

### Flash the board

1. Double-tap the reset button of the XIAO: it appears as a USB drive.
2. Copy `zephyr/build/zephyr/zephyr.uf2` (in your workshop folder) to that drive.
3. The panel refreshes after a few seconds. KEY1 = previous page, KEY2 = next page, KEY3 = redraw.

Check the screen first with test patterns by adding `-DCONFIG_BADGE_TEST_PATTERN=y` to the build command (README, "Debugging the display").

## 3. Send extra text or a picture over USB

No rebuild, no reflash. The page is shown and not stored: the next key press goes back to your built-in pages.

```sh
python3 tools/send_badge.py --info                                   # check the link, prints the panel size
python3 tools/send_badge.py --text "Back in 5 minutes"
python3 tools/send_badge.py --text "Room 2\nTalk at 14:00" --align left
python3 tools/send_badge.py --image logo.png
python3 tools/send_badge.py --image photo.jpg --dither floyd --fit cover
python3 tools/send_badge.py --image portrait.jpg --rotate 90 --fit cover   # portrait
python3 tools/send_badge.py --image photo.jpg --preview /tmp/test.png      # just a preview, no badge needed
```

- `--rotate 90`: top of the page on the left side of the panel (turn the badge clockwise to read it); `270`: top on the right; `180`: upside down.
- Add `--port /dev/ttyACM1` if the badge is not `/dev/ttyACM0`. Add `--port /dev/pts/N` to talk to the simulator.
- Close any serial terminal (`picocom`, `screen`) first.
- **Permission denied?** Run `newgrp dialout` once in your terminal (after `sudo usermod -aG dialout $USER`), or use `sudo .venv/bin/python tools/send_badge.py ...`.
- The refresh takes several seconds. Wait for "done" before sending the next page.

## 4. Send from a phone over Bluetooth

Build with Bluetooth (add `-DEXTRA_CONF_FILE=ble.conf` to the board build; for the 2.13" panel use `"-DEXTRA_CONF_FILE=boards/panel_2in13.conf;ble.conf"`), flash, then open the hosted page (`https://<your-user>.github.io/<your-repo>/`, built with `tools/publish_page.sh`, see README "Hosting the page") or `tools/badge_ble.html` in Chrome or Edge (Android or desktop; iPhone: Bluefy app):

1. Connect, choose the device `Badge`, enter the passkey (default **123456**: change it with `-DCONFIG_BADGE_BLE_PASSKEY=654321`).
2. Type text or pick a picture, check the preview, Send.

Details and security notes: README section 6. Bluetooth has not been tested on real hardware yet.

## 5. NFC: tap to open (new, untested on hardware)

Add `"nfc_url": "https://example.com/alex"` at the top of your JSON (max about 40 characters after `https://`), run `make_badge.py`, build with `-DEXTRA_CONF_FILE=nfc.conf` (`"-DEXTRA_CONF_FILE=ble.conf;nfc.conf"` with Bluetooth), flash. Tapping a phone on the badge opens the link. Needs an NFC antenna on pads P0.09 / P0.10. Details: README section 7.

## Quick reference

| I want to... | Do this |
|---|---|
| change my name or title | edit `badges.json`, run `make_badge.py`, rebuild, flash |
| add a photo or logo as a built-in page | add an `image` page, then as above |
| let people tap the badge to open my link | `nfc_url` in the JSON + `nfc.conf`, section 5 |
| show a one-off message | `send_badge.py --text "..."` (USB) or `badge_ble.html` (phone) |
| switch to the other panel | other JSON size, regenerate, other build command, `-p always` |
| check it without hardware | simulator, section 2 |
