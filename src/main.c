/*
 * Reusable e-paper conference badge (Zephyr).
 *
 * Shows one of several pre-rendered pages (800x480 for the 4.26" panel) (see tools/make_badge.py).
 *   KEY1 = previous page, KEY2 = next page, KEY3 = redraw current page
 *   (a full refresh clears any ghosting).
 *
 * The panel keeps its image with no power, so nothing runs between key presses.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "badge_images.h"
#if CONFIG_BADGE_NFC
#include "nfc.h"
#endif
#if CONFIG_BADGE_REMOTE
#include "remote.h"
#endif

LOG_MODULE_REGISTER(badge, LOG_LEVEL_INF);

#define DEBOUNCE_MS 60
#define ACTION_REMOTE 100 /* a page was received over serial */

static const struct device *const epd = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

#if CONFIG_BADGE_PANEL_VTILED
/*
 * Panels driven by Zephyr's ssd16xx driver (2.13") want MONO10 (1 = black) in vertical
 * 8-pixel tiles. The badge bitmaps are row by row with 1 = white, so each frame is
 * converted just before it is sent.
 */
static uint8_t frame_buf[BADGE_WIDTH * BADGE_HEIGHT / 8];

static void convert_frame(const uint8_t *src)
{
	const size_t row_bytes = BADGE_ROW_BYTES;

	memset(frame_buf, 0, sizeof(frame_buf));

	for (size_t y = 0; y < BADGE_HEIGHT; y++) {
		for (size_t x = 0; x < BADGE_WIDTH; x++) {
			bool white = src[y * row_bytes + x / 8] & (0x80 >> (x % 8));

			if (!white) {
				frame_buf[(y / 8) * BADGE_WIDTH + x] |= (uint8_t)(0x80 >> (y % 8));
			}
		}
	}
}
#endif

#if CONFIG_BADGE_REMOTE
/* Holds a page received over serial until it has been drawn. */
static uint8_t remote_buf[BADGE_BUF_SIZE];
#endif

/* Key actions are queued and handled in main(), since a refresh takes seconds. */
K_MSGQ_DEFINE(key_events, sizeof(int), 4, sizeof(int));

#if CONFIG_BADGE_AUTO_CYCLE_MS > 0
/* Simulator: no buttons, so a timer posts "next page" instead. */
static void cycle_expired(struct k_timer *timer)
{
	int action = +1;

	ARG_UNUSED(timer);
	(void)k_msgq_put(&key_events, &action, K_NO_WAIT);
}

K_TIMER_DEFINE(cycle_timer, cycle_expired, NULL);
#endif

struct key {
	struct gpio_dt_spec gpio;
	struct gpio_callback cb;
	struct k_work_delayable debounce;
	int action; /* -1 previous page, +1 next page, 0 redraw */
};

#define KEY_INIT(alias, act) { .gpio = GPIO_DT_SPEC_GET(DT_ALIAS(alias), gpios), .action = (act) }

static struct key keys[] = {
	KEY_INIT(sw0, -1),
	KEY_INIT(sw1, +1),
	KEY_INIT(sw2, 0),
};

static void key_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct key *k = CONTAINER_OF(cb, struct key, cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	k_work_reschedule(&k->debounce, K_MSEC(DEBOUNCE_MS));
}

static void key_debounced(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct key *k = CONTAINER_OF(dwork, struct key, debounce);

	/* Still pressed after the debounce delay: it was a real press. */
	if (gpio_pin_get_dt(&k->gpio) > 0) {
		int action = k->action;

		(void)k_msgq_put(&key_events, &action, K_NO_WAIT);
	}
}

static int keys_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		struct key *k = &keys[i];
		int err;

		if (!gpio_is_ready_dt(&k->gpio)) {
			LOG_ERR("key %zu: GPIO port not ready", i);
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&k->gpio, GPIO_INPUT);
		if (err) {
			LOG_ERR("key %zu: configure failed (%d)", i, err);
			return err;
		}

		k_work_init_delayable(&k->debounce, key_debounced);
		gpio_init_callback(&k->cb, key_isr, BIT(k->gpio.pin));

		err = gpio_add_callback(k->gpio.port, &k->cb);
		if (err) {
			LOG_ERR("key %zu: add callback failed (%d)", i, err);
			return err;
		}

		err = gpio_pin_interrupt_configure_dt(&k->gpio, GPIO_INT_EDGE_TO_ACTIVE);
		if (err) {
			LOG_ERR("key %zu: interrupt failed (%d)", i, err);
			return err;
		}
	}

	return 0;
}

static int check_display(void)
{
	struct display_capabilities caps;

	if (!device_is_ready(epd)) {
		LOG_ERR("display %s not ready (check DC/RST/BUSY/CS pins and panel power)",
			epd->name);
		return -ENODEV;
	}

	display_get_capabilities(epd, &caps);

	LOG_INF("display %ux%u, screen_info 0x%x, pixel format 0x%x", caps.x_resolution,
		caps.y_resolution, caps.screen_info, caps.current_pixel_format);

#if CONFIG_BADGE_PANEL_VTILED
	/* ssd16xx: MONO10, vertical tiles, MSB first. */
	bool layout_ok = (caps.screen_info & SCREEN_INFO_MONO_VTILED) &&
			 caps.current_pixel_format == PIXEL_FORMAT_MONO10;
#else
	/* ssd1677 driver and simulator: MONO01, rows, MSB first. */
	bool layout_ok = !(caps.screen_info & SCREEN_INFO_MONO_VTILED) &&
			 caps.current_pixel_format == PIXEL_FORMAT_MONO01;
#endif

	if (caps.x_resolution != BADGE_WIDTH || caps.y_resolution != BADGE_HEIGHT || !layout_ok ||
	    !(caps.screen_info & SCREEN_INFO_MONO_MSB_FIRST)) {
		LOG_ERR("display layout does not match badge images (%dx%d, vtiled=%d, MSB)",
			BADGE_WIDTH, BADGE_HEIGHT, IS_ENABLED(CONFIG_BADGE_PANEL_VTILED));
		return -EINVAL;
	}

	return 0;
}

