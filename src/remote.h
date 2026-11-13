/*
 * "Fire and forget" remote pages: a host sends one frame over the console serial port
 * (USB CDC on the board, a pseudo-terminal in the simulator) or over Bluetooth LE (Nordic UART
 * Service, see ble.c); the badge shows it. Same packets on both.
 * Nothing is stored: the page stays on the e-paper (which keeps it without power) until
 * a key is pressed. Host side: tools/send_badge.py.
 *
 * Protocol (host to badge, all integers little endian):
 *   "BDG1" | cmd (1 byte) | payload length (u32) | payload | CRC32 of payload (u32)
 *   cmd 'I' (info, empty payload): reply "BADGE OK info <w>x<h> row=<bytes> size=<bytes>"
 *   cmd 'F' (frame): payload = one full page, row by row, MSB = leftmost pixel, 1 = white,
 *           (width + 7) / 8 bytes per row: the same bytes as a page in badge_images.h.
 *           replies "BADGE OK received", then "BADGE OK shown" once the panel has refreshed.
 * Errors: "BADGE ERR <reason>". Replies are text lines, mixed with the normal log output.
 */
#ifndef REMOTE_H_
#define REMOTE_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

struct remote_cfg {
	uint8_t *buf;          /* frame buffer, size bytes, owned by the caller */
	size_t size;           /* bytes in one frame */
	uint16_t width;
	uint16_t height;
	struct k_msgq *events; /* receives an int: `action` when a frame is ready */
	int action;
};

/* Start the receiver thread. */
void remote_start(const struct remote_cfg *cfg);

/* Feed bytes received over Bluetooth (called by ble.c). One sender at a time, USB or BLE. */
void remote_feed_ble(const uint8_t *data, size_t len);

/* Call once the frame in cfg->buf has been drawn (err = display result): releases the buffer. */
void remote_shown(int err);

#endif /* REMOTE_H_ */
