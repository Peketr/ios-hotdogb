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
static uint64_t ev_ts;		/* the current event's SSC timestamp (19.2 MHz ticks) */
static void vib_config(void);
static void vib_tick(void);
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
	pb_varint(&p, 3, s->tries & 1);	/* alternate: default sensor only / every sensor of the type */
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

/* framework sensors: timer/suid/resampler live in the root PD; registry, interrupt, async_com (the bus/IRQ
 * services the chip drivers need) come with the sensors user PD. Logged as "suid event for ..." */
static void fw_probe(void)
{
	static const char *fw[] = { "registry", "timer", "interrupt", "async_com", "suid", "resampler", "data_acquisition_engine", "dae" };
	for (unsigned i = 0; i < sizeof(fw) / sizeof(fw[0]); i++) {
		uint8_t b[64];
		struct pb p = { b, 0, sizeof(b) };
		pb_bytes(&p, 1, fw[i], strlen(fw[i]));
		pb_varint(&p, 2, 0);
		pb_varint(&p, 3, 0);
		send_req(PORT_SUID, SSC_SUID_MAGIC, SSC_SUID_MAGIC, SSC_MSG_SUID_REQ, b, p.n);
	}
}

void ssc_server(uint32_t node, uint32_t port)
{
	if (ssc_svc != 0xffffffff)
		return;
	ssc_node = node;
	ssc_svc = port;
	vib_config();
	LOGT("#### SSC SERVICE UP (sensor client service %u at %u:%x)\n", SSC_SERVICE, node, port);
	for (unsigned i = 0; i < NS; i++)
		lookup(&sn[i]);
	fw_probe();
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

/*
 * To iOS (kernel 0184, kern.sweet_motion -> SweetHIDAccel/Gyro/Compass -> locationd). SSC reports Android's device
 * frame, which has the same axes as iOS's (+x right, +y to the top, +z out of the screen); units m/s^2, rad/s, uT.
 * iOS accelerometer data is in g with the opposite sign (face up at rest: z = -1, Android +9.8). An optional
 * /var/local/sensors/axes (9 numbers per sensor, accel/gyro/mag rows of a 3x3 matrix) corrects a mounting
 * difference found on the phone. Writes fail with ENODEV unless the boot args carry sweetsensors=1.
 */
#include <errno.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
struct motion_sample { uint32_t kind; int32_t x, y, z; uint64_t t; };
static float axes[3][9] = {
	{ 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1 },
};
static int axes_loaded, kernel_feed = 1;

static void load_axes(void)
{
	axes_loaded = 1;
	FILE *f = fopen("/var/local/sensors/axes", "r");
	if (!f)
		return;
	float m[27];
	int n = 0;
	while (n < 27 && fscanf(f, "%f", &m[n]) == 1)
		n++;
	fclose(f);
	if (n == 27) {
		memcpy(axes, m, sizeof(m));
		LOGT("sensors: axes from /var/local/sensors/axes\n");
	} else
		LOGT("sensors: /var/local/sensors/axes has %d numbers, not 27: ignored\n", n);
}

static void to_ios(struct sens *s, float *v)
{
	int k = (int)(s - sn);
	if (!axes_loaded)
		load_axes();
	const float *a = axes[k];
	float o[3] = {
		a[0] * v[0] + a[1] * v[1] + a[2] * v[2],
		a[3] * v[0] + a[4] * v[1] + a[5] * v[2],
		a[6] * v[0] + a[7] * v[1] + a[8] * v[2],
	};
	if (k == 0)
		for (int i = 0; i < 3; i++)
			o[i] = -o[i] / 9.80665f;
	memcpy(v, o, sizeof(o));
	if (!kernel_feed)
		return;
	struct motion_sample ms = { (uint32_t)k, (int32_t)(o[0] * 65536), (int32_t)(o[1] * 65536), (int32_t)(o[2] * 65536),
				     mach_absolute_time() };
	if (sysctlbyname("kern.sweet_motion", NULL, NULL, &ms, sizeof(ms)) != 0) {
		LOGT("#### SENSORS: KERNEL FEED OFF (kern.sweet_motion: %s)\n", strerror(errno));
		kernel_feed = 0;
	} else if (s->n == 0)
		LOGT("#### FIRST %s SAMPLE SENT TO THE KERNEL\n", s->caps);
}

/*
 * Vibration test (/var/local/sensors/vibtest, content: the accel rate in Hz, default 400): 10 s after accel
 * streams, 12 windows of 4 s, motor off / on alternately (vib.c). Per window, without its first 0.8 s (motor
 * spin-up/brake), per accel and gyro axis: the standard deviation and the RMS of sample-to-sample differences
 * (a high-pass: the 150 Hz LRA shows there even when aliased). Every accel sample of the test goes to
 * /var/local/vib.csv (SSC ticks, window, motor, x y z in g).
 */
#include "vib.h"
#define VIB_WIN_MS	4000
#define VIB_SKIP_MS	800
#define VIB_WINS	12
static int vibtest;
static float vib_rate = 400;
static FILE *vib_csv;
static struct { unsigned n; double sum[3], sq[3], dsq[3], last[3]; } vw[2];
static int vib_win = -1, vib_on, vib_counting;
static double vib_res[2][2][2];	/* [motor][sensor][std, diff] sums of the axis-RMS values */
static unsigned vib_nres[2];

static void vib_config(void)
{
	FILE *f = fopen("/var/local/sensors/vibtest", "r");
	if (!f)
		return;
	float r = 0;
	if (fscanf(f, "%f", &r) == 1 && r >= 10 && r <= 1000)
		vib_rate = r;
	fclose(f);
	vibtest = 1;
	sn[0].rate = vib_rate;
	LOGT("#### VIBRATION TEST ARMED (ACCEL AT %.0f HZ, %d WINDOWS OF %d MS)\n", vib_rate, VIB_WINS, VIB_WIN_MS);
}

static void vib_sample(int k, const float *v)
{
	if (!vibtest || k > 1 || vib_win < 0 || vib_win >= VIB_WINS)
		return;
	if (k == 0 && vib_csv)
		fprintf(vib_csv, "%llu,%d,%d,%.6f,%.6f,%.6f\n", (unsigned long long)ev_ts, vib_win, vib_on, v[0], v[1], v[2]);
	if (!vib_counting)
		return;
	typeof(&vw[0]) w = &vw[k];
	for (int i = 0; i < 3; i++) {
		w->sum[i] += v[i];
		w->sq[i] += (double)v[i] * v[i];
		if (w->n)
			w->dsq[i] += (v[i] - w->last[i]) * (v[i] - w->last[i]);
		w->last[i] = v[i];
	}
	w->n++;
}

static void vib_close_window(void)
{
	static const char *nm[2] = { "accel", "gyro" }, *un[2] = { "g", "rad/s" };
	double sec = (VIB_WIN_MS - VIB_SKIP_MS) / 1000.0;
	for (int k = 0; k < 2; k++) {
		typeof(&vw[0]) w = &vw[k];
		double sd[3] = { 0 }, dr[3] = { 0 }, s2 = 0, d2 = 0;
		for (int i = 0; w->n > 1 && i < 3; i++) {
			double m = w->sum[i] / w->n, var = w->sq[i] / w->n - m * m;
			sd[i] = var > 0 ? __builtin_sqrt(var) : 0;
			dr[i] = __builtin_sqrt(w->dsq[i] / (w->n - 1));
			s2 += sd[i] * sd[i];
			d2 += dr[i] * dr[i];
		}
		LOGT("#### VIB %s W%d %s: n=%u (%.0f Hz) std %.5f %.5f %.5f diff-rms %.5f %.5f %.5f %s\n", vib_on ? "ON " : "OFF",
		     vib_win, nm[k], w->n, w->n / sec, sd[0], sd[1], sd[2], dr[0], dr[1], dr[2], un[k]);
		if (w->n > 1) {
			vib_res[vib_on][k][0] += __builtin_sqrt(s2);
			vib_res[vib_on][k][1] += __builtin_sqrt(d2);
		}
	}
	if (vw[0].n > 1)
		vib_nres[vib_on]++;
	memset(vw, 0, sizeof(vw));
}

static void vib_tick(void)
{
	static uint64_t t0, next;
	if (!vibtest || vib_win >= VIB_WINS || !sn[0].n)
		return;
	uint64_t now = link_ms();
	if (!t0) {
		t0 = now + 10000;
		next = t0;
		LOGT("vib: accel streams; test starts in 10 s\n");
		return;
	}
	if (vib_win >= 0 && !vib_counting && now >= next - VIB_WIN_MS + VIB_SKIP_MS) {
		memset(vw, 0, sizeof(vw));
		vib_counting = 1;
	}
	if (now < next)
		return;
	if (vib_win >= 0)
		vib_close_window();
	vib_counting = 0;
	vib_win++;
	if (vib_win == 0) {
		vib_csv = fopen("/var/local/vib.csv", "w");
		if (vib_csv)
			fprintf(vib_csv, "ssc_ticks,window,motor,x_g,y_g,z_g\n");
		if (vib_init())
			LOGT("#### VIBRATION TEST: HAPTICS INIT FAILED (motor stays off; windows still logged)\n");
	}
	if (vib_win >= VIB_WINS) {
		vib_set(0);
		if (vib_csv)
			fclose(vib_csv);
		vib_csv = NULL;
		for (int k = 0; k < 2; k++) {
			double off_s = vib_nres[0] ? vib_res[0][k][0] / vib_nres[0] : 0, on_s = vib_nres[1] ? vib_res[1][k][0] / vib_nres[1] : 0;
			double off_d = vib_nres[0] ? vib_res[0][k][1] / vib_nres[0] : 0, on_d = vib_nres[1] ? vib_res[1][k][1] / vib_nres[1] : 0;
			LOGT("#### VIB RESULT %s: noise off %.5f on %.5f (x%.1f), diff-rms off %.5f on %.5f (x%.1f)\n", k ? "gyro" : "accel",
			     off_s, on_s, off_s > 0 ? on_s / off_s : 0, off_d, on_d, off_d > 0 ? on_d / off_d : 0);
		}
		double r = vib_res[0][0][1] > 0 && vib_nres[1] ? (vib_res[1][0][1] / vib_nres[1]) / (vib_res[0][0][1] / vib_nres[0]) : 0;
		LOGT("#### VIBRATION %s IN THE ACCELEROMETER DATA (DIFF-RMS ON/OFF x%.1f)\n", r >= 2 ? "VISIBLE" : r >= 1.3 ? "WEAKLY VISIBLE" : "NOT VISIBLE", r);
		return;
	}
	vib_on = vib_win & 1;
	if (vib_set(vib_on) == 0 && vib_on)
		LOGT("vib: motor on (window %d, status %#x)\n", vib_win, vib_status());
	next += VIB_WIN_MS;
}

/*
 * Motion log (always on): rest = slow averages of accel (g, iOS frame) and gyro (the bias). The phone moves when
 * accel leaves the rest value by > 0.05 g or gyro exceeds the bias by > 0.2 rad/s; it is still again after 1 s
 * without either. Logs "#### MOTION START", a trace line at most every 250 ms while moving, and "#### MOTION END"
 * with the peaks, the rotation per axis (gyro integrated over the SSC timestamps, degrees, right-handed about +x
 * right / +y top / +z out of the screen) and the gravity direction before / after. Orientation (iOS readings:
 * y -1 = upright portrait, z -1 = face up) is logged when the dominant axis changes.
 */
#define MO_ACC_G	0.05f
#define MO_GYRO_RAD	0.2f
#define MO_QUIET_MS	1000
static struct {
	int init, moving;
	float base[3], bias[3], before[3], acc[3], gyr[3], rot[3], peak_a, peak_g;
	uint64_t start_ms, quiet_ms, trace_ms, gyro_ts;
	unsigned lines;
	const char *orient;
} mo;

static float norm3(const float *v) { return __builtin_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static const char *orientation(const float *a)
{
	static const char *nm[3][2] = { { "LEFT EDGE DOWN", "RIGHT EDGE DOWN" }, { "PORTRAIT (BOTTOM DOWN)", "UPSIDE DOWN" },
					 { "FACE UP", "FACE DOWN" } };
	float n = norm3(a);
	for (int i = 0; n > 0.5f && i < 3; i++)
		if (__builtin_fabsf(a[i]) / n > 0.75f)
			return nm[i][a[i] > 0];
	return NULL;
}

static void tilt_log(const char *what)
{
	float c = (mo.before[0] * mo.base[0] + mo.before[1] * mo.base[1] + mo.before[2] * mo.base[2]) /
		  (norm3(mo.before) * norm3(mo.base) + 1e-9f);
	if (c > 1)
		c = 1;
	if (c < -1)
		c = -1;
	LOGT("#### MOTION %s after %llu ms: peak accel %.2f g from rest, peak gyro %.2f rad/s, rotation x %+.0f y %+.0f z %+.0f deg, "
	     "gravity (%.2f %.2f %.2f) -> (%.2f %.2f %.2f) g, tilt %.0f deg\n",
	     what, (unsigned long long)(link_ms() - mo.start_ms), mo.peak_a, mo.peak_g, mo.rot[0], mo.rot[1], mo.rot[2],
	     mo.before[0], mo.before[1], mo.before[2], mo.base[0], mo.base[1], mo.base[2], __builtin_acosf(c) * 57.2958f);
}

static void motion_sample(int k, const float *v)
{
	if (k > 1)
		return;
	uint64_t now = link_ms();
	if (!mo.init) {
		if (k != 0)
			return;
		memcpy(mo.base, v, sizeof(mo.base));
		memset(mo.bias, 0, sizeof(mo.bias)); /* not sn[1].v: the first gyro sample can be junk (seen -0.54 0.18 -1.09) */
		mo.init = 1;
		mo.quiet_ms = now;
		return;
	}
	float dev[3], d;
	if (k == 0) {
		memcpy(mo.acc, v, sizeof(mo.acc));
		for (int i = 0; i < 3; i++)
			dev[i] = v[i] - mo.base[i];
		d = norm3(dev);
		if (d > mo.peak_a)
			mo.peak_a = d;
	} else {
		memcpy(mo.gyr, v, sizeof(mo.gyr));
		for (int i = 0; i < 3; i++)
			dev[i] = v[i] - mo.bias[i];
		d = norm3(dev);
		if (d > mo.peak_g)
			mo.peak_g = d;
		if (mo.moving && mo.gyro_ts && ev_ts > mo.gyro_ts) {
			float dt = (float)(ev_ts - mo.gyro_ts) / 19.2e6f;
			if (dt < 0.2f)
				for (int i = 0; i < 3; i++)
					mo.rot[i] += dev[i] * dt * 57.2958f;
		}
		mo.gyro_ts = ev_ts;
	}
	int active = k == 0 ? d > MO_ACC_G : d > MO_GYRO_RAD;
	if (!mo.moving) {
		/* still: follow slow drift (tilt creep, gyro bias) */
		float a = k == 0 ? 0.01f : 0.02f;
		float *b = k == 0 ? mo.base : mo.bias;
		if (!active && (k == 0 || norm3(v) < MO_GYRO_RAD))
			for (int i = 0; i < 3; i++)
				b[i] += a * (v[i] - b[i]);
		if (active) {
			mo.moving = 1;
			mo.start_ms = now;
			mo.quiet_ms = now;
			mo.trace_ms = 0;
			mo.lines = 0;
			memcpy(mo.before, mo.base, sizeof(mo.before));
			memset(mo.rot, 0, sizeof(mo.rot));
			mo.peak_a = k == 0 ? d : 0;
			mo.peak_g = k == 1 ? d : 0;
			LOGT("#### MOTION START (%s %.2f %s; rest accel %.2f %.2f %.2f g)\n", k ? "gyro" : "accel", d, k ? "rad/s" : "g",
			     mo.base[0], mo.base[1], mo.base[2]);
		}
		return;
	}
	if (active)
		mo.quiet_ms = now;
	if (now - mo.trace_ms >= 250 && mo.lines < 400) {
		mo.trace_ms = now;
		mo.lines++;
		LOGT("motion: accel %+.2f %+.2f %+.2f g  gyro %+.2f %+.2f %+.2f rad/s  rot %+.0f %+.0f %+.0f deg\n", mo.acc[0], mo.acc[1],
		     mo.acc[2], mo.gyr[0] - mo.bias[0], mo.gyr[1] - mo.bias[1], mo.gyr[2] - mo.bias[2], mo.rot[0], mo.rot[1], mo.rot[2]);
	}
	if (now - mo.quiet_ms >= MO_QUIET_MS) {
		/* still again: the new rest is the current accel */
		memcpy(mo.base, mo.acc, sizeof(mo.base));
		mo.moving = 0;
		tilt_log("END");
		const char *o = orientation(mo.base);
		if (o && o != mo.orient) {
			LOGT("#### ORIENTATION: %s (accel %.2f %.2f %.2f g)\n", o, mo.base[0], mo.base[1], mo.base[2]);
			mo.orient = o;
		}
	}
}

static void motion_first_orientation(void)
{
	if (mo.init && !mo.orient) {
		const char *o = orientation(mo.base);
		mo.orient = o ? o : "";
		LOGT("#### ORIENTATION AT START: %s (accel %.2f %.2f %.2f g)\n", o ? o : "IN BETWEEN", mo.base[0], mo.base[1], mo.base[2]);
	}
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
	to_ios(s, v);
	vib_sample((int)(s - sn), v);
	motion_sample((int)(s - sn), v);
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
			else if (g.field == 2 && (g.wt == 1 || g.wt == 0))
				ev_ts = g.v;
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
	vib_tick();
	if (now >= next_retry) {
		next_retry = now + 3000;
		static unsigned rounds;
		if (++rounds % 10 == 0 && rounds <= 60)
			fw_probe();
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
		motion_first_orientation();
		LOGT("ssc:");
		for (unsigned i = 0; i < NS; i++)
			printf(" %s %s n=%u (%.3f %.3f %.3f)", sn[i].type, sn[i].found ? (sn[i].n ? "streaming" : "enabled") : "not found",
			       sn[i].n, sn[i].v[0], sn[i].v[1], sn[i].v[2]);
		printf("\n");
	}
}
#endif
