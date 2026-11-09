/*
 * btd: hotdogb (OnePlus 7T) Bluetooth controller bring-up from iOS userland (stage 1, no iOS stack).
 *
 * WCN3998 on QUPv3 UART SE13 (uart.c). Sequence as Linux hci_qca.c / btqca.c for wcn399x:
 *   supplies + rfclka2 -> pulse 0xC0 at 2400 baud -> pulse 0xFC at 115200 -> EDL version (0xFC00 0x19)
 *   -> 3 Mbaud (0xFC48) -> rampatch crbtfwRR.tlv -> NVM crnvRR.bin (in-band sleep / deep sleep cleared)
 *   -> HCI reset -> local version -> passive LE scan + inquiry -> power off.
 * Only counts and vendor prefixes of public addresses are logged.
 *
 *   btd regs           read-only register dump
 *   btd scan [SECS]    bring up, scan SECS each (default 10), power off
 *   btd auto           boot job: skip if /var/local/btd.off, wait 40 s, scan
 *   btd serve          stage 2 boot job (KeepAlive): skip if /var/local/btd.off, wait 40 s, bring up (one retry), then
 *                      serve bluetoothd through the kernel UART bridge (pipe.c, kernel 0165) and log its HCI
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../wifi/pmem.h"
#include "uart.h"
#include "pipe.h"

#define FW_DIR "/var/local/fw/bt"

static uint64_t t0;

static uint64_t ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

#define LOG(...) do { printf("[%7.3f] ", (double)(ms_now() - t0) / 1000.0); printf(__VA_ARGS__); } while (0)

/* ---- power ---- */

static int bt_power(void)
{
	static const struct { const char *res; uint32_t mv; } ldo[] = {
		{ "ldoa7", 1800 }, { "ldoc2", 1304 }, { "ldoc11", 3312 },
	};
	for (unsigned i = 0; i < sizeof(ldo) / sizeof(ldo[0]); i++) {
		uint32_t off[3] = { 0, 8, 4 }, val[3] = { ldo[i].mv, 7, 1 };
		int rc = pm_rpmh(ldo[i].res, 3, off, val);
		LOG("bt power: %s %u mV -> %d\n", ldo[i].res, ldo[i].mv, rc);
		if (rc)
			return -1;
	}
	uint32_t off[1] = { 4 }, val[1] = { 1 };
	int rc = pm_rpmh("rfclka2", 1, off, val);
	LOG("bt power: rfclka2 -> %d\n", rc);
	return rc ? -1 : 0;
}

static int pulse(uint8_t b, unsigned baud)
{
	if (uart_set_baud(baud))
		return -1;
	uart_flow(0);
	int rc = uart_write(&b, 1, 200);
	LOG("power pulse %#04x at %u baud -> %d\n", b, baud, rc);
	return rc;
}

/* ---- H4 ---- */

static uint8_t rxb[8192];
static size_t rxn;
static unsigned junk;

static void consume(size_t n)
{
	memmove(rxb, rxb + n, rxn - n);
	rxn -= n;
}

/* next H4 packet within ms: type (2 ACL, 3 SCO, 4 event), body without the type byte; 0 = none */
static int h4_next(uint8_t *pkt, size_t *len, unsigned ms)
{
	uint64_t end = ms_now() + ms;
	for (;;) {
		if (rxn < sizeof(rxb) - 512)
			rxn += uart_read(rxb + rxn, sizeof(rxb) - rxn);
		while (rxn && rxb[0] != 2 && rxb[0] != 3 && rxb[0] != 4) {
			junk++;
			consume(1);
		}
		size_t need = 0;
		if (rxn >= 3 && rxb[0] == 4)
			need = 3 + rxb[2];
		else if (rxn >= 5 && rxb[0] == 2)
			need = 5 + (rxb[3] | (rxb[4] << 8));
		else if (rxn >= 4 && rxb[0] == 3)
			need = 4 + rxb[3];
		if (need > sizeof(rxb) - 512) {
			junk++;
			consume(1);
			continue;
		}
		if (need && rxn >= need) {
			int t = rxb[0];
			memcpy(pkt, rxb + 1, need - 1);
			*len = need - 1;
			consume(need);
			return t;
		}
		if (ms_now() >= end)
			return 0;
		usleep(need ? 100 : 500);
	}
}

