/*
 * sensord: accelerometer, gyroscope and magnetometer on the OnePlus 7T (SM8150) from userland (/dev/pmem).
 * The IMU (ST LSM6DSM) and the magnetometer (MEMSIC MMC5603NJ) hang off the SLPI (sensor DSP); apps never
 * see their bus. Like Android (sscrpcd + the SSC HAL) and postmarketOS (hexagonrpcd -s + libssc):
 *   1. boot the SLPI (PAS id 12) - stock DT qcom,ssc@5c00000: firmware "slpi", SMP2P "dsps" (in 481 / out 430,
 *      APCS IPC bit 26, entries master-kernel + sleepstate), GLINK edge "dsps" (SMEM host 3, IPC bit 24),
 *      LCX/LMX votes, AOP load_state; carve-out 0x98100000 + 0x1400000 (dtbo overlay 3, fragment@87; the mdt
 *      is relocatable, linked at 0x97300000);
 *   2. GLINK channels fastrpcglink-apps-dsp (FastRPC) + IPCRTR (QRTR: pd-mapper with the SLPI's domains,
 *      servreg listener for msm/slpi/sensor_pd);
 *   3. FastRPC: attach to the sensors PD, run the reverse-RPC listener; the SLPI's registry reads its JSON
 *      configs and writes its registry through it (fastrpc.c, files under /var/local/sensors);
 *   4. SSC client (QMI 400, ssc.c): default accel / gyro / mag, 50/50/25 Hz -> /var/local/imu.txt.
 * FastRPC payloads: 0x9e200000 + 1 MB (unused top of ABL's cont_splash carve-out, below audiod's block), seen by
 * the SLPI through SMMU streams 0x5a1-0x5a3 (stock DT sdsprpc compute banks) on a translation-off bank, with the
 * context bank (1) in address bits 32+ as downstream adsprpc does.
 *
 *   sensord smp2p          the SLPI's SMP2P state
 *   sensord up [DELAY]     everything above after DELAY s
 *   sensord auto           the boot job: `up 45` (after wland's and audiod's AOSS messages); skipped if
 *                          /var/local/sensord.off exists
 *   sensord shutdown       PAS shutdown of the SLPI
 * Files (not in git): /var/local/fw/slpi/slpi.mdt + slpi.bNN (the 7T's modem partition, image/);
 * /var/local/sensors/{etc/config/NAME.json, etc/sns_reg_config, socinfo/FILE, persist/registry/registry/}
 * (tools/hotdogb-sensors-install.py).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "../wifi/pmem.h"
#include "../wifi/smem.h"
#include "../wifi/link.h"
#include "../wifi/pdmapper.h"
#include "../wifi/qmi.h"
#include "fastrpc.h"
#include "ssc.h"

#define SMEM_HOST_SLPI	3
#define SLPI_PA		0x98100000ull	/* dtbo overlay 3, fragment@87: pil_slpi_mem <0x98100000 0x1400000> */
#define SLPI_SIZE	0x01400000ull
#define PAS_SLPI	12
#define FW_DIR		"/var/local/fw/slpi"
#define APCS_IPC	0x17c0000cull
#define IPC_SLPI_SMP2P	26
#define IPC_AOSS	0
#define SMP2P_OUT	430
#define SMP2P_IN	481
#define SLEEPSTATE_AWAKE (1u << 12)	/* downstream smp2p_sleepstate.c: PROC_AWAKE_ID 12 */
#define SHM_PA		0x9e200000ull
#define SHM_SIZE	0x00100000u
#define FRPC_SID	0x5a0		/* + mask 3: the three sdsprpc banks 0x5a1-0x5a3 */
#define FRPC_MSW	1		/* 0x5a1 & 0xf */
#define ROOT		"/var/local/sensors"

uint32_t qrtr_node_remote = 9;		/* replaced by the SLPI's HELLO */

