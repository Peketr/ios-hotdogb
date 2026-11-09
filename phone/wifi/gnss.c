/*
 * GNSS: a QMI LOC client (service 0x10) for the modem's GPS engine, the way Android's LocApiV02 and
 * libqmi's qmi-service-loc start one: inform client revision 2, register for position reports + NMEA +
 * engine/session state, standalone operation mode (no SUPL here), then a periodic 1 s session.
 * Fixes go to GNSS_FIX (one key=value line, replaced atomically), the latest NMEA sentences to
 * GNSS_NMEA, so an iOS-side injector can feed locationd. WLAND_NO_GPS=1 leaves it off.
 *
 * Wire format: loc_api_v02.h (QMI_LOC_*_V02); TLV numbers checked against libqmi's qmi-service-loc.json.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gnss.h"
#include "qmi.h"

#define LOC_INFORM_CLIENT_REVISION	0x0020
#define LOC_REG_EVENTS			0x0021
#define LOC_START			0x0022
#define LOC_STOP			0x0023
#define LOC_POSITION_REPORT_IND		0x0024
#define LOC_SV_INFO_IND			0x0025
#define LOC_NMEA_IND			0x0026
#define LOC_INJECT_TIME_REQ_IND		0x0028
#define LOC_ENGINE_STATE_IND		0x002b
#define LOC_FIX_SESSION_STATE_IND	0x002c
#define LOC_SET_OPERATION_MODE		0x004a

#define EV_POSITION_REPORT	0x001ull
#define EV_NMEA			0x004ull
#define EV_ENGINE_STATE		0x080ull
#define EV_FIX_SESSION_STATE	0x100ull

#define OPER_MODE_STANDALONE	4
#define SESSION_ID		1

#define GNSS_FIX	"/var/local/gps.fix"
#define GNSS_NMEA	"/var/local/gps.nmea"
#define START_DELAY_MS	20000	/* after the server appears: let WLFW and the radio come up first */

static uint32_t srv_node, srv_port;
static int srv_up, step, disabled;
static uint64_t srv_ms, sent_ms;
static uint16_t txn = 1, wait_txn;
static int waiting;

/* what we know; written out by gnss_poll */
static struct {
	int valid;
	double lat, lon;
	float alt, hacc, vacc, speed, course;
	int have_alt, have_vacc, have_speed, have_course;
	uint64_t utc_ms;
	uint32_t status, tech;
	unsigned reports, fixes;
} fix;
static int sats_view[8], sats_used;
static const char *talkers[8] = { "GP", "GL", "GA", "GB", "BD", "GQ", "QZ", "GN" };
static char nmea_ring[32][100];
static unsigned nmea_head, nmea_count, nmea_dirty;
static uint64_t last_write_ms, last_log_ms;

void gnss_new_server(uint32_t node, uint32_t port, uint32_t instance)
{
	if (getenv("WLAND_NO_GPS")) {
		disabled = 1;
		return;
	}
	srv_node = node;
	srv_port = port;
	srv_up = 1;
	srv_ms = link_ms();
	step = 0;
	printf("gnss: LOC server at %u:%x (instance %#x); session in %u s\n", node, port, instance, START_DELAY_MS / 1000);
}

static void send_req(struct qmi_buf *q, const char *what)
{
	memcpy(q->b + 1, &txn, 2);
	wait_txn = txn++;
	waiting = 1;
	sent_ms = link_ms();
	size_t n = qmi_finish(q);
	printf("gnss: > %s\n", what);
	if (n)
		qrtr_send(QRTR_TYPE_DATA, GNSS_PORT, srv_node, srv_port, q->b, (uint32_t)n);
}

/* the bring-up, one request at a time; each step waits for the previous response (2 s at most) */
static void next_step(void)
{
	struct qmi_buf q;
	switch (step) {
	case 0:
		printf("#### STARTING GPS\n");
		qmi_start(&q, QMI_REQUEST, 0, LOC_INFORM_CLIENT_REVISION);
		qmi_tlv_u32(&q, 0x01, 2);
		send_req(&q, "inform client revision 2");
		break;
	case 1:
		qmi_start(&q, QMI_REQUEST, 0, LOC_REG_EVENTS);
		qmi_tlv_u64(&q, 0x01, EV_POSITION_REPORT | EV_NMEA | EV_ENGINE_STATE | EV_FIX_SESSION_STATE);
		send_req(&q, "register events (position, NMEA, engine, session)");
		break;
	case 2:
		qmi_start(&q, QMI_REQUEST, 0, LOC_SET_OPERATION_MODE);
		qmi_tlv_u32(&q, 0x01, OPER_MODE_STANDALONE);
		send_req(&q, "operation mode standalone");
		break;
	case 3:
		qmi_start(&q, QMI_REQUEST, 0, LOC_START);
		qmi_tlv_u8(&q, 0x01, SESSION_ID);
		qmi_tlv_u32(&q, 0x10, 1);	/* fix recurrence: periodic */
		qmi_tlv_u32(&q, 0x11, 3);	/* horizontal accuracy: high */
		qmi_tlv_u32(&q, 0x12, 1);	/* intermediate reports: on */
		qmi_tlv_u32(&q, 0x13, 1000);	/* minimum interval, ms */
		send_req(&q, "start session 1 (periodic 1 s, high accuracy)");
		break;
	default:
		return;
	}
	step++;
}

