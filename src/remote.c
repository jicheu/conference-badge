/* See remote.h. UNTESTED on hardware when this file was written. */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include "remote.h"

LOG_MODULE_REGISTER(remote, LOG_LEVEL_INF);

#define RX_TIMEOUT_MS 2000 /* a frame must keep flowing: no byte for this long aborts it */

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static struct remote_cfg cfg;
static atomic_t busy; /* 1 from "frame committed" until remote_shown() */

K_THREAD_STACK_DEFINE(rx_stack, 2048);
static struct k_thread rx_thread_data;

enum rx_state { ST_SYNC, ST_CMD, ST_LEN, ST_PAYLOAD, ST_CRC };

static const char magic[4] = {'B', 'D', 'G', '1'};

static void reply(const char *what, const char *text)
{
	/* Leading newline: keeps the line intact even if a log line was cut off before it. */
	printk("\nBADGE %s %s\n", what, text);
}

static void reply_info(void)
{
	printk("\nBADGE OK info %ux%u row=%u size=%u\n", cfg.width, cfg.height,
	       (unsigned int)((cfg.width + 7) / 8), (unsigned int)cfg.size);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void rx_thread(void *a, void *b, void *c)
{
	enum rx_state st = ST_SYNC;
	uint8_t cmd = 0, hdr[4];
	size_t pos = 0, sync = 0;
	uint32_t len = 0;
	bool skip = false;    /* consume the payload without storing it */
	const char *deferred_err = NULL;
	int64_t last_byte = k_uptime_get();

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		unsigned char ch;

		if (uart_poll_in(uart, &ch) != 0) {
			if (st != ST_SYNC && k_uptime_get() - last_byte > RX_TIMEOUT_MS) {
				reply("ERR", "timeout");
				st = ST_SYNC;
				sync = 0;
			}
			k_msleep(1);
			continue;
		}
		last_byte = k_uptime_get();

		switch (st) {
		case ST_SYNC:
			if (ch == magic[sync]) {
				sync++;
			} else {
				sync = (ch == magic[0]) ? 1 : 0;
			}
			if (sync == sizeof(magic)) {
				st = ST_CMD;
				sync = 0;
			}
			break;

		case ST_CMD:
			cmd = ch;
			pos = 0;
			st = ST_LEN;
			break;

		case ST_LEN:
			hdr[pos++] = ch;
			if (pos < 4) {
				break;
			}
			len = le32(hdr);
			pos = 0;
			skip = false;
			deferred_err = NULL;
			if (cmd == 'I' && len == 0) {
				st = ST_CRC;
			} else if (cmd == 'F' && len == cfg.size) {
				if (atomic_get(&busy)) {
					skip = true;
					deferred_err = "busy (previous frame still refreshing)";
				}
				st = ST_PAYLOAD;
			} else if (len <= cfg.size) {
				skip = true; /* unknown command or wrong size: swallow it, then report */
				deferred_err = (cmd == 'F') ? "wrong frame size (use INFO to get it)"
							     : "unknown command";
				st = len ? ST_PAYLOAD : ST_CRC;
			} else {
				reply("ERR", "length too large");
				st = ST_SYNC;
			}
			break;

		case ST_PAYLOAD:
			if (!skip) {
				cfg.buf[pos] = ch;
			}
			if (++pos == len) {
				pos = 0;
				st = ST_CRC;
			}
			break;

		case ST_CRC:
			hdr[pos++] = ch;
			if (pos < 4) {
				break;
			}
			pos = 0;
			st = ST_SYNC;

			if (deferred_err) {
				reply("ERR", deferred_err);
			} else if (cmd == 'I') {
				reply_info();
			} else if (crc32_ieee(cfg.buf, cfg.size) != le32(hdr)) {
				reply("ERR", "bad CRC (resend)");
			} else {
				int action = cfg.action;

				atomic_set(&busy, 1);
				reply("OK", "received");
				if (k_msgq_put(cfg.events, &action, K_NO_WAIT) != 0) {
					atomic_set(&busy, 0);
					reply("ERR", "badge busy, try again");
				}
			}
			break;
		}
	}
}

void remote_shown(int err)
{
	if (err) {
		reply("ERR", "display refresh failed");
	} else {
		reply("OK", "shown");
	}
	atomic_set(&busy, 0);
}

void remote_start(const struct remote_cfg *c)
{
	cfg = *c;

	if (!device_is_ready(uart)) {
		LOG_ERR("console UART not ready, remote pages disabled");
		return;
	}

	k_thread_create(&rx_thread_data, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack), rx_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread_data, "remote_rx");
	LOG_INF("remote pages: send a frame over the serial port (tools/send_badge.py)");
}
