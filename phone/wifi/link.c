/*
 * GLINK channels over SMEM, and QRTR on IPCRTR: enough of Linux qcom_glink_native.c and net/qrtr to talk QMI
 * to the modem (wland: one channel, IPCRTR). Polled. The modem is QRTR node 0; we are node 1.
 * LINK_RAW builds (phone/audio, the ADSP edge) have two: LINK_CHAN ("apr_audio_svc", each packet to
 * link_raw) and IPCRTR for QRTR (the ADSP is QRTR node 5: it asks our pd-mapper for its audio PD).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "glink.h"
#include "link.h"
#include "pmem.h"

#define N_INTENTS	8
#define INTENT_SIZE	4096
#define FEAT_INTENT_REUSE 1
#ifndef LINK_CHAN
#define LINK_CHAN	"IPCRTR"
#endif
#ifdef LINK_RAW
#define NCHAN		2
#define QCH		1	/* the QRTR channel */
#else
#define NCHAN		1
#define QCH		0
#endif
#define MAX_RINTENTS	256

static void qrtr_deliver(const uint8_t *d, uint32_t n);

struct chan {
	const char *name;
	uint16_t lcid;
	uint32_t rcid;
	int open_tx, open_acked, opened_by_remote;
	struct { uint32_t size, iid; int busy; } rintent[MAX_RINTENTS];
	int nrintent;
	volatile int intent_req_acked;
	uint8_t rxbuf[65536];	/* reassembly of a TX_DATA spread over chunks */
	uint32_t rxoff;
	void (*deliver)(const uint8_t *d, uint32_t n);
};

static struct chan ch[NCHAN] = {
#ifdef LINK_RAW
	{ .name = LINK_CHAN, .lcid = 1, .deliver = link_raw },
	{ .name = "IPCRTR", .lcid = 2, .deliver = qrtr_deliver },
#else
	{ .name = LINK_CHAN, .lcid = 1, .deliver = qrtr_deliver },
#endif
};
static int version_done;
static qrtr_cb_t qrtr_cb;

int verbose_link = 1;
unsigned link_stats[4];
void (*link_idle)(void);
#define LOG(...) do { if (verbose_link) { printf("[%6llu] ", (unsigned long long)link_ms()); printf(__VA_ARGS__); } } while (0)

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t t0;
uint64_t link_ms(void)
{
	if (!t0)
		t0 = now_ms();
	return now_ms() - t0;
}

static struct chan *by_rcid(uint32_t rcid)
{
	for (int i = 0; i < NCHAN; i++)
		if (ch[i].opened_by_remote && ch[i].rcid == rcid)
			return &ch[i];
	return NULL;
}

static void send_open(struct chan *c)
{
	glink_send(GLINK_CMD_OPEN, c->lcid, (uint32_t)strlen(c->name) + 1, c->name, strlen(c->name) + 1);
	c->open_tx = 1;
}

static void advertise_intents(struct chan *c)
{
	uint32_t p[N_INTENTS * 2];
	for (int i = 0; i < N_INTENTS; i++) {
		p[2 * i] = INTENT_SIZE;
		p[2 * i + 1] = i + 1;
	}
	glink_send(GLINK_CMD_INTENT, c->lcid, N_INTENTS, p, sizeof(p));
}