static int hci_send(uint16_t op, const void *p, uint8_t plen)
{
	uint8_t pkt[260] = { 1, (uint8_t)op, (uint8_t)(op >> 8), plen };
	if (plen)
		memcpy(pkt + 4, p, plen);
	return uart_write(pkt, 4u + plen, 2000);
}

/* ---- scan results ---- */

#define MAXDEV 256
static struct { uint8_t a[6]; uint8_t kind; int8_t rssi; } devs[MAXDEV];
static unsigned ndev, n_le, n_classic, n_reports;

static void seen(const uint8_t *a, int classic, int public_addr, int8_t rssi)
{
	n_reports++;
	for (unsigned i = 0; i < ndev; i++)
		if (!memcmp(devs[i].a, a, 6) && devs[i].kind == classic) {
			if (rssi > devs[i].rssi)
				devs[i].rssi = rssi;
			return;
		}
	if (ndev >= MAXDEV)
		return;
	memcpy(devs[ndev].a, a, 6);
	devs[ndev].kind = (uint8_t)classic;
	devs[ndev].rssi = rssi;
	ndev++;
	if (classic)
		n_classic++;
	else
		n_le++;
	if (public_addr)
		LOG("  %s device %02x:%02x:%02x:xx:xx:xx rssi %d\n", classic ? "classic" : "LE", a[5], a[4], a[3], rssi);
	else
		LOG("  LE device (random address) rssi %d\n", rssi);
}

static void on_event(const uint8_t *ev, size_t len)
{
	if (len < 2)
		return;
	const uint8_t *p = ev + 2;
	size_t pl = ev[1];
	if (ev[0] == 0x3e && pl >= 2 && p[0] == 0x02) {	/* LE advertising report */
		unsigned n = p[1];
		size_t o = 2;
		for (unsigned i = 0; i < n && o + 9 <= pl; i++) {
			uint8_t atype = p[o + 1], dl = p[o + 8];
			if (o + 9 + dl + 1 > pl)
				break;
			seen(p + o + 2, 0, atype == 0, (int8_t)p[o + 9 + dl]);
			o += 10 + dl;
		}
	} else if ((ev[0] == 0x22 || ev[0] == 0x02) && pl >= 1) {	/* inquiry result (with RSSI) */
		unsigned n = p[0];
		for (unsigned i = 0; i < n && 1 + (i + 1) * 14 <= pl; i++) {
			const uint8_t *r = p + 1 + i * 14;
			seen(r, 1, 1, ev[0] == 0x22 ? (int8_t)r[13] : 0);
		}
	} else if (ev[0] == 0x2f && pl >= 15) {	/* extended inquiry result */
		seen(p + 1, 1, 1, (int8_t)p[14]);
	} else if (ev[0] == 0x10) {
		LOG("hardware error event %#x\n", pl ? p[0] : 0);
	}
}

/*
 * Send op; wait for want: 0x0e = its Command Complete (return params from status on), 0x0f = its Command
 * Status, 0xff = a vendor event (body after the event header). Returns the length copied to out, -1 on error.
 */