#if CONFIG_BADGE_TEST_PATTERN
#define NUM_PAGES 4U

static uint8_t pattern_buf[BADGE_BUF_SIZE];

/* Same layout as the badge bitmaps: rows of BADGE_ROW_BYTES bytes, MSB = leftmost pixel, 1 = white. */
static void pattern_pixel(int x, int y, bool black)
{
	if (x < 0 || y < 0 || x >= BADGE_WIDTH || y >= BADGE_HEIGHT) {
		return;
	}

	uint8_t *b = &pattern_buf[y * BADGE_ROW_BYTES + x / 8];
	uint8_t mask = (uint8_t)(0x80 >> (x % 8));

	if (black) {
		*b &= (uint8_t)~mask;
	} else {
		*b |= mask;
	}
}

static const char *make_pattern(size_t idx)
{
	memset(pattern_buf, idx == 1 ? 0x00 : 0xFF, sizeof(pattern_buf));

	if (idx == 2) {
		/* 6 px border, a 40x40 black square in the top-left corner (x=0,y=0),
		 * a 40x16 one at the top-right, and a diagonal: shows orientation and edges. */
		for (int y = 0; y < BADGE_HEIGHT; y++) {
			for (int x = 0; x < BADGE_WIDTH; x++) {
				bool border = x < 6 || y < 6 || x >= BADGE_WIDTH - 6 ||
					      y >= BADGE_HEIGHT - 6;
				bool corner = x < 120 && y < 120;
				bool corner2 = x >= BADGE_WIDTH - 120 && y < 48;
				bool diag = (x * BADGE_HEIGHT / BADGE_WIDTH) == y;

				pattern_pixel(x, y, border || corner || corner2 || diag);
			}
		}
		return "orientation marker";
	}

	if (idx == 3) {
		for (int y = 0; y < BADGE_HEIGHT; y++) {
			for (int x = 0; x < BADGE_WIDTH; x++) {
				pattern_pixel(x, y, (x / 16) % 2 == 0);
			}
		}
		return "vertical stripes 16 px";
	}

	return idx == 0 ? "all white" : "all black";
}
#else
#define NUM_PAGES ((size_t)BADGE_PAGE_COUNT)
#endif

static int draw_frame(const char *page_name, const uint8_t *page_data)
{
	const struct display_buffer_descriptor desc = {
#if CONFIG_BADGE_PANEL_VTILED
		.buf_size = sizeof(frame_buf),
#else
		.buf_size = BADGE_BUF_SIZE,
#endif
		.width = BADGE_WIDTH,
		.height = BADGE_HEIGHT,
		.pitch = BADGE_WIDTH,
	};
	int err;

	LOG_INF("showing %s", page_name);

	/*
	 * Blanking on: nothing to do. display_write() loads the frame into the controller RAM,
	 * display_blanking_off() runs one full refresh and waits for BUSY to clear.
	 */
	(void)display_blanking_on(epd);

#if CONFIG_BADGE_PANEL_VTILED
	convert_frame(page_data);
	page_data = frame_buf;
#endif

	err = display_write(epd, 0, 0, &desc, page_data);
	if (err) {
		LOG_ERR("display_write failed (%d)", err);
	}

	(void)display_blanking_off(epd);

	return err;
}

static int show_page(size_t index)
{
#if CONFIG_BADGE_TEST_PATTERN
	const char *page_name = make_pattern(index);
	const uint8_t *page_data = pattern_buf;
#else
	const char *page_name = badge_pages[index].name;
	const uint8_t *page_data = badge_pages[index].data;
#endif
	char label[64];

	snprintk(label, sizeof(label), "page %zu/%zu: %s", index + 1, (size_t)NUM_PAGES, page_name);

	return draw_frame(label, page_data);
}

int main(void)
{
	size_t page = 0;
	int err;

	err = check_display();
	if (err) {
		return err;
	}

	err = keys_init();
	if (err) {
		return err;
	}

	(void)show_page(page);

#if CONFIG_BADGE_REMOTE
	remote_start(&(const struct remote_cfg){
		.buf = remote_buf,
		.size = sizeof(remote_buf),
		.width = BADGE_WIDTH,
		.height = BADGE_HEIGHT,
		.events = &key_events,
		.action = ACTION_REMOTE,
	});
#endif

#if CONFIG_BADGE_NFC
	(void)nfc_start(); /* the badge works without it; the error is logged */
#endif

#if CONFIG_BADGE_AUTO_CYCLE_MS > 0
	k_timer_start(&cycle_timer, K_MSEC(CONFIG_BADGE_AUTO_CYCLE_MS),
		      K_MSEC(CONFIG_BADGE_AUTO_CYCLE_MS));
#endif

	while (true) {
		int action;

		k_msgq_get(&key_events, &action, K_FOREVER);

#if CONFIG_BADGE_REMOTE
		if (action == ACTION_REMOTE) {
			int derr = draw_frame("remote page", remote_buf);

			/* Purge before releasing the buffer, so a new frame's event is not lost. */
			k_msgq_purge(&key_events);
			remote_shown(derr);
			continue;
		}
#endif

		if (action != 0) {
			page = (size_t)(((int)page + (int)NUM_PAGES + action) % (int)NUM_PAGES);
		}

		(void)show_page(page);

		/* Ignore presses that piled up while the panel was refreshing. */
		k_msgq_purge(&key_events);
	}

	return 0;
}