/* ---- cmd-db and RPMh votes (as audiod.c) ---- */

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (strncmp(name, id, 8))
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static int vote_top(const char *res)
{
	uint64_t aux;
	uint32_t len, top = 0;
	if (cmddb_aux(res, &aux, &len)) {
		printf("vote %s: not in cmd-db\n", res);
		return -1;
	}
	for (uint32_t i = 0; i < len / 2; i++)
		if ((rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff)
			top = i;
	uint32_t off[1] = { 0 }, val[1] = { top };
	int rc = pm_rpmh(res, 1, off, val);
	printf("vote %s level index %u -> %d\n", res, top, rc);
	return rc;
}

/* ---- AOSS QMP (as audiod.c) ---- */

#define QMP_RAM	0x0c300000ull
#define QMP_UP	0x0000ffffu

static int wait_eq(uint64_t pa, uint32_t want, int ms)
{
	for (int i = 0; i < ms; i++) {
		if (rd32(pa) == want)
			return 0;
		usleep(1000);
	}
	return -1;
}

static int qmp_send(const char *msg)
{
	if (rd32(QMP_RAM) != 0x4d41494c || rd32(QMP_RAM + 4) != 1)
		return -1;
	if (!(rd32(QMP_RAM + 0x24) == QMP_UP && rd32(QMP_RAM + 0x2c) == QMP_UP && rd32(QMP_RAM + 0x30) == QMP_UP)) {
		printf("qmp: link not open (wland opens it); skipping \"%s\"\n", msg);
		return -1;
	}
	uint32_t off = rd32(QMP_RAM + 0x38), buf[16] = { 0 };
	if (wait_eq(QMP_RAM + off, 0, 2000))
		return -1;
	strncpy((char *)buf, msg, sizeof(buf) - 1);
	for (int i = 0; i < 16; i++)
		wr32(QMP_RAM + off + 4 + 4 * i, buf[i]);
	wr32(QMP_RAM + off, sizeof(buf));
	(void)rd32(QMP_RAM + off);
	wr32(APCS_IPC, 1u << IPC_AOSS);
	int rc = wait_eq(QMP_RAM + off, 0, 1000);
	printf("qmp: \"%s\" -> %s\n", msg, rc ? "TIMEOUT" : "acked");
	return rc;
}

/* ---- SMP2P with the SLPI (remote pid 3): master-kernel + sleepstate (apps awake) ---- */

static void kick(unsigned bit)
{
	wr32(APCS_IPC, 1u << bit);
}

static uint64_t smp2p_out_pa;

static void smp2p_name(uint64_t e, const char *name)
{
	char nm[16] = { 0 };
	strncpy(nm, name, 15);
	for (int i = 0; i < 16; i += 4) {
		uint32_t w;
		memcpy(&w, nm + i, 4);
		wr32(e + i, w);
	}
}

static int smp2p_create(void)
{
	uint64_t pa;
	if (smem_alloc(SMEM_HOST_SLPI, SMP2P_OUT, 20 + 16 * 20, &pa)) {
		fprintf(stderr, "smp2p: cannot allocate item %u\n", SMP2P_OUT);
		return -1;
	}
	if (rd32(pa) != 0x504d5324) {
		wr32(pa + 8, 0 | (3u << 16));		/* local pid 0, remote pid 3 */
		wr32(pa + 12, 16 | (2u << 16));		/* total 16 entries, 2 valid */
		wr32(pa + 16, 0);
		smp2p_name(pa + 20, "master-kernel");
		wr32(pa + 36, 0);
		smp2p_name(pa + 40, "sleepstate");
		wr32(pa + 56, SLEEPSTATE_AWAKE);
		wr32(pa + 0, 0x504d5324);		/* "$SMP" */
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 4, 1 | (1u << 8));		/* version 1 last, features: SSR ack */
	}
	smp2p_out_pa = pa;
	kick(IPC_SLPI_SMP2P);
	return 0;
}

static int smp2p_in_entry(const char *name, uint64_t *va)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_SLPI, SMP2P_IN, &pa, &sz))
		return -1;
	uint32_t n = rd32(pa + 12) >> 16;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char nm[17];
		for (int k = 0; k < 16; k++)
			nm[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		nm[16] = 0;
		if (!strcmp(nm, name)) {
			*va = e + 16;
			return 0;
		}
	}
	return -1;
}

static uint32_t slpi_state = 0xffffffff;