static int hci_cmd(uint16_t op, const void *p, uint8_t plen, uint8_t want, uint8_t *out, size_t outmax, unsigned ms)
{
	uint8_t pkt[1100];
	size_t len;
	if (hci_send(op, p, plen))
		return -1;
	uint64_t end = ms_now() + ms;
	while (ms_now() < end) {
		int t = h4_next(pkt, &len, (unsigned)(end - ms_now()) + 1);
		if (t != 4)
			continue;
		uint8_t e = pkt[0];
		const uint8_t *b = pkt + 2;
		size_t bl = pkt[1];
		if (e == 0x0e && bl >= 3 && (b[1] | (b[2] << 8)) == op) {
			if (want == 0x0e) {
				size_t n = bl - 3 < outmax ? bl - 3 : outmax;
				if (out)
					memcpy(out, b + 3, n);
				return (int)n;
			}
			continue;
		}
		if (e == 0x0f && bl >= 4 && (b[2] | (b[3] << 8)) == op) {
			if (b[0]) {
				LOG("command %#06x: status %#x\n", op, b[0]);
				return -1;
			}
			if (want == 0x0f)
				return 0;
			continue;
		}
		if (e == 0xff && want == 0xff) {
			size_t n = bl < outmax ? bl : outmax;
			if (out)
				memcpy(out, b, n);
			return (int)n;
		}
		on_event(pkt, len);
	}
	LOG("command %#06x: timeout waiting for event %#x\n", op, want);
	return -1;
}

static void drain(unsigned ms)
{
	uint8_t pkt[1100];
	size_t len;
	uint64_t end = ms_now() + ms;
	while (ms_now() < end)
		if (h4_next(pkt, &len, (unsigned)(end - ms_now()) + 1) == 4)
			on_event(pkt, len);
}

/* ---- firmware ---- */

