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
#if CONFIG_BADGE_BLE
#include "ble.h"
#endif

LOG_MODULE_REGISTER(remote, LOG_LEVEL_INF);

#define RX_TIMEOUT_MS 2000 /* a frame must keep flowing: no byte for this long aborts it */

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static struct remote_cfg cfg;
static atomic_t busy; /* 1 from "frame committed" until remote_shown() */

K_THREAD_STACK_DEFINE(rx_stack, 2048);
static struct k_thread rx_thread_data;

enum rx_state { ST_SYNC, ST_CMD, ST_LEN, ST_PAYLOAD, ST_CRC };

static const char magic[4] = {'B', 'D', 'G', '1'};

/* Replies go to the serial console and, when Bluetooth is on, to subscribed BLE peers. */
static void emit(const char *line)
{
	printk("\n%s\n", line); /* leading newline: keeps the line intact after a cut-off log line */
#if CONFIG_BADGE_BLE
	ble_send_line(line);
#endif
}

static void reply(const char *what, const char *text)
{
	char line[96];

	snprintk(line, sizeof(line), "BADGE %s %s", what, text);
	emit(line);
}

static void reply_info(void)
{
	char line[96];

	snprintk(line, sizeof(line), "BADGE OK info %ux%u row=%u size=%u", cfg.width, cfg.height,
		 (unsigned int)((cfg.width + 7) / 8), (unsigned int)cfg.size);
	emit(line);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

struct parser {
	enum rx_state st;
	uint8_t cmd;
	uint8_t hdr[4];
	size_t pos;
	size_t sync;
	uint32_t len;
	bool skip; /* consume the payload without storing it */
	const char *deferred_err;
	int64_t last_byte;
};

static struct parser uart_parser, ble_parser;

static void parser_timeout(struct parser *p)
{
	if (p->st != ST_SYNC && k_uptime_get() - p->last_byte > RX_TIMEOUT_MS) {
		reply("ERR", "timeout");
		p->st = ST_SYNC;
		p->sync = 0;
	}
}

static void parse_byte(struct parser *p, uint8_t ch)
{
	p->last_byte = k_uptime_get();

	switch (p->st) {
	case ST_SYNC:
		if (ch == magic[p->sync]) {
			p->sync++;
		} else {
			p->sync = (ch == magic[0]) ? 1 : 0;
		}
		if (p->sync == sizeof(magic)) {
			p->st = ST_CMD;
			p->sync = 0;
		}
		break;

	case ST_CMD:
		p->cmd = ch;
		p->pos = 0;
		p->st = ST_LEN;
		break;

	case ST_LEN:
		p->hdr[p->pos++] = ch;
		if (p->pos < 4) {
			break;
		}
		p->len = le32(p->hdr);
		p->pos = 0;
		p->skip = false;
		p->deferred_err = NULL;
		if (p->cmd == 'I' && p->len == 0) {
			p->st = ST_CRC;
		} else if (p->cmd == 'F' && p->len == cfg.size) {
			if (atomic_get(&busy)) {
				p->skip = true;
				p->deferred_err = "busy (previous frame still refreshing)";
			}
			p->st = ST_PAYLOAD;
		} else if (p->len <= cfg.size) {
			p->skip = true; /* unknown command or wrong size: swallow it, then report */
			p->deferred_err = (p->cmd == 'F') ? "wrong frame size (use INFO to get it)"
							  : "unknown command";
			p->st = p->len ? ST_PAYLOAD : ST_CRC;
		} else {
			reply("ERR", "length too large");
			p->st = ST_SYNC;
		}
		break;

	case ST_PAYLOAD:
		if (!p->skip) {
			cfg.buf[p->pos] = ch;
		}
		if (++p->pos == p->len) {
			p->pos = 0;
			p->st = ST_CRC;
		}
		break;

	case ST_CRC:
		p->hdr[p->pos++] = ch;
		if (p->pos < 4) {
			break;
		}
		p->pos = 0;
		p->st = ST_SYNC;

		if (p->deferred_err) {
			reply("ERR", p->deferred_err);
		} else if (p->cmd == 'I') {
			reply_info();
		} else if (crc32_ieee(cfg.buf, cfg.size) != le32(p->hdr)) {
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

/* Bluetooth: called from the BLE stack with each GATT write (see ble.c). */
void remote_feed_ble(const uint8_t *data, size_t len)
{
	parser_timeout(&ble_parser);

	for (size_t i = 0; i < len; i++) {
		parse_byte(&ble_parser, data[i]);
	}
}

static void rx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uart_parser.last_byte = k_uptime_get();

	while (true) {
		unsigned char ch;

		if (uart_poll_in(uart, &ch) != 0) {
			parser_timeout(&uart_parser);
			k_msleep(1);
			continue;
		}
		parse_byte(&uart_parser, ch);
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

#if CONFIG_BADGE_BLE
	ble_start();
#endif

	if (!device_is_ready(uart)) {
		LOG_ERR("console UART not ready, serial remote pages disabled");
		return;
	}

	k_thread_create(&rx_thread_data, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack), rx_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread_data, "remote_rx");
	LOG_INF("remote pages: send a frame over the serial port (tools/send_badge.py)");
}