static void smp2p_service(void)
{
	uint64_t ip, va;
	uint32_t sz;
	if (!smp2p_out_pa)
		return;
	if (smem_get(SMEM_HOST_SLPI, SMP2P_IN, &ip, &sz) == 0) {
		uint32_t want = (rd32(ip + 16) & 1) ? 2 : 0;
		if ((rd32(smp2p_out_pa + 16) & 2) != want) {
			wr32(smp2p_out_pa + 16, (rd32(smp2p_out_pa + 16) & ~2u) | want);
			kick(IPC_SLPI_SMP2P);
		}
	}
	if (smp2p_in_entry("slave-kernel", &va) == 0) {
		uint32_t v = rd32(va);
		if (v != slpi_state) {
			printf("[%6llu] smp2p: slpi state %x%s%s%s%s\n", (unsigned long long)link_ms(), v, v & 1 ? " ERR-FATAL" : "",
			       v & 2 ? " ready" : "", v & 4 ? " handover" : "", v & 8 ? " stop-ack" : "");
			if (v != 0xffffffff && (v & 1))
				printf("[%6llu] #### SLPI ERROR FATAL\n", (unsigned long long)link_ms());
			slpi_state = v;
		}
	}
}

static void smp2p_show(void)
{
	uint64_t pa;
	uint32_t sz;
	for (int k = 0; k < 2; k++) {
		unsigned item = k ? SMP2P_IN : SMP2P_OUT;
		if (smem_get(SMEM_HOST_SLPI, item, &pa, &sz)) {
			printf("smp2p item %u: absent\n", item);
			continue;
		}
		uint32_t pids = rd32(pa + 8), counts = rd32(pa + 12);
		printf("smp2p item %u at %llx: magic %08x pids %u->%u entries %u/%u flags %x\n", item, (unsigned long long)pa,
		       rd32(pa), pids & 0xffff, pids >> 16, counts >> 16, counts & 0xffff, rd32(pa + 16));
		for (uint32_t i = 0; i < (counts >> 16) && i < 16; i++) {
			uint64_t e = pa + 20 + 20ull * i;
			char name[17];
			for (int c = 0; c < 16; c++)
				name[c] = (char)(rd32(e + (c & ~3)) >> (8 * (c & 3)));
			name[16] = 0;
			printf("  entry %u: %-16s value %08x\n", i, name, rd32(e + 16));
		}
	}
}

/* ---- PAS boot (as audiod.c boot_adsp, plus relocation: Linux qcom_mdt_load with QCOM_MDT_RELOCATABLE) ---- */

#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))

static int64_t scm(uint32_t cmd, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { SCM_PIL(cmd), arginfo, a, b, c, 0, 0, 0 };
	pm_smc(x);
	if (r1)
		*r1 = x[1];
	return (int64_t)x[0];
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*len + 4);
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

struct phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };
#define SEG_HASH(p)	((((p)->flags >> 24) & 7) == 2)
#define SEG_RELOC	(1u << 27)