static uint8_t *load(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		LOG("%s: cannot open\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
	if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(d);
		return NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return d;
}

/* NVM tags (tag 16, len 16, 8 reserved, data): offset 4 (type 2) or 8 (type 4 files, as the 7T's) */
static void nvm_patch(uint8_t *d, size_t n, uint8_t baud_code)
{
	size_t start = 4;
	if (n > 16 && d[8] == 2 && d[9] == 0 && d[10] == 6 && d[11] == 0)
		start = 8;
	int found = 0;
	for (size_t i = start; i + 12 <= n && found < 2;) {
		unsigned tag = d[i] | (d[i + 1] << 8), tl = d[i + 2] | (d[i + 3] << 8);
		if (i + 12 + tl > n)
			break;
		uint8_t *v = d + i + 12;
		if (tag == 17 && tl >= 3) {
			LOG("nvm: HCI tag %02x %02x %02x -> ", v[0], v[1], v[2]);
			v[0] &= (uint8_t)~0x80;	/* no in-band sleep */
			v[2] = baud_code;
			printf("%02x %02x %02x\n", v[0], v[1], v[2]);
			found++;
		} else if (tag == 27 && tl >= 1) {
			LOG("nvm: deep sleep %02x -> %02x\n", v[0], v[0] & 0xfe);
			v[0] &= 0xfe;
			found++;
		}
		i += 12 + tl;
	}
	if (found < 2)
		LOG("nvm: found %d of the 2 tags to patch\n", found);
}

static int download(const char *path, int nvm, uint8_t baud_code)
{
	size_t n;
	uint8_t *d = load(path, &n);
	if (!d)
		return -1;
	int skip_acks = 0;
	if (!nvm && n >= 13)
		skip_acks = d[12] == 3 || d[12] == 1;	/* download mode: VSE (and CC) skipped */
	else if (nvm)
		nvm_patch(d, n, baud_code);
	LOG("download %s: %zu bytes, %s\n", path, n, skip_acks ? "last segment acked" : "every segment acked");
	uint64_t t = ms_now();
	int rc = 0;
	for (size_t o = 0; o < n;) {
		size_t seg = n - o < 243 ? n - o : 243;
		uint8_t cmd[245];
		cmd[0] = 0x1e;
		cmd[1] = (uint8_t)seg;
		memcpy(cmd + 2, d + o, seg);
		o += seg;
		int last = o >= n || seg < 243;
		if (skip_acks && !last) {
			if (hci_send(0xfc00, cmd, (uint8_t)(seg + 2))) {
				rc = -1;
				break;
			}
			continue;
		}
		uint8_t r[8];
		int l = hci_cmd(0xfc00, cmd, (uint8_t)(seg + 2), 0xff, r, sizeof(r), 3000);
		if (l < 3 || r[0] != 0 || r[1] != 0x04 || r[2] != 0) {
			LOG("segment at %zu: bad ack (len %d %02x %02x %02x)\n", o - seg, l, r[0], r[1], r[2]);
			rc = -1;
			break;
		}
	}
	free(d);
	LOG("download %s: %s in %llu ms\n", path, rc ? "FAILED" : "done", (unsigned long long)(ms_now() - t));
	return rc;
}

/* ---- bring-up ---- */

static int bt_up(void)
{
	if (bt_power() || uart_init())
		return -1;
	if (pulse(0xc0, 2400))
		return -1;
	usleep(10000);
	if (pulse(0xfc, 115200))
		return -1;
	usleep(100000);
	uart_flush_rx();
	rxn = 0;
	uart_flow(1);

	uint8_t v[32];
	uint8_t q = 0x19;
	int l = hci_cmd(0xfc00, &q, 1, 0xff, v, sizeof(v), 1000);
	if (l < 14 || v[0] != 0 || v[1] != 0x02) {
		LOG("no EDL version answer (len %d, junk bytes %u): the controller did not wake\n", l, junk);
		return -1;
	}
	uint32_t product = v[2] | v[3] << 8 | v[4] << 16 | (uint32_t)v[5] << 24;
	unsigned patch = v[6] | v[7] << 8, rom = v[8] | v[9] << 8;
	uint32_t soc = v[10] | v[11] << 8 | v[12] << 16 | (uint32_t)v[13] << 24;
	/*
	 * File suffix: by SoC id for the WCN399x ("cherokee") versions on the 7T's bluetooth_a (Android's
	 * CHEROKEE_VER_2_0 / 2_1); otherwise btqca's rom formula. k0162-bt-2: soc 0x40010224 reports rom 0x1001,
	 * which the formula makes "01" - no such file; crbtfw21.tlv's header says rom 0x201.
	 */
	unsigned romv = soc == 0x40010224 ? 0x21 : soc == 0x40010214 ? 0x20 : (((rom & 0xf00) >> 4) | (rom & 0xf));
	LOG("#### BLUETOOTH CONTROLLER ANSWERS: product %#x soc %#x rom %#x patch %#x (files %02x)\n",
	    product, soc, rom, patch, romv);

	/* operating speed: 3 Mbaud if the SE clock makes it, else stay at 115200 */
	uint8_t code = 0;
	if (uart_baud_ok(3000000)) {
		uint8_t b = 14;
		uart_flow(0);
		hci_send(0xfc48, &b, 1);
		usleep(10000);
		if (uart_set_baud(3000000))
			return -1;
		uart_flow(1);
		drain(100);	/* the baud rate vendor event */
		code = 14;
	} else {
		LOG("no clock for 3 Mbaud: staying at 115200\n");
	}

	char path[128];
	snprintf(path, sizeof(path), FW_DIR "/crbtfw%02x.tlv", romv);
	if (download(path, 0, code))
		return -1;
	usleep(10000);
	snprintf(path, sizeof(path), FW_DIR "/crnv%02x.bin", romv);
	if (download(path, 1, code))
		return -1;
	uint8_t st[4];
	if (hci_cmd(0x0c03, NULL, 0, 0x0e, st, sizeof(st), 2000) < 1 || st[0]) {
		LOG("HCI reset failed\n");
		return -1;
	}
	uint8_t lv[16];
	l = hci_cmd(0x1001, NULL, 0, 0x0e, lv, sizeof(lv), 1000);
	if (l >= 9 && !lv[0])
		LOG("#### BLUETOOTH UP: HCI %u rev %#x, LMP %u subversion %#x, manufacturer %u\n", lv[1],
		    lv[2] | lv[3] << 8, lv[4], lv[7] | lv[8] << 8, lv[5] | lv[6] << 8);
	return 0;
}

static void bt_scan(unsigned secs)
{
	uint8_t st[8];
	const uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };	/* default + LE meta */
	hci_cmd(0x0c01, mask, 8, 0x0e, st, sizeof(st), 1000);
	const uint8_t params[7] = { 0x00, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };	/* passive, 60/30 ms */
	hci_cmd(0x200b, params, 7, 0x0e, st, sizeof(st), 1000);
	const uint8_t on[2] = { 1, 0 }, off[2] = { 0, 0 };
	if (hci_cmd(0x200c, on, 2, 0x0e, st, sizeof(st), 1000) < 1 || st[0])
		LOG("LE scan enable failed\n");
	LOG("LE scan for %u s\n", secs);
	drain(secs * 1000);
	hci_cmd(0x200c, off, 2, 0x0e, st, sizeof(st), 1000);

	uint8_t mode = 1;
	hci_cmd(0x0c45, &mode, 1, 0x0e, st, sizeof(st), 1000);
	unsigned units = (secs * 100 + 127) / 128;
	uint8_t inq[5] = { 0x33, 0x8b, 0x9e, (uint8_t)(units < 1 ? 1 : units > 0x30 ? 0x30 : units), 0 };
	LOG("inquiry for %u s\n", secs);
	if (hci_cmd(0x0401, inq, 5, 0x0f, NULL, 0, 1000) == 0)
		drain(secs * 1000 + 1500);
	LOG("#### BLUETOOTH SCAN: %u LE devices, %u classic devices (%u reports, %u junk bytes)\n", n_le, n_classic,
	    n_reports, junk);
}

