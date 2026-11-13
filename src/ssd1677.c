/*
 * Minimal out-of-tree Zephyr driver for the SSD1677 e-paper controller
 * (Seeed 4.26" 800x480 monochrome panel). Full refresh only.
 *
 * Command sequence taken from Seeed_GFX2 (src/driver/epaper/Driver_SSD1677.cpp):
 *   reset, 0x12 soft reset, 0x0C booster, 0x3C border, 0x01 driver output,
 *   0x11 data entry (X+ Y+), 0x44/0x45 RAM window, 0x18 internal temperature
 *   sensor, 0x4E/0x4F RAM counters, 0x24 RAM write, 0x22=0xF7 + 0x20 refresh,
 *   0x10=0x01 deep sleep.
 *
 * Differences from the ssd16xx driver (and why it cannot drive this panel):
 *   - X addresses (0x44, 0x4E) are in pixels, 16 bit, not in bytes.
 *   - 800x480 needs 16-bit window parameters.
 *
 * Behaviour: display_write() loads the frame into the controller RAM,
 * display_blanking_off() runs one full refresh, waits for BUSY and puts the
 * controller in deep sleep. Deep sleep is left through the reset pin, so every
 * frame starts with a hardware reset and the full init sequence.
 *
 * Pixel format reported: MONO01 (1 = white), rows of width/8 bytes, MSB = leftmost
 * pixel. Only whole-screen writes (x = 0, y = 0, full width and height) are accepted.
 *
 * UNTESTED on hardware when this file was written.
 */

#define DT_DRV_COMPAT seeed_ssd1677

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ssd1677, LOG_LEVEL_INF);

#define SSD1677_BUSY_POLL_MS   10
#define SSD1677_BUSY_TIMEOUT_MS 30000
#define SSD1677_BAND_ROWS      120 /* rows per SPI transfer: keeps each one far below EasyDMA limits */

struct ssd1677_config {
	const struct device *mipi_dev;
	struct mipi_dbi_config dbi_config;
	struct gpio_dt_spec busy_gpio;
	uint16_t width;
	uint16_t height;
	bool mirror_x;
	uint8_t *band;
};

struct ssd1677_data {
	bool awake;
	bool loaded;
};

static int ssd1677_busy_wait(const struct device *dev)
{
	const struct ssd1677_config *cfg = dev->config;
	int64_t deadline = k_uptime_get() + SSD1677_BUSY_TIMEOUT_MS;

	while (gpio_pin_get_dt(&cfg->busy_gpio) > 0) {
		if (k_uptime_get() > deadline) {
			LOG_ERR("BUSY stuck high (check BUSY pin, power and reset)");
			return -ETIMEDOUT;
		}
		k_msleep(SSD1677_BUSY_POLL_MS);
	}

	return 0;
}

static int ssd1677_cmd(const struct device *dev, uint8_t cmd, const uint8_t *data, size_t len)
{
	const struct ssd1677_config *cfg = dev->config;
	int err = mipi_dbi_command_write(cfg->mipi_dev, &cfg->dbi_config, cmd, data, len);

	mipi_dbi_release(cfg->mipi_dev, &cfg->dbi_config);

	return err;
}

static int ssd1677_cmd8(const struct device *dev, uint8_t cmd, uint8_t value)
{
	return ssd1677_cmd(dev, cmd, &value, 1);
}