static void ring_add(const char *s, size_t n)
{
	while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || !s[n - 1]))
		n--;
	if (n >= sizeof(nmea_ring[0]))
		n = sizeof(nmea_ring[0]) - 1;
	memcpy(nmea_ring[nmea_head], s, n);
	nmea_ring[nmea_head][n] = 0;
	nmea_head = (nmea_head + 1) % 32;
	nmea_count++;
	nmea_dirty = 1;
}

/* field k (0 = the sentence name) of an NMEA sentence, copied into out */
static int nmea_field(const char *s, int k, char *out, size_t sz)
{
	for (int i = 0; i < k; i++) {
		s = strchr(s, ',');
		if (!s)
			return -1;
		s++;
	}
	size_t n = strcspn(s, ",*");
	if (n >= sz)
		n = sz - 1;
	memcpy(out, s, n);
	out[n] = 0;
	return (int)n;
}

static void nmea_one(const char *s, size_t n)
{
	static int first = 1;
	char line[100], f[16];
	if (n >= sizeof(line))
		n = sizeof(line) - 1;
	memcpy(line, s, n);
	line[n] = 0;
	if (first) {
		printf("#### FIRST GPS NMEA SENTENCE: %s\n", line);
		first = 0;
	}
	ring_add(line, n);
	if (n < 7 || line[0] != '$')
		return;
	if (!strncmp(line + 3, "GSV", 3) && nmea_field(line, 3, f, sizeof(f)) > 0) {
		for (int t = 0; t < 8; t++)
			if (!strncmp(line + 1, talkers[t], 2))
				sats_view[t] = atoi(f);
	} else if (!strncmp(line + 3, "GGA", 3) && nmea_field(line, 7, f, sizeof(f)) >= 0) {
		sats_used = atoi(f);
	}
}

static void on_nmea(const struct qmi_msg *m)
{
	uint16_t l;
	const uint8_t *p = qmi_get(m, 0x01, &l);
	if (!p)
		return;
	/* one TLV may carry several sentences; skip any length prefix before the first '$' */
	const char *s = memchr(p, '$', l), *end = (const char *)p + l;
	while (s && s < end) {
		const char *e = s + 1;
		while (e < end && *e != '$' && *e != '\n' && *e)
			e++;
		nmea_one(s, (size_t)(e - s));
		s = memchr(e, '$', (size_t)(end - e));
	}
}

static int get_f32(const struct qmi_msg *m, uint8_t t, float *v)
{
	uint16_t l;
	const uint8_t *p = qmi_get(m, t, &l);
	if (!p || l < 4)
		return 0;
	memcpy(v, p, 4);
	return 1;
}

static int get_f64(const struct qmi_msg *m, uint8_t t, double *v)
{
	uint16_t l;
	const uint8_t *p = qmi_get(m, t, &l);
	if (!p || l < 8)
		return 0;
	memcpy(v, p, 8);
	return 1;
}

static void on_position(const struct qmi_msg *m)
{
	uint32_t status = 0;
	double lat, lon;
	qmi_get_u32(m, 0x01, &status);	/* 0 success, 1 in progress, 2 failure, 3 timeout, ... */
	fix.reports++;
	fix.status = status;
	if (!get_f64(m, 0x10, &lat) || !get_f64(m, 0x11, &lon) || !isfinite(lat) || !isfinite(lon))
		return;
	if (status != 0 && status != 1)
		return;
	fix.lat = lat;
	fix.lon = lon;
	if (!get_f32(m, 0x12, &fix.hacc))
		fix.hacc = -1;
	fix.have_speed = get_f32(m, 0x18, &fix.speed);
	fix.have_alt = get_f32(m, 0x1b, &fix.alt) || get_f32(m, 0x1a, &fix.alt);	/* MSL, else ellipsoid */
	fix.have_vacc = get_f32(m, 0x1c, &fix.vacc);
	fix.have_course = get_f32(m, 0x20, &fix.course);
	uint16_t l;
	const uint8_t *p = qmi_get(m, 0x25, &l);
	if (p && l >= 8)
		memcpy(&fix.utc_ms, p, 8);
	qmi_get_u32(m, 0x23, &fix.tech);
	if (!fix.valid)
		printf("#### FIRST GPS FIX: %.6f %.6f +-%.0f m (status %u, tech %#x)\n", lat, lon, fix.hacc, status, fix.tech);
	fix.valid = 1;
	fix.fixes++;
}

