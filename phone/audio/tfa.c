/*
 * The 7T's speaker amps: two NXP TFA9874 (revision 0x0c74) at I2C 0x34 / 0x35, after the vendor driver
 * (OnePlusOSS/android_kernel_oneplus_sm8150 sound/soc/codecs: tfa_dsp.c tfa_dev_start / tfaRunStartup /
 * tfa_dev_set_state, tfa_init.c tfa9874_specific, tfa_container.c). The TFA9874 has no CoolFlux DSP (its
 * container holds 9874_dummy.patch), so the driver's path is the "CF bypass" one: I2C reset, the revision's
 * optimal settings, the container's device and profile register writes, then INIT_CF (MANSCONF 1, PWDN 0,
 * wait for CLKS) and OPERATING (AMPE = unmute). Family 2, "probus" device: no SBSL.
 * The container (tuning data, not in git): /var/local/fw/tfa98xx.cnt from odm_a /firmware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "i2c.h"
#include "tfa.h"

#define CNT_PATH	"/var/local/fw/tfa98xx.cnt"

/* nxpTfaDescriptorType (tfa98xx_parameters.h) */
enum { dscDevice, dscProfile, dscRegister, dscString, dscFile, dscPatch, dscMarker, dscMode, dscBitfield = 16, dscDefault = 17 };

/* TFA9874 bitfields: (register << 8) | (position << 4) | (length - 1) (tfa9874_tfafieldnames.h) */
#define BF_PWDN		0x0000
#define BF_I2CR		0x0010
#define BF_AMPE		0x0030
#define BF_DCA		0x0040
#define BF_MANSCONF	0x0120
#define BF_PLLS		0x1160
#define BF_CLKS		0x1170
#define BF_SWS		0x11d0
#define BF_AMPS		0x11e0
#define BF_AREFS	0x11f0
#define BF_NOCLK	0x1070
#define BF_MANSTATE	0x1433
#define BF_MTPK		0xa107

static uint8_t *cnt;
static size_t cnt_len;

static int rd(uint8_t a, uint8_t reg, uint16_t *v)
{
	uint8_t b[2];
	int rc = i2c_write_read(a, &reg, 1, b, 2);
	if (!rc)
		*v = (uint16_t)((b[0] << 8) | b[1]);
	return rc;
}

static int wr(uint8_t a, uint8_t reg, uint16_t v)
{
	uint8_t b[3] = { reg, (uint8_t)(v >> 8), (uint8_t)v };
	return i2c_write(a, b, 3);
}

static int set_bf(uint8_t a, uint16_t bf, uint16_t value)
{
	uint8_t reg = (uint8_t)(bf >> 8), pos = (bf >> 4) & 0xf, len = (bf & 0xf) + 1;
	uint16_t old, msk = (uint16_t)(((1u << len) - 1) << pos);
	if (rd(a, reg, &old))
		return -1;
	return wr(a, reg, (uint16_t)((old & ~msk) | ((value << pos) & msk)));
}

static int get_bf(uint8_t a, uint16_t bf)
{
	uint8_t reg = (uint8_t)(bf >> 8), pos = (bf >> 4) & 0xf, len = (bf & 0xf) + 1;
	uint16_t v;
	if (rd(a, reg, &v))
		return -1;
	return (v >> pos) & ((1u << len) - 1);
}

/* ---- the container (tfa98xx_parameters.h, packed) ---- */

static uint32_t u32at(size_t o) { uint32_t v = 0; if (o + 4 <= cnt_len) memcpy(&v, cnt + o, 4); return v; }
static uint16_t u16at(size_t o) { uint16_t v = 0; if (o + 2 <= cnt_len) memcpy(&v, cnt + o, 2); return v; }
static const char *str_at(uint32_t dsc) { uint32_t o = dsc & 0xffffff; return o < cnt_len ? (const char *)cnt + o : "?"; }

