/* SSC client: see ssc.h. */
#include <stdio.h>
#include <string.h>
#include "ssc.h"

/* ---- protobuf ---- */

static void put_varint(struct pb *p, uint64_t v)
{
	do {
		uint8_t c = v & 0x7f;
		v >>= 7;
		if (v)
			c |= 0x80;
		if (p->n < p->cap)
			p->b[p->n] = c;
		p->n++;
	} while (v);
}

static void put_raw(struct pb *p, const void *d, size_t len)
{
	if (p->n + len <= p->cap)
		memcpy(p->b + p->n, d, len);
	p->n += len;
}

void pb_varint(struct pb *p, unsigned field, uint64_t v)
{
	put_varint(p, (uint64_t)field << 3 | 0);
	put_varint(p, v);
}

void pb_fixed32(struct pb *p, unsigned field, uint32_t v)
{
	put_varint(p, (uint64_t)field << 3 | 5);
	put_raw(p, &v, 4);
}

void pb_fixed64(struct pb *p, unsigned field, uint64_t v)
{
	put_varint(p, (uint64_t)field << 3 | 1);
	put_raw(p, &v, 8);
}

void pb_float(struct pb *p, unsigned field, float v)
{
	uint32_t u;
	memcpy(&u, &v, 4);
	pb_fixed32(p, field, u);
}

void pb_bytes(struct pb *p, unsigned field, const void *d, size_t len)
{
	put_varint(p, (uint64_t)field << 3 | 2);
	put_varint(p, len);
	put_raw(p, d, len);
}

static int get_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
	*v = 0;
	for (unsigned s = 0; s < 64; s += 7) {
		if (*p >= end)
			return -1;
		uint8_t c = *(*p)++;
		*v |= (uint64_t)(c & 0x7f) << s;
		if (!(c & 0x80))
			return 0;
	}
	return -1;
}

int pb_next(const uint8_t **p, const uint8_t *end, struct pbf *f)
{
	uint64_t k;
	if (*p >= end)
		return 0;
	if (get_varint(p, end, &k))
		return -1;
	f->field = (unsigned)(k >> 3);
	f->wt = (unsigned)(k & 7);
	f->d = NULL;
	f->len = 0;
	f->v = 0;
	switch (f->wt) {
	case 0:
		return get_varint(p, end, &f->v) ? -1 : 1;
	case 1:
		if (end - *p < 8)
			return -1;
		memcpy(&f->v, *p, 8);
		*p += 8;
		return 1;
	case 5: {
		uint32_t u;
		if (end - *p < 4)
			return -1;
		memcpy(&u, *p, 4);
		f->v = u;
		*p += 4;
		return 1;
	}
	case 2:
		if (get_varint(p, end, &k) || k > (uint64_t)(end - *p))
			return -1;
		f->d = *p;
		f->len = (size_t)k;
		*p += k;
		return 1;
	}
	return -1;
}

size_t ssc_request(uint8_t *out, size_t cap, uint64_t low, uint64_t high, uint32_t msg_id, const void *payload, size_t plen)
{
	uint8_t ub[32], cb[16], bb[512];
	struct pb u = { ub, 0, sizeof(ub) }, c = { cb, 0, sizeof(cb) }, b = { bb, 0, sizeof(bb) }, r = { out, 0, cap };
	pb_fixed64(&u, 1, low);
	pb_fixed64(&u, 2, high);
	pb_varint(&c, 1, 1);	/* processor: APSS */
	pb_varint(&c, 2, 0);	/* suspend mode: wakeup */
	pb_bytes(&b, 2, payload, plen);
	if (b.n > b.cap)
		return 0;
	pb_bytes(&r, 1, ub, u.n);
	pb_fixed32(&r, 2, msg_id);
	pb_bytes(&r, 3, cb, c.n);
	pb_bytes(&r, 4, bb, b.n);
	return r.n <= cap ? r.n : 0;
}