static int ssd1677_set_window(const struct device *dev, uint16_t y0, uint16_t y1)
{
	const struct ssd1677_config *cfg = dev->config;
	const uint16_t x1 = cfg->width - 1;
	uint8_t p[4];
	int err;

	/* RAM X range, in pixels. */
	p[0] = 0;
	p[1] = 0;
	p[2] = x1 & 0xFF;
	p[3] = x1 >> 8;
	err = ssd1677_cmd(dev, 0x44, p, 4);
	if (err) {
		return err;
	}

	/* RAM Y range. */
	p[0] = y0 & 0xFF;
	p[1] = y0 >> 8;
	p[2] = y1 & 0xFF;
	p[3] = y1 >> 8;
	err = ssd1677_cmd(dev, 0x45, p, 4);
	if (err) {
		return err;
	}

	/* RAM counters back to the window start. */
	p[0] = 0;
	p[1] = 0;
	err = ssd1677_cmd(dev, 0x4E, p, 2);
	if (err) {
		return err;
	}

	p[0] = y0 & 0xFF;
	p[1] = y0 >> 8;

	return ssd1677_cmd(dev, 0x4F, p, 2);
}

static int ssd1677_wake(const struct device *dev)
{
	const struct ssd1677_config *cfg = dev->config;
	struct ssd1677_data *data = dev->data;
	static const uint8_t booster[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
	uint8_t drv_out[3];
	int err;

	if (data->awake) {
		return 0;
	}

	err = mipi_dbi_reset(cfg->mipi_dev, 10);
	if (err) {
		LOG_ERR("reset failed (%d)", err);
		return err;
	}
	k_msleep(10);

	err = ssd1677_busy_wait(dev);
	if (err) {
		return err;
	}

	err = ssd1677_cmd(dev, 0x12, NULL, 0); /* software reset */
	if (err) {
		return err;
	}
	k_msleep(10);
	err = ssd1677_busy_wait(dev);
	if (err) {
		return err;
	}

	err = ssd1677_cmd(dev, 0x0C, booster, sizeof(booster));
	err = err ? err : ssd1677_cmd8(dev, 0x3C, 0x05); /* border waveform */

	drv_out[0] = (cfg->height - 1) & 0xFF;
	drv_out[1] = (cfg->height - 1) >> 8;
	drv_out[2] = 0x02;
	err = err ? err : ssd1677_cmd(dev, 0x01, drv_out, sizeof(drv_out));
	err = err ? err : ssd1677_cmd8(dev, 0x11, 0x03); /* data entry: X+, Y+ */
	err = err ? err : ssd1677_cmd8(dev, 0x18, 0x80); /* internal temperature sensor */
	err = err ? err : ssd1677_set_window(dev, 0, cfg->height - 1);
	if (err) {
		LOG_ERR("init sequence failed (%d)", err);
		return err;
	}

	data->awake = true;

	return ssd1677_busy_wait(dev);
}

static inline uint8_t reverse_bits(uint8_t b)
{
	b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
	b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
	b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));

	return b;
}

static int ssd1677_write(const struct device *dev, const uint16_t x, const uint16_t y,
			 const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct ssd1677_config *cfg = dev->config;
	struct ssd1677_data *data = dev->data;
	const uint16_t row_bytes = cfg->width / 8;
	const uint8_t *src = buf;
	int err;

	if (x != 0 || y != 0 || desc->width != cfg->width || desc->height != cfg->height ||
	    desc->pitch != cfg->width) {
		LOG_ERR("only whole-screen writes (%ux%u) are supported", cfg->width, cfg->height);
		return -ENOTSUP;
	}

	if (desc->buf_size < (size_t)row_bytes * cfg->height) {
		LOG_ERR("buffer too small");
		return -EINVAL;
	}

	err = ssd1677_wake(dev);
	if (err) {
		return err;
	}

	for (uint16_t y0 = 0; y0 < cfg->height; y0 += SSD1677_BAND_ROWS) {
		uint16_t rows = MIN(SSD1677_BAND_ROWS, cfg->height - y0);
		const uint8_t *chunk = src + (size_t)y0 * row_bytes;
		size_t len = (size_t)rows * row_bytes;

		if (cfg->mirror_x) {
			for (uint16_t r = 0; r < rows; r++) {
				const uint8_t *in = chunk + (size_t)r * row_bytes;
				uint8_t *out = cfg->band + (size_t)r * row_bytes;

				for (uint16_t c = 0; c < row_bytes; c++) {
					out[c] = reverse_bits(in[row_bytes - 1 - c]);
				}
			}
			chunk = cfg->band;
		}

		err = ssd1677_set_window(dev, y0, y0 + rows - 1);
		err = err ? err : ssd1677_cmd(dev, 0x24, chunk, len);
		if (err) {
			LOG_ERR("RAM write failed (%d)", err);
			return err;
		}
	}

	data->loaded = true;

	return 0;
}