/* one command from the FIFO; returns 0 if incomplete (wait for more) */
static int rx_one(void)
{
	size_t avail = glink_rx_avail();
	if (avail < 8)
		return 0;
	uint8_t h[16];
	glink_rx_peek(h, 0, 8);
	uint16_t cmd, p1;
	uint32_t p2;
	memcpy(&cmd, h, 2);
	memcpy(&p1, h + 2, 2);
	memcpy(&p2, h + 4, 4);
	size_t adv = 8;
	struct chan *c;

	switch (cmd) {
	case GLINK_CMD_VERSION:
		LOG("glink: < VERSION %u features %#x\n", p1, p2);
		glink_send(GLINK_CMD_VERSION_ACK, 1, p2 & FEAT_INTENT_REUSE, NULL, 0);
		if (!version_done) {
			glink_send(GLINK_CMD_VERSION, 1, FEAT_INTENT_REUSE, NULL, 0);
		}
		break;
	case GLINK_CMD_VERSION_ACK:
		LOG("glink: < VERSION_ACK %u features %#x\n", p1, p2);
		version_done = 1;
		break;
	case GLINK_CMD_OPEN: {
		size_t nl = (p2 + 7) & ~7u;
		if (avail < 8 + nl)
			return 0;
		char name[64] = { 0 };
		glink_rx_peek(name, 8, p2 < 63 ? p2 : 63);
		adv += nl;
		LOG("glink: < OPEN rcid %u \"%s\"\n", p1, name);
		/* Linux acks every remote open; only our channels get opened from our side */
		glink_send(GLINK_CMD_OPEN_ACK, p1, 0, NULL, 0);
		for (int i = 0; i < NCHAN; i++)
			if (!strcmp(name, ch[i].name)) {
				ch[i].rcid = p1;
				ch[i].opened_by_remote = 1;
				if (!ch[i].open_tx)
					send_open(&ch[i]);
			}
		break;
	}
	case GLINK_CMD_OPEN_ACK:
		LOG("glink: < OPEN_ACK lcid %u\n", p1);
		for (int i = 0; i < NCHAN; i++)
			if (p1 == ch[i].lcid && !ch[i].open_acked) {
				ch[i].open_acked = 1;
				advertise_intents(&ch[i]);
			}
		break;
	case GLINK_CMD_CLOSE:
		LOG("glink: < CLOSE rcid %u\n", p1);
		glink_send(GLINK_CMD_CLOSE_ACK, p1, 0, NULL, 0);
		break;
	case GLINK_CMD_CLOSE_ACK:
		LOG("glink: < CLOSE_ACK %u\n", p1);
		break;
	case GLINK_CMD_INTENT: {
		size_t len = 8 + (size_t)p2 * 8;
		if (avail < len)
			return 0;
		c = by_rcid(p1);
		for (uint32_t i = 0; i < p2; i++) {
			uint32_t pr[2];
			glink_rx_peek(pr, 8 + i * 8, 8);
			if (verbose_link > 1)
				LOG("glink: < INTENT rcid %u size %u iid %u\n", p1, pr[0], pr[1]);
			if (c && c->nrintent < MAX_RINTENTS) {	/* intent ids are per channel */
				c->rintent[c->nrintent].size = pr[0];
				c->rintent[c->nrintent].iid = pr[1];
				c->rintent[c->nrintent].busy = 0;
				c->nrintent++;
			}
		}
		adv = (len + 7) & ~7u;
		break;
	}
	case GLINK_CMD_RX_DONE:
	case GLINK_CMD_RX_DONE_W_REUSE:
		if (!(c = by_rcid(p1)))
			break;
		if (verbose_link > 1)
			LOG("glink: < RX_DONE%s iid %u\n", cmd == GLINK_CMD_RX_DONE ? "" : "_W_REUSE", p2);
		link_stats[cmd == GLINK_CMD_RX_DONE ? 0 : 1]++;
		for (int i = 0; i < c->nrintent; i++)
			if (c->rintent[i].iid == p2) {
				if (cmd == GLINK_CMD_RX_DONE) {
					c->rintent[i] = c->rintent[--c->nrintent];
				} else {
					c->rintent[i].busy = 0;
				}
				break;
			}
		break;
	case GLINK_CMD_RX_INTENT_REQ:
		/* we take any data in place, so every request is granted with a fresh intent id */
		if ((c = by_rcid(p1))) {
			static uint32_t next_iid = 100;
			uint32_t p[2] = { p2 > INTENT_SIZE ? p2 : INTENT_SIZE, next_iid++ };
			glink_send(GLINK_CMD_INTENT, c->lcid, 1, p, sizeof(p));
			glink_send(GLINK_CMD_RX_INTENT_REQ_ACK, c->lcid, 1, NULL, 0);
		} else {
			glink_send(GLINK_CMD_RX_INTENT_REQ_ACK, 0, 0, NULL, 0);
		}
		break;
	case GLINK_CMD_RX_INTENT_REQ_ACK:
		for (int i = 0; i < NCHAN; i++)
			ch[i].intent_req_acked = 1;
		if (!p2)
			LOG("glink: < RX_INTENT_REQ_ACK: refused\n");
		break;
	case GLINK_CMD_TX_DATA:
	case GLINK_CMD_TX_DATA_CONT: {
		if (avail < 16)
			return 0;
		glink_rx_peek(h, 0, 16);
		uint32_t chunk, left;
		memcpy(&chunk, h + 8, 4);
		memcpy(&left, h + 12, 4);
		size_t len = 16 + chunk;
		if (avail < len)
			return 0;
		adv = (len + 7) & ~7u;
		if (!(c = by_rcid(p1))) {
			LOG("glink: < TX_DATA on rcid %u (%u bytes)\n", p1, chunk);
			break;
		}
		if (c->rxoff + chunk <= sizeof(c->rxbuf))
			glink_rx_peek(c->rxbuf + c->rxoff, 16, chunk);
		c->rxoff += chunk;
		if (!left) {
			glink_rx_advance(adv);
			uint32_t n = c->rxoff;
			c->rxoff = 0;
			glink_send(GLINK_CMD_RX_DONE_W_REUSE, c->lcid, p2, NULL, 0);
			c->deliver(c->rxbuf, n);
			return 1;
		}
		break;
	}
	case GLINK_CMD_READ_NOTIF:
		glink_kick();
		break;
	case GLINK_CMD_SIGNALS:
		LOG("glink: < SIGNALS %#x\n", p2);
		break;
	default:
		LOG("glink: < unknown cmd %u (%u %#x); dropping the FIFO\n", cmd, p1, p2);
		adv = avail;
		break;
	}
	glink_rx_advance(adv);
	return 1;
}