/* ---- serve: bluetoothd <-> kernel bridge <-> UART ---- */

static const char *pkt_name(int t)
{
	return t == 1 ? "cmd" : t == 2 ? "acl" : t == 3 ? "sco" : t == 4 ? "evt" : "?";
}

/* reassemble H4 packets of one direction for the log (the bytes themselves pass unchanged) */
struct h4log {
	const char *dir;
	uint8_t b[1100];
	size_t n;
	unsigned logged, pkts, bytes, junk;
};

static void h4log_pkt(struct h4log *l, const uint8_t *p, size_t len)
{
	l->pkts++;
	/* refused commands are logged always: each is a candidate for the local answers below */
	if (p[0] == 4 && len >= 7 && ((p[1] == 0x0e && p[6]) || (p[1] == 0x0f && p[3])))
		LOG("%s REFUSED %#06x status %#x (%s)\n", l->dir, p[1] == 0x0e ? p[4] | p[5] << 8 : p[5] | p[6] << 8,
		    p[1] == 0x0e ? p[6] : p[3], p[1] == 0x0e ? "cmd complete" : "cmd status");
	if (l->logged >= 400)
		return;
	if (p[0] == 1 && len >= 4) {
		unsigned op = p[1] | p[2] << 8;
		LOG("%s cmd %#06x (ogf %#x ocf %#05x) len %u%s\n", l->dir, op, op >> 10, op & 0x3ff, p[3],
		    (op >> 10) == 0x3f ? " VENDOR" : "");
	} else if (p[0] == 4 && len >= 3) {
		if (p[1] == 0x0e && len >= 7)
			LOG("%s evt cmd complete %#06x status %#x len %u\n", l->dir, p[4] | p[5] << 8, p[6], p[2]);
		else if (p[1] == 0x0f && len >= 7)
			LOG("%s evt cmd status %#06x status %#x\n", l->dir, p[5] | p[6] << 8, p[3]);
		else if (p[1] == 0x3e && len >= 4)
			LOG("%s evt LE meta sub %#x len %u\n", l->dir, p[3], p[2]);
		else
			LOG("%s evt %#04x len %u\n", l->dir, p[1], p[2]);
	} else {
		LOG("%s %s len %zu\n", l->dir, pkt_name(p[0]), len);
	}
	if (++l->logged == 400)
		LOG("%s: logged 400 packets, counting only from here\n", l->dir);
}

