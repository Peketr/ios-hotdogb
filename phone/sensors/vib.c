/*
 * The 7T's vibration motor from userland (/dev/pmem): an LRA on the PM8150B haptics block (stock DT
 * qcom,haptics@c000 under qcom,pm8150b@3 = SPMI SID 3, peripheral 0xc0; OnePlus drives it with
 * drivers/input/misc/qti-haptics.c - their SM8150 kernel has no AW8697 driver, that DT node is for other
 * projects). Its supply haptics_boost is a fixed regulator on PM8150B GPIO 5 (SID 2, 0xc400), active high.
 *
 * SPMI through the PMIC arbiter v5 like bsd/dev/sweet_spmi.c: APID from the core's PPID map (0x900 + 4 * n),
 * owner EE from cfg 0x700 + 4 * n (must be apps, 0), commands on the APID's owner channel (0x0c600000 +
 * 0x10000 * APID): WDATA0 +0x10, CMD +0x00 (opcode << 27 | addr << 4, BC 0; EXT_WRITEL 0, EXT_READL 1),
 * STATUS +0x08 (done bit 0, errors bits 1-3), RDATA0 +0x18. The kernel only uses the APIDs of SID 0/2
 * charger, gauge and RTC peripherals, so these lanes are ours.
 *
 * Play (qti-haptics.c hw_init + a VMAX-source constant play, OnePlus' auto-resonance forced off):
 * CFG1 0x4c = 0 (LRA), ILIM 0x52 = 2 (1 A), EN_CTL3 0x4a = 0xfd, RATE_CFG1/2 0x54/0x55 = the calibrated
 * resonance period from LRA_AUTO_RES 0x0b/0x0c as OnePlus does (else play-rate-us 6667 / 5 us), CFG2 0x4d = 0
 * (sine), AUTO_RES_CTRL 0x4b bit 7 off, VMAX 0x51 [5:1] = mV / 116, HAP_SEL 0x4e [5:4] = 0 (VMAX), EN_CTL1
 * 0x46 bit 7, PLAY 0x70 bit 7. Stop: PLAY 0, then EN_CTL1 0 after 1 ms (HAP_DISABLE_DELAY_USEC).
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "../wifi/pmem.h"
#include "vib.h"

#define SP_CORE_PA	0x0c440000ull
#define SP_CFG_PA	0x0c40a000ull
#define SP_CHNLS_PA	0x0c600000ull
#define HAP_SID		3
#define HAP_BASE	0xc000u
#define BOOST_SID	2
#define BOOST_BASE	0xc400u		/* PM8150B GPIO 5 */
#define VMAX_MV		3400		/* stock DT qcom,vmax-mv 0xd48 */
#define PLAY_RATE_US	6667		/* stock DT qcom,play-rate-us 0x1a0b (150 Hz) */

static unsigned sp_count;
static int ready = -1;

static int apid_of(unsigned sid, unsigned per, unsigned *owner)
{
	int found = -1;
	for (unsigned i = 0; i < sp_count; i++) {
		uint32_t map = rd32(SP_CORE_PA + 0x900 + 4 * i);
		if (!map || ((map >> 8) & 0xfff) != ((sid << 8) | per))
			continue;
		unsigned o = rd32(SP_CFG_PA + 0x700 + 4 * i) & 7;
		if (found < 0 || o == 0) {
			found = (int)i;
			*owner = o;
		}
	}
	return found;
}

/* opcode 0 = write, 1 = read; returns 0 or -1 */
static int sp_cmd(unsigned sid, unsigned addr, int rd, uint8_t *v)
{
	unsigned owner = 7;
	int apid = apid_of(sid, addr >> 8, &owner);
	if (apid < 0 || owner != 0) {
		printf("vib: SPMI SID %u %#x: %s\n", sid, addr, apid < 0 ? "no APID" : "not owned by apps");
		return -1;
	}
	uint64_t ch = SP_CHNLS_PA + 0x10000ull * (unsigned)apid;
	if (!rd)
		wr32(ch + 0x10, *v);
	wr32(ch, ((rd ? 1u : 0u) << 27) | ((addr & 0xff) << 4));
	for (int i = 0; i < 2000; i++) {
		uint32_t st = rd32(ch + 0x08);
		if (st & 1) {
			if (st & 0x0e) {
				printf("vib: SPMI SID %u %#x %s: status %#x\n", sid, addr, rd ? "read" : "write", st);
				return -1;
			}
			if (rd)
				*v = (uint8_t)rd32(ch + 0x18);
			return 0;
		}
		usleep(1);
	}
	printf("vib: SPMI SID %u %#x %s: timeout\n", sid, addr, rd ? "read" : "write");
	return -1;
}