int link_poll(unsigned ms)
{
	uint64_t end = now_ms() + ms;
	int n = 0;
	do {
		while (rx_one())
			n++;
		if (link_idle)
			link_idle();
		if (ms)
			delay_ms(1);
	} while (now_ms() < end);
	return n;
}

int link_up(qrtr_cb_t cb, unsigned timeout_ms)
{
	struct chan *c = &ch[0];
	qrtr_cb = cb;
	if (glink_init())
		return -1;
	glink_send(GLINK_CMD_VERSION, 1, FEAT_INTENT_REUSE, NULL, 0);
	uint64_t start = now_ms(), end = start + timeout_ms;
	while (now_ms() < end && !(c->open_acked && c->opened_by_remote)) {
		link_poll(10);
#ifdef LINK_RAW
		/* the remote may wait for us to open first */
		if (!c->open_tx && version_done && now_ms() > start + 3000)
			send_open(c);
#endif
	}
	if (!(c->open_acked && c->opened_by_remote)) {
		printf("glink: %s not open (version %d, remote open %d, ack %d)\n", c->name,
		       version_done, c->opened_by_remote, c->open_acked);
		return -1;
	}
	printf("glink: %s open (rcid %u)\n", c->name, c->rcid);
	return 0;
}

int link_qrtr_open(void)
{
	return ch[QCH].open_acked && ch[QCH].opened_by_remote;
}

static int find_intent(struct chan *c, uint32_t len)
{
	int best = -1;
	for (int i = 0; i < c->nrintent; i++)
		if (!c->rintent[i].busy && c->rintent[i].size >= len && (best < 0 || c->rintent[i].size < c->rintent[best].size))
			best = i;
	return best;
}