static int ssd1677_blanking_on(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static int ssd1677_blanking_off(const struct device *dev)
{
	struct ssd1677_data *data = dev->data;
	int err;

	if (!data->loaded) {
		return 0;
	}

	err = ssd1677_cmd8(dev, 0x22, 0xF7); /* full update sequence */
	err = err ? err : ssd1677_cmd(dev, 0x20, NULL, 0);
	if (err) {
		return err;
	}

	k_msleep(10);
	err = ssd1677_busy_wait(dev);

	/* Deep sleep mode 1 keeps the RAM; only the reset pin wakes it. */
	(void)ssd1677_cmd8(dev, 0x10, 0x01);
	data->awake = false;
	data->loaded = false;

	return err;
}

static void ssd1677_get_capabilities(const struct device *dev, struct display_capabilities *caps)
{
	const struct ssd1677_config *cfg = dev->config;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = cfg->width;
	caps->y_resolution = cfg->height;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO01;
	caps->current_pixel_format = PIXEL_FORMAT_MONO01;
	caps->screen_info = SCREEN_INFO_EPD; /* row-major, MSB first: neither VTILED nor MSB flag */
	caps->screen_info |= SCREEN_INFO_MONO_MSB_FIRST;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int __maybe_unused ssd1677_init(const struct device *dev)
{
	const struct ssd1677_config *cfg = dev->config;
	int err;

	if (!device_is_ready(cfg->mipi_dev)) {
		LOG_ERR("MIPI DBI device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&cfg->busy_gpio)) {
		LOG_ERR("BUSY GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&cfg->busy_gpio, GPIO_INPUT);
	if (err) {
		return err;
	}

	/* The controller is configured at the first frame, not here: the panel stays asleep. */
	return 0;
}

static DEVICE_API(display, ssd1677_api) = {
	.blanking_on = ssd1677_blanking_on,
	.blanking_off = ssd1677_blanking_off,
	.write = ssd1677_write,
	.get_capabilities = ssd1677_get_capabilities,
};

#define SSD1677_DEFINE(n)                                                                       \
	static uint8_t ssd1677_band_##n[SSD1677_BAND_ROWS * (DT_INST_PROP(n, width) / 8)];     \
	static const struct ssd1677_config ssd1677_cfg_##n = {                                 \
		.mipi_dev = DEVICE_DT_GET(DT_INST_PARENT(n)),                                  \
		.dbi_config = {                                                                \
			.mode = MIPI_DBI_MODE_SPI_4WIRE,                                       \
			.config = MIPI_DBI_SPI_CONFIG_DT(                                      \
				DT_DRV_INST(n),                                                \
				SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_HOLD_ON_CS |        \
					SPI_LOCK_ON,                                           \
				0),                                                            \
		},                                                                             \
		.busy_gpio = GPIO_DT_SPEC_INST_GET(n, busy_gpios),                             \
		.width = DT_INST_PROP(n, width),                                               \
		.height = DT_INST_PROP(n, height),                                             \
		.mirror_x = DT_INST_PROP(n, mirror_x),                                         \
		.band = ssd1677_band_##n,                                                      \
	};                                                                                     \
	static struct ssd1677_data ssd1677_data_##n;                                           \
	DEVICE_DT_INST_DEFINE(n, ssd1677_init, NULL, &ssd1677_data_##n, &ssd1677_cfg_##n,      \
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &ssd1677_api);

DT_INST_FOREACH_STATUS_OKAY(SSD1677_DEFINE)
