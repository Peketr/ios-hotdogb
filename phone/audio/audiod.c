/*
 * audiod: sound on the OnePlus 7T (SM8150) from userland (/dev/pmem), step 1: boot the ADSP (LPASS Hexagon)
 * and reach its audio services over APR. Same layers as phone/wifi's modem bring-up (built from ../wifi with
 * the ADSP's edge), after Linux qcom_q6v5_pas (PAS id 1), qcom_smp2p (lpass: in 443, out 429, APCS IPC bit 10),
 * qcom_glink_smem (edge "lpass": SMEM host 2, IPC bit 8) and soc/qcom/apr.c on the "apr_audio_svc" channel.
 *
 *   audiod smp2p        the ADSP's SMP2P state
 *   audiod tfa          the speaker amps' registers 0x00-0x13 over I2C (also done first by up/auto)
 *   audiod up [DELAY]   after DELAY s (default 0): power votes, PAS boot, GLINK, APR core state + versions
 *   audiod auto         the boot job: `up 30` (wland's AOSS messages come first: one QMP mailbox)
 *   audiod shutdown     PAS shutdown of the ADSP
 * After the core is ready, up also muxes the quaternary MI2S pins and starts QUAT_MI2S_RX (48 kHz 16-bit stereo on
 * SD1, LPASS bit clock 1.536 MHz), then reads the amps again (/var/local/audiod.noport skips this).
 *
 * The speakers (dtbo overlay 3): NXP TFA98xx amps at I2C 0x34 (right, reset GPIO 37) and 0x35 (left, reset
 * GPIO 100) on the quaternary MI2S port (oplus,speaker-i2s-id 3). No WCD9340 / SLIMbus on that path.
 * Firmware: /var/local/fw/adsp/adsp.mdt + adsp.bNN from the 7T's modem partition (image/), not in git.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../wifi/pmem.h"
#include "../wifi/smem.h"
#include "../wifi/link.h"
#include "i2c.h"
#include "../wifi/pdmapper.h"
#include "../wifi/qmi.h"

#define SMEM_HOST_ADSP	2
#define ADSP_PA		0x8be00000ull	/* dtbo overlay 3: adsp region <0x8be00000 0x1e00000> = adsp.mdt's span */
#define ADSP_SIZE	0x01e00000ull
#define PAS_ADSP	1
#define FW_DIR		"/var/local/fw/adsp"
#define APCS_IPC	0x17c0000cull
#define IPC_ADSP_SMP2P	10
#define IPC_AOSS	0
#define SMP2P_OUT	429
#define SMP2P_IN	443

/* ---- cmd-db and RPMh votes (as wland.c) ---- */

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
	if (cmddb_aux(res, &aux, &len))
		return -1;
	for (uint32_t i = 0; i < len / 2; i++)
		if ((rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff)
			top = i;
	uint32_t off[1] = { 0 }, val[1] = { top };
	int rc = pm_rpmh(res, 1, off, val);
	printf("vote %s level index %u -> %d\n", res, top, rc);
	return rc;
}

/* ---- AOSS QMP (as wland.c) ---- */

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
	if (wait_eq(QMP_RAM + off, 0, 2000))	/* someone else's message still pending */
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

/* ---- SMP2P with the ADSP (as wland.c, remote pid 2) ---- */

static void kick(unsigned bit)
{
	wr32(APCS_IPC, 1u << bit);
}

static uint64_t smp2p_out_pa;

static int smp2p_create(void)
{
	uint64_t pa;
	if (smem_alloc(SMEM_HOST_ADSP, SMP2P_OUT, 20 + 16 * 20, &pa)) {
		fprintf(stderr, "smp2p: cannot allocate item %u\n", SMP2P_OUT);
		return -1;
	}
	if (rd32(pa) != 0x504d5324) {
		static const char nm[16] = "master-kernel";
		wr32(pa + 8, 0 | (2u << 16));		/* local pid 0, remote pid 2 */
		wr32(pa + 12, 16 | (1u << 16));		/* total 16 entries, 1 valid */
		wr32(pa + 16, 0);
		for (int i = 0; i < 16; i += 4) {
			uint32_t w;
			memcpy(&w, nm + i, 4);
			wr32(pa + 20 + i, w);
		}
		wr32(pa + 36, 0);
		wr32(pa + 0, 0x504d5324);		/* "$SMP" */
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 4, 1 | (1u << 8));		/* version 1 last, features: SSR ack */
	}
	smp2p_out_pa = pa;
	kick(IPC_ADSP_SMP2P);
	return 0;
}