static int boot_slpi(void)
{
	uint64_t va;
	if (smp2p_in_entry("slave-kernel", &va) == 0 && (rd32(va) & 2)) {
		printf("boot: the SLPI already says ready (%x): not booting it again\n", rd32(va));
		return 0;
	}
	if (vote_top("lcx.lvl") || vote_top("lmx.lvl"))
		return -1;
	size_t mlen;
	uint8_t *mdt = slurp(FW_DIR "/slpi.mdt", &mlen);
	if (!mdt) {
		fprintf(stderr, "boot: no %s/slpi.mdt\n", FW_DIR);
		return -1;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	int hash = -1, reloc = 0;
	uint64_t lo = ~0ull, hi = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		if (SEG_HASH(&ph[i]))
			hash = (int)i;
		if (ph[i].type != 1 || SEG_HASH(&ph[i]) || !ph[i].memsz)
			continue;
		if (ph[i].flags & SEG_RELOC)
			reloc = 1;
		if (ph[i].paddr < lo)
			lo = ph[i].paddr;
		if (ph[i].paddr + (uint64_t)ph[i].memsz > hi)
			hi = ph[i].paddr + (uint64_t)ph[i].memsz;
	}
	if (hash < 0 || lo > hi) {
		fprintf(stderr, "boot: no hash segment or no loadable segment\n");
		return -1;
	}
	uint64_t base = reloc ? lo : SLPI_PA;	/* image address that lands at SLPI_PA */
	printf("boot: slpi.mdt %u segments, image %llx-%llx%s -> %llx\n", phnum, (unsigned long long)lo,
	       (unsigned long long)hi, reloc ? " (relocatable)" : "", (unsigned long long)SLPI_PA);
	if (hi - base > SLPI_SIZE) {
		fprintf(stderr, "boot: image (%llx bytes) larger than the region\n", (unsigned long long)(hi - base));
		return -1;
	}
	size_t ehsz = phoff + 32ull * phnum, meta_len = ehsz + ph[hash].filesz;
	uint8_t *meta = malloc(meta_len);
	memcpy(meta, mdt, ehsz);
	if (ph[hash].offset + ph[hash].filesz <= mlen) {
		memcpy(meta + ehsz, mdt + ph[hash].offset, ph[hash].filesz);
	} else {
		char path[128];
		size_t hl;
		snprintf(path, sizeof(path), FW_DIR "/slpi.b%02d", hash);
		uint8_t *h = slurp(path, &hl);
		if (!h || hl < ph[hash].filesz) {
			fprintf(stderr, "boot: hash segment %s missing\n", path);
			return -1;
		}
		memcpy(meta + ehsz, h, ph[hash].filesz);
		free(h);
	}
	uint64_t meta_pa = pm_alloc(meta_len, 0x1000);
	if (!meta_pa)
		return -1;
	pm_wr(meta_pa, meta, (meta_len + 3) & ~3ull);
	uint64_t r1 = 0;
	int64_t rc = scm(1, 2 | (2u << 6), PAS_SLPI, meta_pa, 0, &r1);
	printf("boot: init image (%zu bytes metadata at %llx) -> %lld/%llx\n", meta_len, (unsigned long long)meta_pa,
	       (long long)rc, (unsigned long long)r1);
	pm_free(meta_pa);
	if (rc || r1)
		return -1;
	rc = scm(2, 3, PAS_SLPI, SLPI_PA, SLPI_SIZE, &r1);
	printf("boot: mem setup %llx+%llx -> %lld/%llx\n", SLPI_PA, SLPI_SIZE, (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;
	uint64_t total = 0;
	static uint8_t zeros[0x10000];
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		uint64_t pa = ph[i].paddr - base + SLPI_PA;
		if (ph[i].paddr < base || pa + ph[i].memsz > SLPI_PA + SLPI_SIZE) {
			fprintf(stderr, "boot: segment %u at %llx outside the region\n", i, (unsigned long long)ph[i].paddr);
			return -1;
		}
		if (ph[i].filesz) {
			char path[128];
			static uint8_t chunk[0x10000 + 4];
			snprintf(path, sizeof(path), FW_DIR "/slpi.b%02u", i);
			FILE *f = fopen(path, "rb");
			uint32_t done = 0;
			while (f && done < ph[i].filesz) {
				size_t want = ph[i].filesz - done < 0x10000 ? ph[i].filesz - done : 0x10000;
				size_t got = fread(chunk, 1, want, f);
				if (got != want)
					break;
				memset(chunk + got, 0, 4);
				pm_wr(pa + done, chunk, (got + 3) & ~(size_t)3);
				done += (uint32_t)got;
			}
			if (f)
				fclose(f);
			if (done < ph[i].filesz) {
				fprintf(stderr, "boot: %s missing or short (%u of %u bytes)\n", path, done, ph[i].filesz);
				return -1;
			}
			total += ph[i].filesz;
		}
		for (uint64_t o = (ph[i].filesz + 3) & ~3ull; o < ph[i].memsz; o += sizeof(zeros)) {
			uint64_t n = ph[i].memsz - o < sizeof(zeros) ? ph[i].memsz - o : sizeof(zeros);
			pm_wr(pa + o, zeros, (n + 3) & ~3ull);
		}
	}
	printf("boot: %llu bytes of segments loaded\n", (unsigned long long)total);
	qmp_send("{class: image, res: load_state, name: slpi, val: on}");
	rc = scm(5, 1, PAS_SLPI, 0, 0, &r1);
	printf("boot: auth and reset -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;
	for (int t = 0; t < 200; t++) {
		smp2p_service();
		if (slpi_state != 0xffffffff && (slpi_state & 2) && !(slpi_state & 1)) {
			printf("#### SLPI BOOTED (ready after %d00 ms)\n", t);
			return 0;
		}
		usleep(100000);
	}
	printf("boot: no ready from the SLPI in 20 s\n");
	smp2p_show();
	return -1;
}

/* ---- apps SMMU: the SLPI's FastRPC streams through a translation-off bank (as audiod.c smmu_identity) ---- */

#define SMMU_PA		0x15000000ull

static int smmu_identity(uint32_t sid, uint32_t mask)
{
	uint64_t s = SMMU_PA;
	uint32_t id0 = rd32(s + 0x20), id1 = rd32(s + 0x24), nsmr = id0 & 0xff, ncb = id1 & 0xff;
	uint32_t psize = (id1 >> 31) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
	uint64_t used[4] = { 0 };
	int slot = -1, cb = -1;
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd32(s + 0x800 + 4 * i), s2cr = rd32(s + 0xc00 + 4 * i);
		if (!(smr >> 31)) {
			if (slot < 0)
				slot = (int)i;
			continue;
		}
		if (((s2cr >> 16) & 3) == 0)
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
		uint32_t id = smr & 0x7fff, m = (smr >> 16) & 0x7fff;
		if (((id ^ sid) & ~(m | mask) & 0x7fff) == 0) {
			printf("smmu: SMR%u %08x already matches %#x: S2CR %08x\n", i, smr, sid, s2cr);
			return 0;
		}
	}
	for (uint32_t c = 0; c < ncb && c < 256; c++) {
		uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)c * psize;
		if (!(used[c / 64] & (1ull << (c % 64))) && !(rd32(cbp) & 1) && rd32(s + psize + 4 * c) == 0) {
			cb = (int)c;
			break;
		}
	}
	if (slot < 0 || cb < 0) {
		printf("smmu: no free SMR/bank (SMR %d, bank %d)\n", slot, cb);
		return -1;
	}
	uint64_t gr1 = s + psize, cbp = s + (uint64_t)npage * psize + (uint64_t)cb * psize;
	wr32(cbp + 0x58, 0xffffffff);
	wr32(cbp + 0x20, 0);
	wr32(cbp + 0x24, 0);
	wr32(cbp + 0x30, 0);
	wr32(cbp + 0x10, 0);
	wr32(cbp + 0x38, 0);
	wr32(cbp + 0x3c, 0);
	wr32(gr1 + 0x800 + 4 * cb, 1);			/* CBA2R: AArch64 */
	wr32(gr1 + 4 * cb, 0x0001f000u);		/* CBAR as ABL's banks */
	wr32(cbp, 0x000000e0u);				/* SCTLR: M = 0, no translation */
	wr32(s + 0xc00 + 4 * slot, (uint32_t)cb);
	wr32(s + 0x800 + 4 * slot, (1u << 31) | (mask << 16) | sid);
	wr32(s + 0x70, 0);
	for (int i = 0; i < 1000 && (rd32(s + 0x74) & 1); i++)
		usleep(1000);
	printf("smmu: fastrpc %#x/%#x -> CB%d via SMR%d: SMR %08x S2CR %08x\n", sid, mask, cb, slot,
	       rd32(s + 0x800 + 4 * slot), rd32(s + 0xc00 + 4 * slot));
	return 0;
}