static int rd8(unsigned sid, unsigned addr) { uint8_t v = 0; return sp_cmd(sid, addr, 1, &v) ? -1 : v; }
static int wr8(unsigned sid, unsigned addr, uint8_t v) { return sp_cmd(sid, addr, 0, &v); }
static int hw(unsigned off, uint8_t v) { return wr8(HAP_SID, HAP_BASE + off, v); }
static int hr(unsigned off) { return rd8(HAP_SID, HAP_BASE + off); }
static int hmask(unsigned off, uint8_t mask, uint8_t v)
{
	int o = hr(off);
	return o < 0 ? -1 : hw(off, (uint8_t)((o & ~mask) | (v & mask)));
}

int vib_init(void)
{
	if (ready >= 0)
		return ready ? 0 : -1;
	ready = 0;
	pm_init();
	uint32_t ver = rd32(SP_CORE_PA);
	sp_count = rd32(SP_CORE_PA + 4) & 0x7ff;
	if (ver < 0x50000000u || ver >= 0x70000000u || !sp_count || sp_count > 512) {
		printf("vib: unsupported SPMI arbiter %#x (%u APIDs)\n", ver, sp_count);
		return -1;
	}
	int ht = hr(0x04), hs = hr(0x05), gt = rd8(BOOST_SID, BOOST_BASE + 0x04), gs = rd8(BOOST_SID, BOOST_BASE + 0x05);
	int en = hr(0x46), st = hr(0x0a), lo = hr(0x0b), hi = hr(0x0c);
	printf("vib: haptics SID 3 0xc000 type %#x subtype %#x en %#x status %#x auto-res %#x %#x; boost GPIO5 type %#x subtype %#x mode %#x out %#x en %#x\n",
	       ht, hs, en, st, lo, hi, gt, gs, rd8(BOOST_SID, BOOST_BASE + 0x40), rd8(BOOST_SID, BOOST_BASE + 0x44),
	       rd8(BOOST_SID, BOOST_BASE + 0x46));
	if (ht < 0 || gt < 0)
		return -1;
	/* haptics_boost: GPIO 5 digital output, VIN 0, no pull, CMOS high strength, inverted constant low = high */
	if (wr8(BOOST_SID, BOOST_BASE + 0x40, 0x01) || wr8(BOOST_SID, BOOST_BASE + 0x41, 0x00) ||
	    wr8(BOOST_SID, BOOST_BASE + 0x42, 0x05) || wr8(BOOST_SID, BOOST_BASE + 0x45, 0x03) ||
	    wr8(BOOST_SID, BOOST_BASE + 0x44, 0x80) || wr8(BOOST_SID, BOOST_BASE + 0x46, 0x80))
		return -1;
	usleep(1000);	/* regulator-enable-ramp-delay 300 us */
	uint8_t r1, r2;
	if (lo > 0 || (hi > 0 && (hi & 0xf0))) {
		r1 = (uint8_t)lo;
		r2 = (uint8_t)((hi & 0xf0) >> 4);
	} else {
		r1 = (PLAY_RATE_US / 5) & 0xff;
		r2 = ((PLAY_RATE_US / 5) >> 8) & 0xf;
	}
	if (hw(0x4c, 0x00) || hw(0x52, 0x02) || hw(0x4a, 0xfd) || hmask(0x5a, 0x07, 0x03) || hw(0x54, r1) ||
	    hw(0x55, r2) || hw(0x4d, 0x00) || hmask(0x4f, 0x8f, 0x0a) || hmask(0x4b, 0x80, 0x00) ||
	    hmask(0x51, 0x3e, (uint8_t)((VMAX_MV / 116) << 1)) || hmask(0x4e, 0x31, 0x00) || hmask(0x48, 0x01, 0x00))
		return -1;
	printf("#### HAPTICS READY (PM8150B LRA, BOOST GPIO5 HIGH %#x, RATE %#x %#x = %u US, VMAX %u MV)\n",
	       rd8(BOOST_SID, BOOST_BASE + 0x08), r2, r1, ((unsigned)r2 << 8 | r1) * 5, VMAX_MV);
	ready = 1;
	return 0;
}

int vib_set(int on)
{
	if (vib_init())
		return -1;
	int rc;
	if (on)
		rc = hw(0x46, 0x80) || hw(0x70, 0x80);
	else {
		rc = hw(0x70, 0x00);
		usleep(1000);
		rc |= hw(0x46, 0x00);
	}
	return rc ? -1 : 0;
}

int vib_status(void)
{
	return vib_init() ? -1 : hr(0x0a);
}