static int smp2p_in_entry(const char *name, uint64_t *va)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_ADSP, SMP2P_IN, &pa, &sz))
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

static uint32_t adsp_state = 0xffffffff;

static void smp2p_service(void)
{
	uint64_t ip, va;
	uint32_t sz;
	if (!smp2p_out_pa)
		return;
	if (smem_get(SMEM_HOST_ADSP, SMP2P_IN, &ip, &sz) == 0) {
		uint32_t want = (rd32(ip + 16) & 1) ? 2 : 0;
		if ((rd32(smp2p_out_pa + 16) & 2) != want) {
			wr32(smp2p_out_pa + 16, (rd32(smp2p_out_pa + 16) & ~2u) | want);
			kick(IPC_ADSP_SMP2P);
		}
	}
	if (smp2p_in_entry("slave-kernel", &va) == 0) {
		uint32_t v = rd32(va);
		if (v != adsp_state) {
			printf("[%6llu] smp2p: adsp state %x%s%s%s%s\n", (unsigned long long)link_ms(), v, v & 1 ? " ERR-FATAL" : "",
			       v & 2 ? " ready" : "", v & 4 ? " handover" : "", v & 8 ? " stop-ack" : "");
			adsp_state = v;
		}
	}
}

static void smp2p_show(void)
{
	uint64_t pa;
	uint32_t sz;
	for (int k = 0; k < 2; k++) {
		unsigned item = k ? SMP2P_IN : SMP2P_OUT;
		if (smem_get(SMEM_HOST_ADSP, item, &pa, &sz)) {
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

/* ---- PAS boot (as wland.c boot_modem) ---- */

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

static int boot_adsp(void)
{
	/* never boot over an earlier run's state (ios-topaz's lesson): a phone boot starts without it */
	uint64_t va;
	if (smp2p_in_entry("slave-kernel", &va) == 0 && (rd32(va) & 2)) {
		printf("boot: the ADSP already says ready (%x): not booting it again\n", rd32(va));
		return 0;
	}
	if (vote_top("cx.lvl"))
		return -1;
	size_t mlen;
	uint8_t *mdt = slurp(FW_DIR "/adsp.mdt", &mlen);
	if (!mdt) {
		fprintf(stderr, "boot: no %s/adsp.mdt\n", FW_DIR);
		return -1;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	int hash = -1;
	for (uint32_t i = 0; i < phnum; i++)
		if (((ph[i].flags >> 24) & 7) == 2)
			hash = (int)i;
	if (hash < 0) {
		fprintf(stderr, "boot: no hash segment\n");
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
		snprintf(path, sizeof(path), FW_DIR "/adsp.b%02d", hash);
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
	int64_t rc = scm(1, 2 | (2u << 6), PAS_ADSP, meta_pa, 0, &r1);
	printf("boot: init image (%zu bytes metadata at %llx) -> %lld/%llx\n", meta_len, (unsigned long long)meta_pa,
	       (long long)rc, (unsigned long long)r1);
	pm_free(meta_pa);
	if (rc || r1)
		return -1;
	rc = scm(2, 3, PAS_ADSP, ADSP_PA, ADSP_SIZE, &r1);
	printf("boot: mem setup %llx+%llx -> %lld/%llx\n", ADSP_PA, ADSP_SIZE, (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;
	uint64_t total = 0;
	static uint8_t zeros[0x10000];
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		uint64_t pa = ph[i].paddr;
		if (pa < ADSP_PA || pa + ph[i].memsz > ADSP_PA + ADSP_SIZE) {
			fprintf(stderr, "boot: segment %u at %llx outside the region\n", i, (unsigned long long)pa);
			return -1;
		}
		if (ph[i].filesz) {
			/* streamed in 64 KB pieces: launchd jobs have a small jetsam limit (a 9.4 MB segment in one
			 * malloc got audiod killed, reason "per-process limit") */
			char path[128];
			static uint8_t chunk[0x10000 + 4];
			snprintf(path, sizeof(path), FW_DIR "/adsp.b%02u", i);
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
	qmp_send("{class: image, res: load_state, name: adsp, val: on}");
	rc = scm(5, 1, PAS_ADSP, 0, 0, &r1);
	printf("boot: auth and reset -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;
	for (int t = 0; t < 200; t++) {
		smp2p_service();
		if (adsp_state != 0xffffffff && (adsp_state & 2) && !(adsp_state & 1)) {
			printf("#### ADSP BOOTED (ready after %d00 ms)\n", t);
			return 0;
		}
		usleep(100000);
	}
	printf("boot: no ready from the ADSP in 20 s\n");
	smp2p_show();
	return -1;
}

/* ---- APR (Linux drivers/soc/qcom/apr.c, include/linux/soc/qcom/apr.h) ---- */

struct apr_hdr {
	uint16_t hdr_field, pkt_size;
	uint8_t src_svc, src_domain;
	uint16_t src_port;
	uint8_t dest_svc, dest_domain;
	uint16_t dest_port;
	uint32_t token, opcode;
} __attribute__((packed));

#define APR_HDR_FIELD_CMD	((1u << 8) | ((sizeof(struct apr_hdr) / 4) << 4))	/* SEQ_CMD, 8 words, ver 0 */
#define APR_DOMAIN_ADSP		4
#define APR_DOMAIN_APPS		5
#define APR_SVC_ADSP_CORE	3
#define APR_BASIC_RSP_RESULT	0x000110e8
#define AVCS_GET_VERSIONS	0x00012905
#define AVCS_GET_VERSIONS_RSP	0x00012906
#define AVCS_GET_STATE		0x0001290c
#define AVCS_GET_STATE_RSP	0x0001290d

static uint32_t apr_token;
static volatile int core_ready, versions_done;
static volatile uint32_t rsp_opcode, rsp_status, rsp_seen;

static int apr_send(uint8_t svc, uint32_t opcode, const void *payload, uint16_t len)
{
	uint8_t b[512];
	struct apr_hdr h = {
		.hdr_field = APR_HDR_FIELD_CMD, .pkt_size = (uint16_t)(sizeof(h) + len),
		.src_svc = svc, .src_domain = APR_DOMAIN_APPS, .src_port = 0,
		.dest_svc = svc, .dest_domain = APR_DOMAIN_ADSP, .dest_port = 0,
		.token = ++apr_token, .opcode = opcode,
	};
	if (sizeof(h) + len > sizeof(b))
		return -1;
	memcpy(b, &h, sizeof(h));
	if (len)
		memcpy(b + sizeof(h), payload, len);
	return link_send(b, (uint32_t)(sizeof(h) + len));
}

static const char *svc_name(uint32_t id)
{
	switch (id) {
	case 3: return "core"; case 4: return "AFE"; case 5: return "VSM"; case 6: return "VPM";
	case 7: return "ASM"; case 8: return "ADM"; case 9: return "MVM"; case 10: return "CVS";
	case 11: return "CVP"; case 12: return "USM"; case 13: return "LSM"; default: return "?";
	}
}

void link_raw(const uint8_t *d, uint32_t n)
{
	struct apr_hdr h;
	if (n < sizeof(h)) {
		printf("apr: short packet (%u bytes)\n", n);
		return;
	}
	memcpy(&h, d, sizeof(h));
	uint32_t hl = ((h.hdr_field >> 4) & 0xf) * 4, w[64] = { 0 };
	uint32_t pl = n > hl ? n - hl : 0;
	memcpy(w, d + hl, pl < sizeof(w) ? pl : sizeof(w));
	switch (h.opcode) {
	case AVCS_GET_STATE_RSP:
		if (w[0] == 1 && !core_ready)
			printf("[%6llu] #### AUDIO DSP READY (AVCS state %u)\n", (unsigned long long)link_ms(), w[0]);
		else if (w[0] != 1)
			printf("[%6llu] apr: core state %u (not ready yet)\n", (unsigned long long)link_ms(), w[0]);
		core_ready = w[0] == 1;
		break;
	case AVCS_GET_VERSIONS_RSP:
		printf("apr: build %#x, %u services:", w[0], w[1]);
		for (uint32_t i = 0; i < w[1] && 2 + 2 * i + 1 < 64; i++)
			printf(" %s(%#x) v%u", svc_name(w[2 + 2 * i] & 0xff), w[2 + 2 * i], w[3 + 2 * i]);
		printf("\n");
		versions_done = 1;
		break;
	case APR_BASIC_RSP_RESULT:
		printf("apr: result for %#x from %s: %#x%s\n", w[0], svc_name(h.src_svc), w[1], w[1] ? " (ERROR)" : "");
		if (w[0] == AVCS_GET_VERSIONS)
			versions_done = 1;
		rsp_opcode = w[0];
		rsp_status = w[1];
		rsp_seen++;
		break;
	default:
		printf("apr: < %s port %u -> %s port %u opcode %#x token %u, %u bytes: %x %x %x %x\n", svc_name(h.src_svc),
		       h.src_port, svc_name(h.dest_svc), h.dest_port, h.opcode, h.token, pl, w[0], w[1], w[2], w[3]);
		break;
	}
}

/* ---- the speaker amps: NXP TFA98xx, 16-bit big-endian registers (Linux tfa98xx / tfa989x.c) ---- */

static int tfa_rd(uint8_t addr, uint8_t reg, uint16_t *v)
{
	uint8_t b[2];
	int rc = i2c_write_read(addr, &reg, 1, b, 2);
	if (!rc)
		*v = (uint16_t)((b[0] << 8) | b[1]);
	return rc;
}

static void tfa_probe(void)
{
	static const struct { uint8_t addr; const char *side; } amp[] = { { 0x34, "right" }, { 0x35, "left" } };
	i2c_init();
	for (unsigned a = 0; a < 2; a++) {
		uint16_t rev;
		int rc = tfa_rd(amp[a].addr, 0x03, &rev);
		if (rc) {
			printf("tfa %02x (%s): no answer (%d)\n", amp[a].addr, amp[a].side, rc);
			continue;
		}
		printf("#### SPEAKER AMP %02x (%s): TFA98xx revision %04x\n", amp[a].addr, amp[a].side, rev);
		printf("tfa %02x:", amp[a].addr);
		for (uint8_t r = 0; r < 0x14; r++) {
			uint16_t v;
			if (tfa_rd(amp[a].addr, r, &v) == 0)
				printf(" %02x=%04x", r, v);
			else
				printf(" %02x=----", r);
		}
		printf("\n");
	}
}

/* ---- AFE (Linux sound/soc/qcom/qdsp6/q6afe.c, q6dsp-lpass-clocks): the quaternary MI2S RX port ---- */

#define APR_SVC_AFE		4
#define AFE_PORT_QUAT_MI2S_RX	0x1006
#define AFE_PORT_CMD_DEVICE_START 0x000100e5
#define AFE_PORT_CMD_DEVICE_STOP 0x000100e6
#define AFE_PORT_CMD_SET_PARAM_V2 0x000100ef
#define AFE_SVC_CMD_SET_PARAM	0x000100f2
#define AFE_MODULE_AUDIO_DEV_INTERFACE 0x0001020c
#define AFE_PARAM_ID_I2S_CONFIG	0x0001020d
#define AFE_MODULE_CLOCK_SET	0x0001028f
#define AFE_PARAM_ID_CLOCK_SET	0x00010290
#define LPASS_CLK_QUAD_MI2S_IBIT 0x106
#define I2S_SD1			2	/* AFE_PORT_I2S_SD1: the stock DT's quat mi2s rx-lines = 0x02 */
#define RATE			48000
#define BCLK			(RATE * 16 * 2)

/* send and wait for the APR basic response to this opcode: 0 = ok */
static int apr_cmd(uint8_t svc, uint32_t opcode, const void *payload, uint16_t len, const char *what)
{
	uint32_t seen = rsp_seen;
	if (apr_send(svc, opcode, payload, len))
		return -1;
	for (int t = 0; t < 200; t++) {
		link_poll(10);
		if (rsp_seen != seen && rsp_opcode == opcode) {
			printf("afe: %s -> %#x\n", what, rsp_status);
			return rsp_status ? -1 : 0;
		}
	}
	printf("afe: %s -> no answer\n", what);
	return -1;
}

struct param_hdr { uint32_t module_id, param_id; uint16_t param_size, reserved; } __attribute__((packed));

static int afe_clock(uint32_t id, uint32_t hz, int on)
{
	struct {
		uint32_t payload_size, lsw, msw, map;
		struct param_hdr p;
		uint32_t minor, clk_id, freq;
		uint16_t attri, root;
		uint32_t enable;
	} __attribute__((packed)) c = {
		.payload_size = sizeof(struct param_hdr) + 24,
		.p = { AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, 24, 0 },
		.minor = 1, .clk_id = id, .freq = hz, .attri = 1 /* COUPLE_NO */, .root = 0, .enable = (uint32_t)on,
	};
	char what[64];
	snprintf(what, sizeof(what), "clock %#x %u Hz %s", id, hz, on ? "on" : "off");
	return apr_cmd(APR_SVC_AFE, AFE_SVC_CMD_SET_PARAM, &c, sizeof(c), what);
}

static int afe_i2s_config(void)
{
	struct {
		uint16_t port, payload_size;
		uint32_t lsw, msw, map;
		struct param_hdr p;
		uint32_t minor;
		uint16_t bit_width, channel_mode, mono_stereo, ws_src;
		uint32_t sample_rate;
		uint16_t data_format, reserved;
	} __attribute__((packed)) c = {
		.port = AFE_PORT_QUAT_MI2S_RX, .payload_size = sizeof(struct param_hdr) + 20,
		.p = { AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG, 20, 0 },
		.minor = 1, .bit_width = 16, .channel_mode = I2S_SD1, .mono_stereo = 1 /* stereo */,
		.ws_src = 1 /* internal: we are the I2S master */, .sample_rate = RATE,
	};
	return apr_cmd(APR_SVC_AFE, AFE_PORT_CMD_SET_PARAM_V2, &c, sizeof(c), "QUAT_MI2S_RX i2s config 48 kHz 16-bit stereo SD1");
}

static int afe_start(void)
{
	uint16_t s[2] = { AFE_PORT_QUAT_MI2S_RX, 0 };
	return apr_cmd(APR_SVC_AFE, AFE_PORT_CMD_DEVICE_START, s, sizeof(s), "QUAT_MI2S_RX start");
}

/* TLMM south tile (pinctrl-sm8150.c): 137 sck, 138 ws, 139 sd0, 140 sd1 = qua_mi2s (function 1), 8 mA, no pull */
static void mi2s_pins(void)
{
	for (unsigned g = 137; g <= 140; g++) {
		uint64_t ctl = 0x03d00000ull + 0x1000ull * g;
		uint32_t was = rd32(ctl);
		wr32(ctl, (1u << 2) | (3u << 6));
		printf("pins: gpio%u ctl %#x -> %#x\n", g, was, rd32(ctl));
	}
}

/* ---- QRTR on IPCRTR: the ADSP (node 5) asks our pd-mapper where avs/audio runs before its audio PD serves ---- */

static volatile int qrtr_hello, pdm_served;

/*
 * The service registry notifier (QMI 0x42, Linux soc/qcom/pdr_interface.c): Linux's APR registers as a listener
 * for msm/adsp/audio_pd and talks to AFE/ASM/ADM only once it says UP; state changes come as indications to ack.
 */
#define SR_SERVICE	0x42
#define SR_PORT		0x4001		/* our client port */
#define SR_REGISTER	0x20
#define SR_STATE_IND	0x22
#define SR_STATE_ACK	0x23
#define SR_UP		0x1fffffffu
static const char audio_pd[] = "msm/adsp/audio_pd";
static uint32_t sr_node, sr_port;
static volatile uint32_t audio_pd_state;
static uint16_t sr_txn;

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
	qmi_tlv(&q, 2, audio_pd, (uint16_t)strlen(audio_pd));
	qrtr_send(QRTR_TYPE_DATA, SR_PORT, sr_node, sr_port, q.b, (uint32_t)qmi_finish(&q));
	printf("servreg: listening for %s at %u:%x\n", audio_pd, sr_node, sr_port);
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
		uint16_t res = r && rl >= 4 ? (uint16_t)(r[0] | r[1] << 8) : 0xffff, err = r && rl >= 4 ? (uint16_t)(r[2] | r[3] << 8) : 0;
		if (qmi_get_u32(&m, 0x10, &st) == 0)
			audio_pd_state = st;
		printf("[%6llu] servreg: register -> result %u error %u, %s is %#x %s\n", (unsigned long long)link_ms(), res, err,
		       audio_pd, st, sr_state(st));
		if (st == SR_UP)
			printf("[%6llu] #### AUDIO PD UP\n", (unsigned long long)link_ms());
	} else if (m.type == QMI_INDICATION && m.id == SR_STATE_IND) {
		uint16_t pl = 0, tl = 0;
		const uint8_t *path = qmi_get(&m, 2, &pl), *tx = qmi_get(&m, 3, &tl);
		qmi_get_u32(&m, 1, &st);
		audio_pd_state = st;
		printf("[%6llu] servreg: %.*s now %#x %s\n", (unsigned long long)link_ms(), path ? pl : 0,
		       path ? (const char *)path : "", st, sr_state(st));
		if (st == SR_UP)
			printf("[%6llu] #### AUDIO PD UP\n", (unsigned long long)link_ms());
		struct qmi_buf q;
		qmi_start(&q, QMI_REQUEST, ++sr_txn, SR_STATE_ACK);
		qmi_tlv(&q, 1, path ? path : (const uint8_t *)audio_pd, path ? pl : (uint16_t)strlen(audio_pd));
		if (tx && tl >= 2)
			qmi_tlv(&q, 2, tx, 2);
		qrtr_send(QRTR_TYPE_DATA, SR_PORT, sr_node, sr_port, q.b, (uint32_t)qmi_finish(&q));
	} else if (m.type == QMI_RESPONSE && m.id == SR_STATE_ACK) {
		/* ack of our ack */
	} else {
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
		printf("[%6llu] qrtr: < DATA %u:%x -> %u:%x, %u bytes\n", (unsigned long long)link_ms(), h->src_node, h->src_port,
		       h->dst_node, h->dst_port, n);
		return;
	}
	printf("[%6llu] qrtr: < ctrl type %u (cmd %u) from %u:%x: %x %x %x %x\n", (unsigned long long)link_ms(), h->type, w[0],
	       h->src_node, h->src_port, w[1], w[2], w[3], w[4]);
	if (w[0] == QRTR_TYPE_HELLO) {
		uint32_t hello[5] = { QRTR_TYPE_HELLO };	/* struct qrtr_ctrl_pkt: always 20 bytes */
		qrtr_send(QRTR_TYPE_HELLO, QRTR_PORT_CTRL, h->src_node, QRTR_PORT_CTRL, hello, sizeof(hello));
		if (!qrtr_hello) {
			qrtr_hello = 1;
			pdmapper_init();
		}
	} else if (w[0] == QRTR_TYPE_NEW_SERVER) {
		printf("qrtr:   server service %#x instance %#x at %u:%x\n", w[1], w[2], w[3], w[4]);
		if (w[1] == SR_SERVICE && !sr_node) {
			sr_node = w[3];
			sr_port = w[4];
			sr_register();
		}
	}
}

static int up(unsigned delay)
{
	tfa_probe();
	if (delay) {
		printf("waiting %u s (wland's AOSS messages first)\n", delay);
		sleep(delay);
	}
	if (smp2p_create())
		return 1;
	printf("smp2p: outbound item %u at %llx\n", SMP2P_OUT, (unsigned long long)smp2p_out_pa);
	if (boot_adsp())
		return 1;
	link_idle = smp2p_service;
	int rc = -1;
	for (int t = 0; t < 10 && rc; t++) {	/* the ADSP's GLINK FIFO appears a little after "ready" */
		rc = link_up(qrtr_cb, 2000);
	}
	if (rc)
		return 1;
	printf("#### APR CHANNEL OPEN\n");
	for (int t = 0; t < 60 && !core_ready; t++) {
		apr_send(APR_SVC_ADSP_CORE, AVCS_GET_STATE, NULL, 0);
		link_poll(500);
	}
	if (!core_ready) {
		printf("apr: the core never said ready in 30 s\n");
		return 1;
	}
	apr_send(APR_SVC_ADSP_CORE, AVCS_GET_VERSIONS, NULL, 0);
	for (int t = 0; t < 30 && !versions_done; t++)
		link_poll(100);
	/* QRTR: HELLO both ways, then our pd-mapper; the audio PD (AFE/ASM/ADM) starts after the ADSP asked it */
	for (int t = 0; t < 100 && !pdm_served; t++)
		link_poll(100);
	printf("qrtr: IPCRTR %s, hello %s, pd-mapper answered %d request(s)\n", link_qrtr_open() ? "open" : "NOT open",
	       qrtr_hello ? "yes" : "no", pdm_served);
	for (int t = 0; t < 150 && audio_pd_state != SR_UP; t++)	/* as Linux APR: AFE/ASM/ADM only after UP */
		link_poll(100);
	printf("servreg: %s %#x %s%s\n", audio_pd, audio_pd_state, sr_state(audio_pd_state),
	       sr_node ? "" : " (no notifier service seen)");
	if (access("/var/local/audiod.noport", F_OK) != 0) {
		mi2s_pins();
		int ok = 0;
		for (int t = 0; t < 6 && !ok; t++) {	/* the audio PD may need a few seconds after the lookup */
			ok = afe_clock(LPASS_CLK_QUAD_MI2S_IBIT, BCLK, 1) == 0;
			if (!ok)
				link_poll(3000);
		}
		if (ok && afe_i2s_config() == 0 && afe_start() == 0)
			printf("[%6llu] #### QUAT MI2S RUNNING (%u Hz bit clock)\n", (unsigned long long)link_ms(), BCLK);
		link_poll(500);
		printf("amps with the bit clock running:\n");
		tfa_probe();
	}
	printf("audiod: up; serving the ADSP (SMP2P, GLINK)\n");
	for (;;)
		link_poll(1000);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2) {
		fprintf(stderr, "usage: audiod smp2p | up [DELAY] | auto | shutdown\n");
		return 2;
	}
	pm_init();
	if (smem_init())
		return 1;
	if (!strcmp(argv[1], "smp2p")) {
		smp2p_show();
		return 0;
	}
	if (!strcmp(argv[1], "tfa")) {
		tfa_probe();
		return 0;
	}
	if (!strcmp(argv[1], "up"))
		return up(argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	if (!strcmp(argv[1], "auto")) {
		if (access("/var/local/audiod.off", F_OK) == 0) {
			printf("audiod: /var/local/audiod.off: not starting\n");
			return 0;
		}
		return up(30);
	}
	if (!strcmp(argv[1], "shutdown")) {
		uint64_t r1;
		int64_t rc = scm(6, 1, PAS_ADSP, 0, 0, &r1);
		printf("shutdown -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
		return rc != 0;
	}
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