/* ---- servreg notifier: msm/slpi/sensor_pd (as audiod.c) ---- */

#define SR_SERVICE	0x42
#define SR_PORT		0x4001
#define SR_REGISTER	0x20
#define SR_STATE_IND	0x22
#define SR_STATE_ACK	0x23
#define SR_UP		0x1fffffffu
static const char sensor_pd[] = "msm/slpi/sensor_pd";
static uint32_t sr_node, sr_port;
static volatile uint32_t sensor_pd_state;
static uint16_t sr_txn;
static int qrtr_hello, pdm_served;

static const char *sr_state(uint32_t s)
{
	switch (s) {
	case 0x7fffffffu: return "UNINIT";
	case 0x0fffffffu: return "DOWN";
	case SR_UP: return "UP";
	case 0x2fffffffu: return "EARLY_DOWN";
	default: return "?";
	}
}

static void sr_register(void)
{
	struct qmi_buf q;
	qmi_start(&q, QMI_REQUEST, ++sr_txn, SR_REGISTER);
	qmi_tlv_u8(&q, 1, 1);
	qmi_tlv(&q, 2, sensor_pd, (uint16_t)strlen(sensor_pd));
	qrtr_send(QRTR_TYPE_DATA, SR_PORT, sr_node, sr_port, q.b, (uint32_t)qmi_finish(&q));
	printf("servreg: listening for %s at %u:%x\n", sensor_pd, sr_node, sr_port);
}

