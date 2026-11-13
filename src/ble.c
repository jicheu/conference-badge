/*
 * Bluetooth LE transport: a GATT service with the Nordic UART Service UUIDs (so generic
 * "BLE UART" apps and tools/badge_ble.html can find it).
 *
 *   RX characteristic (phone writes):   the same packets as on USB, see remote.h
 *   TX characteristic (badge notifies): reply lines, split in chunks of 20 bytes
 *
 * Zephyr's own bt_nus service (4.4) has no security option, so this file defines the service
 * itself, which lets it demand an authenticated link: with CONFIG_BADGE_BLE_PASSKEY != 0 the
 * phone must pair with that passkey before it may write or subscribe. With 0 the link is open.
 *
 * UNTESTED on hardware when this file was written.
 */

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ble.h"
#include "remote.h"

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

#define LINE_MAX_LEN 96
#define NOTIFY_CHUNK 20 /* fits any ATT MTU */

#define UUID_SVC BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)
#define UUID_RX  BT_UUID_128_ENCODE(0x6e400002, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)
#define UUID_TX  BT_UUID_128_ENCODE(0x6e400003, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(UUID_SVC);
static const struct bt_uuid_128 rx_uuid = BT_UUID_INIT_128(UUID_RX);
static const struct bt_uuid_128 tx_uuid = BT_UUID_INIT_128(UUID_TX);

#if CONFIG_BADGE_BLE_PASSKEY != 0
#define PERM_WRITE BT_GATT_PERM_WRITE_AUTHEN
#else
#define PERM_WRITE BT_GATT_PERM_WRITE
#endif

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, UUID_SVC),
};

struct line {
	char text[LINE_MAX_LEN];
};

K_MSGQ_DEFINE(tx_lines, sizeof(struct line), 6, 4);

static ssize_t rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	/* A long write (iPhone/Bluefy use it above 20 bytes) first sends the pieces as
	 * "prepare" requests, which are only a check, then calls us again with the same
	 * data in order at "execute" time. Feed the data only once. */
	static uint32_t n_writes;

	if (n_writes < 8 || (n_writes % 200) == 0 || (flags & BT_GATT_WRITE_FLAG_PREPARE)) {
		LOG_INF("BLE write #%u: %u bytes, offset %u, flags 0x%x", n_writes, len, offset, flags);
	}
	n_writes++;
	if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
		return len;
	}

	remote_feed_ble(buf, len);

	return len;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	LOG_INF("BLE replies %s", value == BT_GATT_CCC_NOTIFY ? "on" : "off");
}

BT_GATT_SERVICE_DEFINE(badge_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	BT_GATT_CHARACTERISTIC(&rx_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       PERM_WRITE, NULL, rx_write, NULL),
	BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | PERM_WRITE),
);

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	ARG_UNUSED(conn);
	LOG_INF("ATT MTU: tx %u, rx %u", tx, rx);
}

static struct bt_gatt_cb gatt_cb = {.att_mtu_updated = mtu_updated};

static void advertise(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("advertising failed (%d)", err);
	} else {
		LOG_INF("advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
	}
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);

	if (err) {
		LOG_ERR("connection failed (0x%02x)", err);
		advertise();
		return;
	}
	LOG_INF("BLE connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	LOG_INF("BLE disconnected (0x%02x)", reason);
	advertise();
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

#if CONFIG_BADGE_BLE_PASSKEY != 0
/* Providing this callback gives the badge the "display" capability: the phone asks the user to
 * type this passkey. */
static uint32_t app_passkey(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	return CONFIG_BADGE_BLE_PASSKEY;
}

static void pairing_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	LOG_WRN("pairing cancelled");
}

static struct bt_conn_auth_cb auth_cbs = {
	.app_passkey = app_passkey,
	.cancel = pairing_cancel,
};
#endif

/* Replies are sent from here, never from the BLE stack's own thread. */
static void tx_thread(void *a, void *b, void *c)
{
	struct line l;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		char out[LINE_MAX_LEN + 2];
		size_t n;

		k_msgq_get(&tx_lines, &l, K_FOREVER);
		n = (size_t)snprintk(out, sizeof(out), "%s\n", l.text);

		for (size_t off = 0; off < n; off += NOTIFY_CHUNK) {
			size_t part = MIN((size_t)NOTIFY_CHUNK, n - off);
			int err = bt_gatt_notify_uuid(NULL, &tx_uuid.uuid, badge_svc.attrs,
						      out + off, (uint16_t)part);

			if (err) { /* not connected / not subscribed: nobody is listening */
				break;
			}
		}
	}
}

K_THREAD_STACK_DEFINE(tx_stack, 1024);
static struct k_thread tx_thread_data;

void ble_send_line(const char *line)
{
	struct line l;

	strncpy(l.text, line, sizeof(l.text) - 1);
	l.text[sizeof(l.text) - 1] = '\0';
	(void)k_msgq_put(&tx_lines, &l, K_NO_WAIT);
}

int ble_start(void)
{
	int err;

#if CONFIG_BADGE_BLE_PASSKEY != 0
	err = bt_conn_auth_cb_register(&auth_cbs);
	if (err) {
		LOG_ERR("auth callbacks failed (%d)", err);
		return err;
	}
#else
	LOG_WRN("BLE is OPEN: anyone nearby can send a page (set CONFIG_BADGE_BLE_PASSKEY)");
#endif

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

	bt_gatt_cb_register(&gatt_cb);

	k_thread_create(&tx_thread_data, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack), tx_thread, NULL,
			NULL, NULL, K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread_data, "ble_tx");

	advertise();

	return 0;
}