#ifndef SSC_PURE
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "../wifi/link.h"
#include "../wifi/qmi.h"

#define QMI_SNS_REQ	0x20
#define QMI_SNS_REPORT	0x21
#define QMI_SNS_JUMBO	0x22
#define PORT_SUID	0x4020
#define OUT_FILE	"/var/local/imu.txt"

struct sens {
	const char *type, *caps;
	float rate;
	uint32_t port;
	uint64_t lo, hi;
	int found, enabled, tries, reenables;
	unsigned n, configs;
	float v[3];
	int status;
	uint64_t last_ms, enable_ms;
};
static struct sens sn[3] = {
	{ .type = "accel", .caps = "ACCEL", .rate = 50, .port = PORT_SUID + 1 },
	{ .type = "gyro", .caps = "GYRO", .rate = 50, .port = PORT_SUID + 2 },
	{ .type = "mag", .caps = "MAG", .rate = 25, .port = PORT_SUID + 3 },
};
#define NS (sizeof(sn) / sizeof(sn[0]))
static uint32_t ssc_node, ssc_svc = 0xffffffff;
static uint16_t txn;

#define LOGT(...) do { printf("[%6llu] ", (unsigned long long)link_ms()); printf(__VA_ARGS__); } while (0)

static void send_req(uint32_t port, uint64_t lo, uint64_t hi, uint32_t msg_id, const void *pl, size_t plen)
{
	uint8_t buf[600];
	size_t n = ssc_request(buf + 2, sizeof(buf) - 2, lo, hi, msg_id, pl, plen);
	if (!n)
		return;
	uint16_t l16 = (uint16_t)n;
	memcpy(buf, &l16, 2);
	struct qmi_buf q;
	qmi_start(&q, QMI_REQUEST, ++txn, QMI_SNS_REQ);
	qmi_tlv(&q, 1, buf, (uint16_t)(n + 2));
	qmi_tlv_u8(&q, 0x10, 1);	/* jumbo reports allowed */
	qrtr_send(QRTR_TYPE_DATA, port, ssc_node, ssc_svc, q.b, (uint32_t)qmi_finish(&q));
}

static void lookup(struct sens *s)
{
	uint8_t b[64];
	struct pb p = { b, 0, sizeof(b) };
	pb_bytes(&p, 1, s->type, strlen(s->type));
	pb_varint(&p, 2, 0);	/* no updates */
	pb_varint(&p, 3, 1);	/* the default sensor only */
	s->tries++;
	send_req(PORT_SUID, SSC_SUID_MAGIC, SSC_SUID_MAGIC, SSC_MSG_SUID_REQ, b, p.n);
}

static void enable(struct sens *s)
{
	uint8_t b[16];
	struct pb p = { b, 0, sizeof(b) };
	pb_float(&p, 1, s->rate);
	send_req(s->port, s->lo, s->hi, SSC_MSG_ENABLE, b, p.n);
	s->enabled = 1;
	s->enable_ms = link_ms();
	LOGT("ssc: enable %s at %.0f Hz\n", s->type, s->rate);
}

void ssc_server(uint32_t node, uint32_t port)
{
	if (ssc_svc != 0xffffffff)
		return;
	ssc_node = node;
	ssc_svc = port;
	LOGT("#### SSC SERVICE UP (sensor client service %u at %u:%x)\n", SSC_SERVICE, node, port);
	for (unsigned i = 0; i < NS; i++)
		lookup(&sn[i]);
}

int ssc_port(uint32_t port)
{
	return port >= PORT_SUID && port <= PORT_SUID + NS;
}