static void h4log(struct h4log *l, const uint8_t *d, size_t len)
{
	l->bytes += (unsigned)len;
	for (size_t i = 0; i < len; i++) {
		if (l->n == 0 && (d[i] < 1 || d[i] > 4)) {
			l->junk++;
			continue;
		}
		if (l->n < sizeof(l->b))
			l->b[l->n] = d[i];
		l->n++;
		size_t need = 0;
		uint8_t *b = l->b;
		if (b[0] == 1 && l->n >= 4)
			need = 4u + b[3];
		else if (b[0] == 4 && l->n >= 3)
			need = 3u + b[2];
		else if (b[0] == 2 && l->n >= 5)
			need = 5u + (b[3] | b[4] << 8);
		else if (b[0] == 3 && l->n >= 4)
			need = 4u + b[3];
		if (need && l->n >= need) {
			h4log_pkt(l, b, need < sizeof(l->b) ? need : sizeof(l->b));
			l->n = 0;
		}
	}
}

/*
 * Commands bluetoothd sends that the WCN3998 does not take; btd answers them itself (Command Complete, status 0,
 * plus any return bytes) and they never reach the chip. bluetoothd aborts its HCI init on any refusal
 * ("HCI initialization failed", reason 108).
 *   0x0c5b Write Default Erroneous Data Reporting: refused (k0165-bt-8, OI status 628); setting only.
 */
static const struct local_answer {
	uint16_t op;
	uint8_t ret_len;
	uint8_t ret[8];
	uint8_t octet, bit;	/* its bit in the Read Local Supported Commands mask (Core spec vol 4 part E 6.27) */
} local_answers[] = {
	{ 0x0c5b, 0, { 0 }, 18, 3 },
};

/*
 * bluetoothd checks that mask before it sends a command and fails its HCI init when one it needs is missing, without
 * sending it (k0165-bt-9: "WriteDefaultErroneousDataReporting (status=628)", the chip's octet 18 is 0x83). Set the
 * bits of the commands btd answers in the chip's reply: 04 0e 44 <ncmd> 02 10 <status> <64 octets>. Applied to whole
 * packets (ctlr_feed): k0165-bt-11 showed the reply split across reads after a bluetoothd restart.
 */
static void patch_supported_commands(uint8_t *d, size_t len)
{
	if (len < 71 || d[0] != 4 || d[1] != 0x0e || d[2] != 0x44 || d[4] != 0x02 || d[5] != 0x10 || d[6])
		return;
	uint8_t *mask = d + 7;
	for (size_t k = 0; k < sizeof(local_answers) / sizeof(local_answers[0]); k++)
		mask[local_answers[k].octet] |= (uint8_t)(1u << local_answers[k].bit);
	LOG("<ctlr supported commands: set the bits of %zu commands btd answers\n",
	    sizeof(local_answers) / sizeof(local_answers[0]));
}


/* the host's H4 stream, cut into packets: answered here or passed to the UART */
struct h4host {
	uint8_t b[1100];
	size_t n;
	unsigned answered;
};

/*
 * bluetoothd's vendor commands (OGF 0x3f) are Broadcom/Apple ones ("BD_VSC_*", the "Olympic" set on 0xfce2 with a
 * sub-opcode first); the WCN3998 refuses them all (status 1, no return data). Most refusals bluetoothd logs and
 * skips ("completed with status 701"), but BD_VSC_OLYMPIC_ANTENNA_PREF_B (0xfce2, 10 bytes) wants return data:
 * "Invalid data size 0", 15 s timeout, restart (reason 3602, k0165-bt-10). btd answers every one of them: status 0,
 * then the first parameter byte (the Olympic sub-opcode) echoed. btd's own Qualcomm vendor commands go straight to
 * the UART (hci_cmd), not through here.
 */
static unsigned vendor_answered;