void gnss_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	if (qmi_parse(d, n, &m))
		return;
	if (m.type == QMI_RESPONSE) {
		uint16_t l, result = 0, error = 0;
		const uint8_t *res = qmi_get(&m, 2, &l);
		if (res && l >= 4) {
			memcpy(&result, res, 2);
			memcpy(&error, res + 2, 2);
		}
		printf("gnss: < response %#x: %s", m.id, result ? "FAILED, " : "ok\n");
		if (result) {
			printf("error %u: ", error);
			qmi_dump(&m);
		}
		if (m.id == LOC_START && !result)
			printf("#### GPS SESSION STARTED\n");
		if (waiting && m.txn == wait_txn)
			waiting = 0;
		return;
	}
	if (m.type != QMI_INDICATION)
		return;
	uint32_t v = 0;
	switch (m.id) {
	case LOC_NMEA_IND:
		on_nmea(&m);
		break;
	case LOC_POSITION_REPORT_IND:
		on_position(&m);
		break;
	case LOC_ENGINE_STATE_IND:
		qmi_get_u32(&m, 0x01, &v);
		printf("gnss: < engine %s\n", v == 1 ? "ON" : v == 2 ? "OFF" : "state ?");
		break;
	case LOC_FIX_SESSION_STATE_IND:
		qmi_get_u32(&m, 0x01, &v);
		printf("gnss: < fix session %s\n", v == 1 ? "STARTED" : v == 2 ? "FINISHED" : "state ?");
		break;
	case LOC_INJECT_TIME_REQ_IND:
		printf("gnss: < engine asks for time injection (not done: cold start)\n");
		break;
	default:
		printf("gnss: < indication ");
		qmi_dump(&m);
		break;
	}
}

static void write_files(uint64_t now)
{
	char tmp[64];
	int view = 0;
	for (int t = 0; t < 8; t++)
		view += sats_view[t];
	if (fix.valid) {
		snprintf(tmp, sizeof(tmp), "%s.tmp", GNSS_FIX);
		FILE *f = fopen(tmp, "w");
		if (f) {
			fprintf(f, "utc_ms=%llu lat=%.8f lon=%.8f hacc=%.1f", (unsigned long long)fix.utc_ms, fix.lat, fix.lon, fix.hacc);
			if (fix.have_alt)
				fprintf(f, " alt=%.1f", fix.alt);
			if (fix.have_vacc)
				fprintf(f, " vacc=%.1f", fix.vacc);
			if (fix.have_speed)
				fprintf(f, " speed=%.2f", fix.speed);
			if (fix.have_course)
				fprintf(f, " course=%.1f", fix.course);
			fprintf(f, " sats_used=%d sats_view=%d status=%u wland_ms=%llu\n", sats_used, view, fix.status,
				(unsigned long long)now);
			fclose(f);
			rename(tmp, GNSS_FIX);
		}
	}
	if (nmea_dirty) {
		snprintf(tmp, sizeof(tmp), "%s.tmp", GNSS_NMEA);
		FILE *f = fopen(tmp, "w");
		if (f) {
			unsigned k = nmea_count < 32 ? nmea_count : 32;
			for (unsigned i = 0; i < k; i++)
				fprintf(f, "%s\r\n", nmea_ring[(nmea_head + 32 - k + i) % 32]);
			fclose(f);
			rename(tmp, GNSS_NMEA);
		}
		nmea_dirty = 0;
	}
}

void gnss_poll(void)
{
	if (!srv_up || disabled)
		return;
	uint64_t now = link_ms();
	if (step < 4 && now >= srv_ms + START_DELAY_MS) {
		if (waiting && now > sent_ms + 2000) {
			printf("gnss: no response in 2 s, going on\n");
			waiting = 0;
		}
		if (!waiting)
			next_step();
	}
	if (now >= last_write_ms + 1000) {
		last_write_ms = now;
		write_files(now);
	}
	if (step >= 4 && now >= last_log_ms + 15000) {
		int view = 0;
		for (int t = 0; t < 8; t++)
			view += sats_view[t];
		last_log_ms = now;
		printf("gnss: %u reports, %u fixes, %u NMEA; satellites in view %d (GPS %d GLO %d GAL %d BDS %d), used %d",
		       fix.reports, fix.fixes, nmea_count, view, sats_view[0], sats_view[1], sats_view[2], sats_view[3] + sats_view[4],
		       sats_used);
		if (fix.valid)
			printf("; last %.6f %.6f +-%.0f m\n", fix.lat, fix.lon, fix.hacc);
		else
			printf("; no fix yet (needs open sky; cold start can take minutes)\n");
	}
}
