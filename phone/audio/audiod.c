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
		printf("apr: result for %#x from %s: %#x\n", w[0], svc_name(h.src_svc), w[1]);
		if (w[0] == AVCS_GET_VERSIONS)
			versions_done = 1;
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
		rc = link_up(NULL, 2000);
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
