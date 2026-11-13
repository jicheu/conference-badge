/*
 * NFC Type 2 tag with one NDEF URL record, on the nRF52840 NFCT peripheral (nrfx driver).
 *
 * The hardware answers the anticollision and selection steps by itself. We answer the
 * Type 2 commands: READ (0x30) returns 16 bytes of the 64-byte tag memory, HALT (0x50)
 * puts the tag to sleep, everything else (WRITE, ...) gets a NACK, so the tag is read only.
 *
 * Tag memory (64 bytes): 0-9 UID and check bytes (built here from the chip's NFC ID),
 * 10-63 come from the generated header (lock bytes, capability container, NDEF message).
 */
#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <nrfx.h>
#include <nrfx_nfct.h>

#include "badge_nfc.h"
#include "nfc.h"

LOG_MODULE_REGISTER(nfc, LOG_LEVEL_INF);

#ifndef BADGE_NFC_URL
#error "CONFIG_BADGE_NFC needs an \"nfc_url\" in the JSON file: add it and run tools/make_badge.py again"
#endif

#define T2T_MEM_SIZE 64
#define T2T_READ     0x30
#define T2T_HALT     0x50
#define NFC_CRC_SIZE 2
#define NFC_READ_LEN 16

static uint8_t mem[T2T_MEM_SIZE];
static uint8_t rx_buf[16];
static uint8_t tx_buf[NFC_READ_LEN];
static const uint8_t nack_not_acked; /* 4-bit NACK 0x0: invalid argument */

static const nrfx_nfct_data_desc_t rx_desc = {
	.data_size = sizeof(rx_buf),
	.p_data = rx_buf,
};

static void rearm_rx(void)
{
	(void)nrfx_nfct_rx(&rx_desc);
}

static void send_nack(void)
{
	const nrfx_nfct_data_desc_t d = {.data_size = 4, .p_data = &nack_not_acked}; /* bits */

	if (nrfx_nfct_bits_tx(&d, NRF_NFCT_FRAME_DELAY_MODE_WINDOWGRID) != 0) {
		rearm_rx();
	}
}

static void handle_frame(const uint8_t *data, uint32_t size)
{
	if (size < NFC_CRC_SIZE + 1) {
		rearm_rx();
		return;
	}
	size -= NFC_CRC_SIZE; /* the hardware checks and strips nothing: CRC bytes are at the end */

	switch (data[0]) {
	case T2T_READ:
		if (size != 2) {
			send_nack();
			return;
		}
		for (int i = 0; i < NFC_READ_LEN; i++) {
			/* page n is bytes 4n..4n+3; reads past the end wrap around (Type 2 rule) */
			tx_buf[i] = mem[(data[1] * 4 + i) % T2T_MEM_SIZE];
		}
		{
			const nrfx_nfct_data_desc_t d = {.data_size = NFC_READ_LEN, .p_data = tx_buf};

			if (nrfx_nfct_tx(&d, NRF_NFCT_FRAME_DELAY_MODE_WINDOWGRID) != 0) {
				rearm_rx();
			}
		}
		break;
	case T2T_HALT:
		nrfx_nfct_init_substate_force(NRFX_NFCT_ACTIVE_STATE_SLEEP);
		rearm_rx();
		break;
	default:
		send_nack(); /* WRITE and the rest: the tag is read only */
		break;
	}
}

/* Runs in interrupt context: keep it short. */
static void nfct_handler(nrfx_nfct_evt_t const *ev)
{
	switch (ev->evt_id) {
	case NRFX_NFCT_EVT_FIELD_DETECTED:
		nrfx_nfct_state_force(NRFX_NFCT_STATE_ACTIVATED);
		break;
	case NRFX_NFCT_EVT_FIELD_LOST:
		nrfx_nfct_state_force(NRFX_NFCT_STATE_SENSING);
		break;
	case NRFX_NFCT_EVT_SELECTED:
		rearm_rx();
		break;
	case NRFX_NFCT_EVT_RX_FRAMEEND:
		if (ev->params.rx_frameend.rx_status != 0) {
			rearm_rx(); /* CRC or parity error: ignore the frame */
		} else {
			handle_frame(ev->params.rx_frameend.rx_data.p_data,
				     ev->params.rx_frameend.rx_data.data_size);
		}
		break;
	case NRFX_NFCT_EVT_TX_FRAMEEND:
		rearm_rx();
		break;
	default:
		break;
	}
}

static void build_memory(void)
{
	uint8_t id[NRFX_NFCT_NFCID1_DOUBLE_SIZE];

	nrfx_nfct_nfcid1_default_bytes_get(id, sizeof(id));

	/* Type 2 internal bytes for a 7-byte UID: UID0-2, BCC0, UID3-6, BCC1, internal. */
	mem[0] = id[0];
	mem[1] = id[1];
	mem[2] = id[2];
	mem[3] = 0x88 ^ id[0] ^ id[1] ^ id[2];
	mem[4] = id[3];
	mem[5] = id[4];
	mem[6] = id[5];
	mem[7] = id[6];
	mem[8] = id[3] ^ id[4] ^ id[5] ^ id[6];
	mem[9] = 0x00;

	BUILD_ASSERT(sizeof(badge_nfc_t2t_tail) == T2T_MEM_SIZE - 10, "tag image has the wrong size");
	memcpy(&mem[10], badge_nfc_t2t_tail, sizeof(badge_nfc_t2t_tail));
}

/* The nrfx NFCT driver uses a TIMER for a silicon workaround (errata 79/190) and asks the
 * application to call nrfx_nfct_workaround_timer_handler() from that timer's interrupt. */
#ifdef NRFX_NFCT_CONFIG_TIMER_INSTANCE_ID
#define NFC_TIMER_IRQN    NRFX_CONCAT(NRFX_CONCAT(TIMER, NRFX_NFCT_CONFIG_TIMER_INSTANCE_ID), _IRQn)
#define NFC_TIMER_HANDLER nrfx_nfct_workaround_timer_handler /* the driver owns that timer instance */
#endif

int nfc_start(void)
{
	int err;
	const nrfx_nfct_config_t cfg = {
		.rxtx_int_mask = NRFX_NFCT_EVT_RX_FRAMEEND | NRFX_NFCT_EVT_TX_FRAMEEND,
		.cb = nfct_handler,
		.irq_priority = DT_IRQ(DT_NODELABEL(nfct), priority),
	};

	IRQ_CONNECT(DT_IRQN(DT_NODELABEL(nfct)), DT_IRQ(DT_NODELABEL(nfct), priority), nrfx_isr,
		    nrfx_nfct_irq_handler, 0);
#ifdef NFC_TIMER_IRQN
	IRQ_CONNECT(NFC_TIMER_IRQN, DT_IRQ(DT_NODELABEL(nfct), priority), nrfx_isr,
		    NFC_TIMER_HANDLER, 0);
#endif

	build_memory();

	err = nrfx_nfct_init(&cfg);
	if (err) {
		/* -EPERM: the NFC pins are set up as normal GPIO (UICR), see README */
		LOG_ERR("NFC init failed (%d)", err);
		return err;
	}

	nrfx_nfct_enable();
	LOG_INF("NFC tag ready: %s", BADGE_NFC_URL);
	return 0;
}