/* One packet into one of the remote's intents (requested if none fits), in chunks of 8K as Linux does. */
static int chan_send(struct chan *c, const void *data, uint32_t len)
{
	uint64_t start = now_ms(), end = start + 3000;
	int i, asked = 0;
	while ((i = find_intent(c, len)) < 0) {
		if (!asked || (c->intent_req_acked && asked)) {
			if (asked > 3)
				goto fail;
			c->intent_req_acked = 0;
			link_stats[2]++;
			glink_send(GLINK_CMD_RX_INTENT_REQ, c->lcid, len, NULL, 0);
			asked++;
		}
		if (now_ms() > end)
			goto fail;
		link_poll(1);
	}
	link_stats[3] += (unsigned)(now_ms() - start);
	c->rintent[i].busy = 1;
	if (verbose_link > 1)
		LOG("glink: > TX %u bytes in iid %u (size %u)\n", len, c->rintent[i].iid, c->rintent[i].size);
	const uint8_t *d = data;
	for (uint32_t off = 0; off < len;) {
		uint32_t chunk = len - off > 8192 ? 8192 : len - off;
		uint8_t b[8 + 8192];
		uint32_t hdr[2] = { chunk, len - off - chunk };
		memcpy(b, hdr, 8);
		memcpy(b + 8, d + off, chunk);
		while (glink_send(off ? GLINK_CMD_TX_DATA_CONT : GLINK_CMD_TX_DATA, c->lcid, c->rintent[i].iid, b, 8 + chunk)) {
			if (now_ms() > end)
				goto fail;
			link_poll(1);
		}
		off += chunk;
	}
	return 0;
fail:
	printf("glink: cannot send %u bytes on %s (intents %d)\n", len, c->name, c->nrintent);
	return -1;
}

int link_send(const void *data, uint32_t len)
{
	return chan_send(&ch[0], data, len);
}

/* ---- QRTR ---- */

static void qrtr_deliver(const uint8_t *d, uint32_t n)
{
	struct qrtr_hdr hd;
	if (n < sizeof(hd)) {
		LOG("qrtr: short packet %u\n", n);
		return;
	}
	memcpy(&hd, d, sizeof(hd));
	if (hd.version != 1) {
		LOG("qrtr: version %u\n", hd.version);
		return;
	}
	if (qrtr_cb)
		qrtr_cb(&hd, d + sizeof(hd), n - sizeof(hd) < hd.size ? n - sizeof(hd) : hd.size);
}

int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port,
	      const void *payload, uint32_t len)
{
	static uint8_t b[65536 + 64];
	struct qrtr_hdr hd = {
		.version = 1, .type = type, .src_node = QRTR_NODE_APPS, .src_port = src_port,
		.confirm_rx = 0, .size = len, .dst_node = dst_node, .dst_port = dst_port,
	};
	uint32_t padded = (len + 3) & ~3u;
	if (sizeof(hd) + padded > sizeof(b))
		return -1;
	memset(b + sizeof(hd) + len, 0, padded - len);
	memcpy(b, &hd, sizeof(hd));
	memcpy(b + sizeof(hd), payload, len);
	return chan_send(&ch[QCH], b, sizeof(hd) + padded);
}

int qrtr_new_server(uint32_t service, uint32_t version, uint32_t instance, uint32_t port)
{
	uint32_t pkt[5] = { QRTR_TYPE_NEW_SERVER, service, (version & 0xff) | (instance << 8), QRTR_NODE_APPS, port };
	return qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, QRTR_NODE_REMOTE, QRTR_PORT_CTRL, pkt, sizeof(pkt));
}

/* After a packet with confirm_rx: tell the sender it may go on (Linux qrtr_port_post: RESUME_TX). */
int qrtr_resume_tx(const struct qrtr_hdr *h)
{
	/* struct qrtr_ctrl_pkt: cmd, then client { node, port } (the union's first two words) */
	uint32_t pkt[5] = { QRTR_TYPE_RESUME_TX, h->dst_node, h->dst_port, 0, 0 };
	return qrtr_send(QRTR_TYPE_RESUME_TX, h->dst_port, h->src_node, h->src_port, pkt, sizeof(pkt));
}