static void suid_event(const uint8_t *m, size_t len)
{
	const uint8_t *p = m, *end = m + len;
	struct pbf f;
	char type[32] = { 0 };
	uint64_t lo = 0, hi = 0;
	int nuid = 0;
	while (pb_next(&p, end, &f) > 0) {
		if (f.field == 1 && f.wt == 2)
			memcpy(type, f.d, f.len < 31 ? f.len : 31);
		else if (f.field == 2 && f.wt == 2) {
			const uint8_t *q = f.d;
			struct pbf g;
			uint64_t l = 0, h = 0;
			while (pb_next(&q, f.d + f.len, &g) > 0)
				if (g.field == 1)
					l = g.v;
				else if (g.field == 2)
					h = g.v;
			if (!nuid++) {
				lo = l;
				hi = h;
			}
		}
	}
	for (unsigned i = 0; i < NS; i++) {
		struct sens *s = &sn[i];
		if (strcmp(type, s->type) || s->found)
			continue;
		if (!nuid) {
			if (s->tries <= 3 || s->tries % 10 == 0)
				LOGT("ssc: no \"%s\" sensor yet (try %d)\n", type, s->tries);
			return;
		}
		s->lo = lo;
		s->hi = hi;
		s->found = 1;
		LOGT("#### FOUND %s SENSOR (suid %016llx%016llx, %d of that type)\n", s->caps, (unsigned long long)hi,
		     (unsigned long long)lo, nuid);
		enable(s);
		return;
	}
	LOGT("ssc: suid event for \"%s\" (%d uids)\n", type, nuid);
}

static void sample(struct sens *s, const uint8_t *m, size_t len)
{
	const uint8_t *p = m, *end = m + len;
	struct pbf f;
	float v[8];
	unsigned nv = 0;
	int status = -1;
	while (pb_next(&p, end, &f) > 0) {
		if (f.field == 1 && f.wt == 5 && nv < 8) {
			uint32_t u = (uint32_t)f.v;
			memcpy(&v[nv++], &u, 4);
		} else if (f.field == 1 && f.wt == 2) {	/* packed */
			for (size_t o = 0; o + 4 <= f.len && nv < 8; o += 4)
				memcpy(&v[nv++], f.d + o, 4);
		} else if (f.field == 2 && f.wt == 0)
			status = (int)f.v;
	}
	if (nv < 3)
		return;
	memcpy(s->v, v, sizeof(s->v));
	s->status = status;
	s->last_ms = link_ms();
	if (!s->n++)
		LOGT("#### FIRST %s SAMPLE: %.4f %.4f %.4f (status %d) %.0f ms after enable\n", s->caps, v[0], v[1], v[2],
		     status, (double)(s->last_ms - s->enable_ms));
}

static void event(uint32_t port, const uint8_t *m, size_t len)
{
	const uint8_t *p = m, *end = m + len;
	struct pbf f;
	uint64_t lo = 0, hi = 0;
	/* the uid comes first; the events after it */
	while (pb_next(&p, end, &f) > 0) {
		if (f.field == 1 && f.wt == 2) {
			const uint8_t *q = f.d;
			struct pbf g;
			while (pb_next(&q, f.d + f.len, &g) > 0)
				if (g.field == 1)
					lo = g.v;
				else if (g.field == 2)
					hi = g.v;
			continue;
		}
		if (f.field != 2 || f.wt != 2)
			continue;
		const uint8_t *q = f.d;
		struct pbf g;
		uint32_t msg_id = 0;
		const uint8_t *msg = NULL;
		size_t mlen = 0;
		while (pb_next(&q, f.d + f.len, &g) > 0)
			if (g.field == 1)
				msg_id = (uint32_t)g.v;
			else if (g.field == 3 && g.wt == 2) {
				msg = g.d;
				mlen = g.len;
			}
		if (lo == SSC_SUID_MAGIC && hi == SSC_SUID_MAGIC) {
			if (msg_id == SSC_MSG_SUID_EVENT && msg)
				suid_event(msg, mlen);
			continue;
		}
		struct sens *s = NULL;
		for (unsigned i = 0; i < NS; i++)
			if (sn[i].found && sn[i].lo == lo && sn[i].hi == hi)
				s = &sn[i];
		if (!s) {
			LOGT("ssc: event %u for an unknown sensor %016llx%016llx (port %#x)\n", msg_id, (unsigned long long)hi,
			     (unsigned long long)lo, port);
			continue;
		}
		if (msg_id == SSC_MSG_SAMPLE && msg)
			sample(s, msg, mlen);
		else if (msg_id == SSC_MSG_CONFIG_EVENT) {
			if (!s->configs++)
				LOGT("ssc: %s config event (%zu bytes)\n", s->type, mlen);
		} else {
			LOGT("ssc: %s event msg_id %u (%zu bytes)\n", s->type, msg_id, mlen);
		}
	}
}