static void sr_state_now(uint32_t st)
{
	sensor_pd_state = st;
	if (st == SR_UP)
		printf("[%6llu] #### SLPI SENSOR PD UP\n", (unsigned long long)link_ms());
}

static void sr_handle(const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	uint32_t st = 0;
	if (qmi_parse(d, n, &m))
		return;
	if (m.type == QMI_RESPONSE && m.id == SR_REGISTER) {
		uint16_t rl = 0;
		const uint8_t *r = qmi_get(&m, 2, &rl);
		uint16_t res = r && rl >= 4 ? (uint16_t)(r[0] | r[1] << 8) : 0xffff;
		int have = qmi_get_u32(&m, 0x10, &st) == 0;
		printf("[%6llu] servreg: register -> result %u, %s is %#x %s\n", (unsigned long long)link_ms(), res, sensor_pd,
		       st, have ? sr_state(st) : "(no state)");
		if (have)
			sr_state_now(st);
	} else if (m.type == QMI_INDICATION && m.id == SR_STATE_IND) {
		uint16_t pl = 0, tl = 0;
		const uint8_t *path = qmi_get(&m, 2, &pl), *tx = qmi_get(&m, 3, &tl);
		qmi_get_u32(&m, 1, &st);
		printf("[%6llu] servreg: %.*s now %#x %s\n", (unsigned long long)link_ms(), path ? pl : 0,
		       path ? (const char *)path : "", st, sr_state(st));
		sr_state_now(st);
		struct qmi_buf q;
		qmi_start(&q, QMI_REQUEST, ++sr_txn, SR_STATE_ACK);
		qmi_tlv(&q, 1, path ? path : (const uint8_t *)sensor_pd, path ? pl : (uint16_t)strlen(sensor_pd));
		if (tx && tl >= 2)
			qmi_tlv(&q, 2, tx, 2);
		qrtr_send(QRTR_TYPE_DATA, SR_PORT, sr_node, sr_port, q.b, (uint32_t)qmi_finish(&q));
	} else if (!(m.type == QMI_RESPONSE && m.id == SR_STATE_ACK)) {
		printf("servreg: message type %u id %#x\n", m.type, m.id);
	}
}

static void qrtr_cb(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	uint32_t w[5] = { 0 };
	memcpy(w, d, n < sizeof(w) ? n : sizeof(w));
	if (h->type == QRTR_TYPE_DATA) {
		if (h->confirm_rx)
			qrtr_resume_tx(h);
		if (h->dst_port == PDMAPPER_PORT) {
			pdmapper_handle(h, d, n);
			pdm_served++;
			return;
		}
		if (h->dst_port == SR_PORT) {
			sr_handle(d, n);
			return;
		}
		if (ssc_port(h->dst_port)) {
			ssc_handle(h->dst_port, d, n);
			return;
		}
		printf("[%6llu] qrtr: < DATA %u:%x -> %u:%x, %u bytes\n", (unsigned long long)link_ms(), h->src_node, h->src_port,
		       h->dst_node, h->dst_port, n);
		return;
	}
	if (w[0] == QRTR_TYPE_HELLO) {
		printf("[%6llu] qrtr: < HELLO from node %u\n", (unsigned long long)link_ms(), h->src_node);
		qrtr_node_remote = h->src_node;
		uint32_t hello[5] = { QRTR_TYPE_HELLO };
		qrtr_send(QRTR_TYPE_HELLO, QRTR_PORT_CTRL, h->src_node, QRTR_PORT_CTRL, hello, sizeof(hello));
		if (!qrtr_hello) {
			qrtr_hello = 1;
			pdmapper_init();
		}
	} else if (w[0] == QRTR_TYPE_NEW_SERVER) {
		printf("[%6llu] qrtr: server service %#x (%u) instance %#x at %u:%x\n", (unsigned long long)link_ms(), w[1], w[1],
		       w[2], w[3], w[4]);
		if (w[1] == SR_SERVICE && !sr_node) {
			sr_node = w[3];
			sr_port = w[4];
			sr_register();
		} else if (w[1] == SSC_SERVICE) {
			ssc_server(w[3], w[4]);
		}
	} else {
		printf("[%6llu] qrtr: < ctrl type %u (cmd %u) from %u:%x: %x %x %x %x\n", (unsigned long long)link_ms(), h->type,
		       w[0], h->src_node, h->src_port, w[1], w[2], w[3], w[4]);
	}
}

