/*
 * Reusable e-paper conference badge (Zephyr).
 *
 * Shows one of several pre-rendered 296x128 pages (see tools/make_badge.py).
 *   KEY1 = previous page, KEY2 = next page, KEY3 = redraw current page
 *   (a full refresh clears any ghosting).
 *
 * The panel keeps its image with no power, so nothing runs between key presses.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "badge_images.h"

LOG_MODULE_REGISTER(badge, LOG_LEVEL_INF);

#define DEBOUNCE_MS 60

static const struct device *const epd = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

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

	/*
	 * make_badge.py packs pixels as 1 = white, vertical 8-pixel tiles, MSB at the
	 * top. Refuse to draw if the driver would interpret the buffer differently.
	 */
	if (caps.x_resolution != BADGE_WIDTH || caps.y_resolution != BADGE_HEIGHT ||
	    !(caps.screen_info & SCREEN_INFO_MONO_VTILED) ||
	    !(caps.screen_info & SCREEN_INFO_MONO_MSB_FIRST) ||
	    caps.current_pixel_format != PIXEL_FORMAT_MONO01) {
		LOG_ERR("display layout does not match badge images (%dx%d, MONO01, VTILED, MSB)",
			BADGE_WIDTH, BADGE_HEIGHT);
		return -EINVAL;
	}

	return 0;
}

static int show_page(size_t index)
{
	const struct badge_page *page = &badge_pages[index];
	const struct display_buffer_descriptor desc = {
		.buf_size = BADGE_BUF_SIZE,
		.width = BADGE_WIDTH,
		.height = BADGE_HEIGHT,
		.pitch = BADGE_WIDTH,
	};
	int err;

	LOG_INF("showing page %zu/%zu: %s", index + 1, (size_t)BADGE_PAGE_COUNT, page->name);

	/*
	 * Blanking on: the driver only loads the frame into the controller RAM.
	 * Blanking off: it triggers one full refresh and waits for BUSY to clear.
	 */
	(void)display_blanking_on(epd);

	err = display_write(epd, 0, 0, &desc, page->data);
	if (err) {
		LOG_ERR("display_write failed (%d)", err);
	}

	(void)display_blanking_off(epd);

	return err;
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

#if CONFIG_BADGE_AUTO_CYCLE_MS > 0
	k_timer_start(&cycle_timer, K_MSEC(CONFIG_BADGE_AUTO_CYCLE_MS),
		      K_MSEC(CONFIG_BADGE_AUTO_CYCLE_MS));
#endif

	while (true) {
		int action;

		k_msgq_get(&key_events, &action, K_FOREVER);

		if (action != 0) {
			page = (size_t)(((int)page + BADGE_PAGE_COUNT + action) % BADGE_PAGE_COUNT);
		}

		(void)show_page(page);

		/* Ignore presses that piled up while the panel was refreshing. */
		k_msgq_purge(&key_events);
	}

	return 0;
}