static int answer_vendor(const uint8_t *p)
{
	unsigned op = p[1] | p[2] << 8;
	if ((op >> 10) != 0x3f)
		return 0;
	/* return data: the parameters echoed whole. _BCM_ByteStreamEventParser wants more than 1 byte after the
	 * status (0x10012617c: cmp #1, b.gt; "Invalid data size 1" with only the sub-opcode, k0165-bt-11) */
	uint8_t e[7 + 255] = { 4, 0x0e, 4, 1, p[1], p[2], 0 };
	size_t plen = p[3] > 251 ? 251 : p[3];
	memcpy(e + 7, p + 4, plen);
	e[2] = (uint8_t)(4 + plen);
	size_t n = 7 + plen;
	if (vendor_answered++ < 100)
		LOG("host> vendor cmd %#06x len %u (first byte %#04x) answered by btd\n", op, p[3], p[3] ? p[4] : 0);
	if (pipe_write(e, n))
		LOG("host write of a vendor answer failed\n");
	return 1;
}

static void host_pkt(const uint8_t *p, size_t len, struct h4host *h)
{
	if (p[0] == 1 && len >= 4 && answer_vendor(p))
		return;
	if (p[0] == 1 && len >= 4) {
		unsigned op = p[1] | p[2] << 8;
		for (size_t i = 0; i < sizeof(local_answers) / sizeof(local_answers[0]); i++) {
			const struct local_answer *a = &local_answers[i];
			if (a->op != op)
				continue;
			uint8_t e[16] = { 4, 0x0e, (uint8_t)(4 + a->ret_len), 1, p[1], p[2], 0 };
			memcpy(e + 7, a->ret, a->ret_len);
			if (h->answered++ < 50)
				LOG("host> cmd %#06x answered by btd (not supported by the chip)\n", op);
			if (pipe_write(e, 7u + a->ret_len))
				LOG("host write of a local answer failed\n");
			return;
		}
	}
	if (uart_write(p, len, 2000))
		LOG("UART write of %zu bytes failed\n", len);
}

static void host_feed(struct h4host *h, const uint8_t *d, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (h->n == 0 && (d[i] < 1 || d[i] > 4))
			continue;	/* junk between packets: dropped (counted by h4log) */
		if (h->n >= sizeof(h->b)) {
			uart_write(h->b, h->n, 2000);	/* longer than any packet we cut: pass through */
			h->n = 0;
		}
		h->b[h->n++] = d[i];
		size_t need = 0;
		uint8_t *b = h->b;
		if (b[0] == 1 && h->n >= 4)
			need = 4u + b[3];
		else if (b[0] == 2 && h->n >= 5)
			need = 5u + (b[3] | b[4] << 8);
		else if (b[0] == 3 && h->n >= 4)
			need = 4u + b[3];
		else if (b[0] == 4 && h->n >= 3)
			need = 3u + b[2];
		if (need && h->n >= need) {
			host_pkt(b, need, h);
			h->n = 0;
		}
	}
}

/* the chip's H4 stream, cut into packets (patched as needed) before they go to the host; bytes between packets are
 * dropped (h4log counts them as junk) */
static void ctlr_feed(struct h4host *c, const uint8_t *d, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (c->n == 0 && (d[i] < 1 || d[i] > 4))
			continue;
		if (c->n >= sizeof(c->b)) {
			pipe_write(c->b, c->n);
			c->n = 0;
		}
		c->b[c->n++] = d[i];
		size_t need = 0;
		uint8_t *b = c->b;
		if (b[0] == 4 && c->n >= 3)
			need = 3u + b[2];
		else if (b[0] == 2 && c->n >= 5)
			need = 5u + (b[3] | b[4] << 8);
		else if (b[0] == 3 && c->n >= 4)
			need = 4u + b[3];
		else if (b[0] == 1 && c->n >= 4)
			need = 4u + b[3];
		if (need && c->n >= need) {
			patch_supported_commands(b, need);
			if (pipe_write(b, need))
				LOG("host write of %zu bytes failed (no host, or it is not reading)\n", need);
			c->n = 0;
		}
	}
}