/* every packet on the raw channels (LINK_CHAN = FastRPC; LINK_CHAN2 is a name nobody opens) */
void link_raw(const uint8_t *d, uint32_t n)
{
	frpc_rx(d, n);
}

static void idle(void)
{
	smp2p_service();
	frpc_service();
	ssc_tick();
}

static int up(unsigned delay)
{
	printf("#### STARTING SENSORS (SLPI)\n");
	if (delay) {
		printf("waiting %u s (wland's and audiod's AOSS messages first)\n", delay);
		sleep(delay);
	}
	if (smp2p_create())
		return 1;
	printf("smp2p: outbound item %u at %llx (sleepstate awake)\n", SMP2P_OUT, (unsigned long long)smp2p_out_pa);
	if (boot_slpi())
		return 1;
	if (smmu_identity(FRPC_SID, 3))
		return 1;
	frpc_init(SHM_PA, SHM_SIZE, FRPC_MSW, ROOT);
	link_idle = idle;
	int rc = -1;
	for (int t = 0; t < 10 && rc; t++)	/* the GLINK FIFO appears a little after "ready" */
		rc = link_up(qrtr_cb, 2000);
	if (rc)
		return 1;
	printf("[%6llu] #### FASTRPC CHANNEL OPEN\n", (unsigned long long)link_ms());
	for (int t = 0; t < 100 && !pdm_served; t++)
		link_poll(100);
	printf("qrtr: IPCRTR %s, hello %s (SLPI node %u), pd-mapper answered %d request(s)\n",
	       link_qrtr_open() ? "open" : "NOT open", qrtr_hello ? "yes" : "no", qrtr_node_remote, pdm_served);
	/* the sensors PD: attach (retried while it starts), then the listener its registry needs */
	int att = -1;
	for (int t = 0; t < 30 && att; t++) {
		att = frpc_attach_sensors();
		if (att)
			link_poll(2000);
	}
	if (att) {
		printf("[%6llu] #### FASTRPC ATTACH TO THE SENSORS PD FAILED (%d)\n", (unsigned long long)link_ms(), att);
	} else {
		printf("[%6llu] #### FASTRPC ATTACHED TO THE SENSORS PD\n", (unsigned long long)link_ms());
		if (frpc_listener_start())
			printf("[%6llu] #### FASTRPC LISTENER FAILED\n", (unsigned long long)link_ms());
	}
	printf("sensord: up; serving the SLPI (SMP2P, GLINK, FastRPC files, SSC)\n");
	uint64_t next = 0;
	for (;;) {
		link_poll(1000);
		if (link_ms() >= next) {
			next = link_ms() + 15000;
			printf("[%6llu] sensord: sensor PD %s, %u reverse calls, %u files opened\n", (unsigned long long)link_ms(),
			       sr_state(sensor_pd_state), frpc_reverse_calls, frpc_files_opened);
		}
	}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2) {
		fprintf(stderr, "usage: sensord smp2p | up [DELAY] | auto | shutdown\n");
		return 2;
	}
	pm_init();
	if (smem_init())
		return 1;
	if (!strcmp(argv[1], "smp2p")) {
		smp2p_show();
		return 0;
	}
	if (!strcmp(argv[1], "up"))
		return up(argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	if (!strcmp(argv[1], "auto")) {
		if (access("/var/local/sensord.off", F_OK) == 0) {
			printf("sensord: /var/local/sensord.off: not starting\n");
			return 0;
		}
		return up(45);
	}
	if (!strcmp(argv[1], "shutdown")) {
		uint64_t r1;
		int64_t rc = scm(6, 1, PAS_SLPI, 0, 0, &r1);
		printf("shutdown -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
		return rc != 0;
	}
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