void ssc_handle(uint32_t port, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	if (qmi_parse(d, n, &m))
		return;
	if (m.type == QMI_RESPONSE) {
		uint16_t rl = 0;
		const uint8_t *r = qmi_get(&m, 2, &rl);
		uint16_t res = r && rl >= 4 ? (uint16_t)(r[0] | r[1] << 8) : 0xffff, err = r && rl >= 4 ? (uint16_t)(r[2] | r[3] << 8) : 0;
		uint32_t rc = 0;
		qmi_get_u32(&m, 0x11, &rc);
		if (res || err || rc)
			LOGT("ssc: request on port %#x -> result %u error %u sns result %u\n", port, res, err, rc);
		return;
	}
	if (m.type != QMI_INDICATION || (m.id != QMI_SNS_REPORT && m.id != QMI_SNS_JUMBO)) {
		LOGT("ssc: message type %u id %#x on port %#x\n", m.type, m.id, port);
		return;
	}
	uint16_t l = 0;
	const uint8_t *pl = qmi_get(&m, 2, &l);
	if (!pl || l < 2)
		return;
	uint16_t inner;
	memcpy(&inner, pl, 2);
	if (inner == l - 2) {	/* the payload is a QMI variable-length array: u16 count first */
		pl += 2;
		l -= 2;
	}
	event(port, pl, l);
}

static void write_out(void)
{
	char b[400];
	int k = snprintf(b, sizeof(b), "t=%llu", (unsigned long long)link_ms());
	for (unsigned i = 0; i < NS; i++)
		k += snprintf(b + k, sizeof(b) - (size_t)k, " %s=%.5f,%.5f,%.5f", sn[i].type, sn[i].v[0], sn[i].v[1], sn[i].v[2]);
	k += snprintf(b + k, sizeof(b) - (size_t)k, " n=%u,%u,%u\n", sn[0].n, sn[1].n, sn[2].n);
	int fd = open(OUT_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;
	if (write(fd, b, (size_t)k) == k)
		rename(OUT_FILE ".tmp", OUT_FILE);
	close(fd);
}

void ssc_tick(void)
{
	static uint64_t next_retry, next_out, next_sum;
	uint64_t now = link_ms();
	if (ssc_svc == 0xffffffff)
		return;
	if (now >= next_retry) {
		next_retry = now + 3000;
		for (unsigned i = 0; i < NS; i++) {
			struct sens *s = &sn[i];
			if (!s->found && s->tries < 100)
				lookup(s);
			else if (s->found && !s->n && now - s->enable_ms > 15000 && s->reenables < 3) {
				LOGT("ssc: no %s samples 15 s after enable: enabling again\n", s->type);
				s->reenables++;
				enable(s);
			}
		}
	}
	if ((sn[0].n || sn[1].n || sn[2].n) && now >= next_out) {
		next_out = now + 200;
		write_out();
	}
	if (now >= next_sum) {
		next_sum = now + 15000;
		LOGT("ssc:");
		for (unsigned i = 0; i < NS; i++)
			printf(" %s %s n=%u (%.3f %.3f %.3f)", sn[i].type, sn[i].found ? (sn[i].n ? "streaming" : "enabled") : "not found",
			       sn[i].n, sn[i].v[0], sn[i].v[1], sn[i].v[2]);
		printf("\n");
	}
}
#endif