static int bt_serve(void)
{
	static struct h4host host, ctlr;
	static uint8_t buf[64 * 1024];
	struct h4log up = { .dir = "host>", .n = 0 }, down = { .dir = "<ctlr", .n = 0 };
	uint64_t speed = 0, last_speed = 0, next_stat = ms_now() + 10000;
	if (pipe_open()) {
		LOG("#### BLUETOOTH PIPE FAILED\n");
		return -1;
	}
	LOG("#### BLUETOOTH SERVING bluetoothd (H4 at 3 Mbaud)\n");
	for (;;) {
		int busy = 0;
		size_t n = pipe_read(buf, sizeof(buf));
		if (n) {
			busy = 1;
			h4log(&up, buf, n);
			host_feed(&host, buf, n);
		}
		n = uart_read(buf, sizeof(buf));
		if (n) {
			busy = 1;
			h4log(&down, buf, n);
			ctlr_feed(&ctlr, buf, n);
		}
		if (ms_now() >= next_stat) {
			next_stat = ms_now() + 10000;
			if (!pipe_termios_speed(&speed) && speed != last_speed) {
				LOG("bluetoothd termios speed %llu\n", (unsigned long long)speed);
				last_speed = speed;
			}
			uint32_t hs[4] = { 0 };
			pipe_host(hs);
			LOG("stats: host> %u packets %u bytes, <ctlr %u packets %u bytes, junk %u/%u; host unit %u pid %u, "
			    "drops %u/%u\n", up.pkts, up.bytes, down.pkts, down.bytes, up.junk, down.junk, hs[0], hs[1], hs[2],
			    hs[3]);
		}
		if (!busy)
			usleep(300);
	}
	return 0;
}

static void bt_off(void)
{
	uint8_t st[4];
	hci_cmd(0x0c03, NULL, 0, 0x0e, st, sizeof(st), 1000);
	pulse(0xc0, 2400);
	LOG("controller off\n");
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	t0 = ms_now();
	if (argc < 2) {
		fprintf(stderr, "usage: btd regs | scan [SECS] | auto\n");
		return 2;
	}
	if (!strcmp(argv[1], "regs")) {
		uart_regs();
		return 0;
	}
	unsigned secs = 10;
	int serve = !strcmp(argv[1], "serve");
	if (serve) {
		struct stat sb;
		while (!stat("/var/local/btd.off", &sb)) {	/* KeepAlive job: idle, do not respawn */
			printf("btd: /var/local/btd.off exists, idle\n");
			sleep(600);
		}
		printf("btd: serve: waiting 40 s\n");
		sleep(40);
		t0 = ms_now();
	} else if (!strcmp(argv[1], "auto")) {
		struct stat sb;
		if (!stat("/var/local/btd.off", &sb)) {
			printf("btd: /var/local/btd.off exists, not starting\n");
			return 0;
		}
		printf("btd: waiting 40 s for wland\n");
		sleep(40);
		t0 = ms_now();
	} else if (!strcmp(argv[1], "scan")) {
		if (argc > 2)
			secs = (unsigned)atoi(argv[2]);
		if (secs < 1 || secs > 60)
			secs = 10;
	} else {
		fprintf(stderr, "usage: btd regs | scan [SECS] | auto | serve\n");
		return 2;
	}
	LOG("#### BLUETOOTH BRING-UP (btd, pid %d)\n", getpid());
	pm_init();
	int up = bt_up();
	if (up && serve) {
		/* k0163-bt-2: the first bring-up stalled mid-rampatch (TX held off); the next one worked */
		LOG("bring-up failed: power off, retry once\n");
		bt_off();
		sleep(2);
		rxn = 0;
		up = bt_up();
	}
	if (up) {
		LOG("#### BLUETOOTH FAILED\n");
		bt_off();
		if (serve)
			sleep(60);	/* KeepAlive: retry in a minute, not every 10 s */
		return 1;
	}
	if (serve) {
		bt_serve();
		pipe_close();
		bt_off();
		sleep(30);
		return 1;
	}
	bt_scan(secs);
	bt_off();
	return 0;
}