static int load_cnt(void)
{
	if (cnt)
		return 0;
	FILE *f = fopen(CNT_PATH, "rb");
	if (!f) {
		printf("tfa: no %s\n", CNT_PATH);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	cnt_len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	cnt = malloc(cnt_len + 1);
	if (!cnt || fread(cnt, 1, cnt_len, f) != cnt_len) {
		fclose(f);
		return -1;
	}
	fclose(f);
	if (cnt_len < 46 || memcmp(cnt, "PM", 2)) {
		printf("tfa: %s is not a TFA container\n", CNT_PATH);
		return -1;
	}
	printf("tfa: container %.6s, %u devices, %u profiles, \"%.8s\" \"%.8s\"\n", (const char *)cnt, u16at(40), u16at(42),
	       (const char *)cnt + 24, (const char *)cnt + 32);
	return 0;
}

/* the device list for I2C address addr: its offset, 0 if none */
static size_t find_dev(uint8_t addr)
{
	for (unsigned i = 0; i < u16at(40); i++) {
		uint32_t d = u32at(46 + 4 * i);
		if ((d >> 24) == dscDevice && (d & 0xffffff) + 12 <= cnt_len && cnt[(d & 0xffffff) + 2] == addr)
			return d & 0xffffff;
	}
	return 0;
}

/* one register / bitfield item: 0 = written, 1 = not a write item */
static int write_item(uint8_t a, uint32_t dsc, int verbose)
{
	uint32_t o = dsc & 0xffffff, type = dsc >> 24;
	if (type == dscRegister && o + 5 <= cnt_len) {
		uint8_t reg = cnt[o];
		uint16_t val = u16at(o + 1), msk = u16at(o + 3), old = 0;
		rd(a, reg, &old);
		wr(a, reg, (uint16_t)((old & ~msk) | (val & msk)));
		if (verbose)
			printf("tfa %02x:  reg %02x = %04x (mask %04x, was %04x)\n", a, reg, val, msk, old);
		return 0;
	}
	if (type == dscBitfield && o + 4 <= cnt_len) {
		uint16_t val = u16at(o), field = u16at(o + 2);
		set_bf(a, field, val);
		if (verbose)
			printf("tfa %02x:  bitfield %04x = %u\n", a, field, val);
		return 0;
	}
	return 1;
}

static void write_profile(uint8_t a, size_t dev, const char *want)
{
	uint8_t n = cnt[dev];
	for (unsigned i = 0; i < n; i++) {
		uint32_t d = u32at(dev + 12 + 4 * i);
		if ((d >> 24) != dscProfile)
			continue;
		size_t p = d & 0xffffff;
		uint32_t hdr = u32at(p);
		const char *name = str_at(u32at(p + 4));
		unsigned len = hdr & 0xff;
		printf("tfa %02x: profile \"%s\" (%u items)%s\n", a, name, len ? len - 1 : 0, strcmp(name, want) ? "" : " <- using");
		if (strcmp(name, want))
			continue;
		for (unsigned k = 0; k + 1 < len; k++) {
			uint32_t it = u32at(p + 8 + 4 * k);
			if ((it >> 24) == dscDefault)
				break;
			write_item(a, it, 1);
		}
	}
}

static void status(uint8_t a, const char *when)
{
	uint16_t r00 = 0, r10 = 0, r11 = 0, r13 = 0, r14 = 0;
	rd(a, 0x00, &r00);
	rd(a, 0x10, &r10);
	rd(a, 0x11, &r11);
	rd(a, 0x13, &r13);
	rd(a, 0x14, &r14);
	printf("tfa %02x %s: 00=%04x 10=%04x 11=%04x 13=%04x 14=%04x  PLLS %d CLKS %d AREFS %d AMPS %d SWS %d NOCLK %d MANSTATE %d\n",
	       a, when, r00, r10, r11, r13, r14, (r11 >> 6) & 1, (r11 >> 7) & 1, (r11 >> 15) & 1, (r11 >> 14) & 1,
	       (r11 >> 13) & 1, (r10 >> 7) & 1, (r14 >> 3) & 0xf);
}

int tfa_start(uint8_t a, const char *profile)
{
	uint16_t rev = 0, fb = 0;
	if (load_cnt())
		return -1;
	size_t dev = find_dev(a);
	if (!dev) {
		printf("tfa %02x: not in the container\n", a);
		return -1;
	}
	if (rd(a, 0x03, &rev) || rev != 0x0c74) {
		printf("tfa %02x: revision %04x, expected 0c74\n", a, rev);
		return -1;
	}
	printf("tfa %02x: device \"%s\", %u items\n", a, str_at(u32at(dev + 8)), cnt[dev]);
	status(a, "before");

	/* tfa98xx_init: I2C reset (the whole register: I2CR only) */
	wr(a, 0x00, 1u << ((BF_I2CR >> 4) & 0xf));
	usleep(5000);
	/* tfa9874_specific: unlock key 1 and 2, then the V1.16 settings for revision 0x0c74 */
	wr(a, 0x0f, 0x5a6b);
	rd(a, 0xfb, &fb);
	wr(a, 0xa0, (uint16_t)(fb ^ 0x005a));
	wr(a, 0x0f, 0x5a6b);		/* tfa98xx_key2(unlock) */
	set_bf(a, BF_MTPK, 0x5a);
	wr(a, 0x0f, 0);
	static const uint8_t r[] = { 0x02, 0x52, 0x53, 0x56, 0x61, 0x6f, 0x70, 0x73, 0x74, 0x75, 0x80, 0x83, 0x84 };
	static const uint16_t v[] = { 0x22c8, 0x57dc, 0x003e, 0x0400, 0x0110, 0x00a5, 0x07f8, 0x0047, 0x5098, 0x8d28, 0x0000,
				      0x0799, 0x0081 };
	for (unsigned i = 0; i < sizeof(r); i++)
		wr(a, r[i], v[i]);

	/* tfaContWriteRegsDev: the device list until a patch, file or profile */
	for (unsigned i = 0; i < cnt[dev]; i++) {
		uint32_t d = u32at(dev + 12 + 4 * i), t = d >> 24;
		if (t == dscPatch || t == dscFile || t == dscProfile)
			break;
		write_item(a, d, 1);
	}
	/* tfaContWriteRegsProf */
	write_profile(a, dev, profile);

	/* INIT_CF (probus device: no SBSL): MANSCONF 1, PWDN 0, wait for the clocks (CLKS) */
	set_bf(a, BF_MANSCONF, 1);
	set_bf(a, BF_PWDN, 0);
	int ok = 0;
	for (int t = 0; t < 50 && !ok; t++) {
		ok = get_bf(a, BF_CLKS) == 1;
		if (!ok)
			usleep(10000);
	}
	status(a, ok ? "clocks stable" : "NO CLOCK (CLKS 0 after 0.5 s)");
	/* OPERATING, muted (AMPE 0) until tfa_mute(0) */
	set_bf(a, BF_AMPE, 0);
	return ok ? 0 : -1;
}

void tfa_mute(uint8_t a, int mute)
{
	set_bf(a, BF_AMPE, mute ? 0 : 1);
	usleep(20000);
	status(a, mute ? "muted" : "unmuted");
}
