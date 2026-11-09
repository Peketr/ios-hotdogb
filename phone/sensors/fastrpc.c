/*
 * FastRPC (adsprpc) apps side from userland, and the reverse-RPC listener + apps_std file server for the SLPI's
 * sensors PD. See fastrpc.h. The SLPI's sns_registry reads /vendor/etc/sensors/config/NAME.json and
 * /sys/devices/soc0/FILE and writes /mnt/vendor/persist/sensors/registry/registry/FILE through these calls; all of it
 * is served from ROOT (/var/local/sensors) as below (map_path).
 *
 * Transport (Linux fastrpc.c): a 40-byte fastrpc_msg on the GLINK channel points at a payload in shared memory:
 * remote args { pv, len }[n], invoke bufs { num, pgidx }[n], pages { addr, size }[n], fd list (16 x u64),
 * CRC list (64 x u32), then each buffer at a 128-byte boundary. The DSP answers { u64 ctx, s32 retval } and has
 * written the output buffers in place. ctx: bits 63:16 our sequence, 15:8 the payload area + 1, 7:0 the PD
 * (2 = sensors PD, which FASTRPC_IOCTL_INIT_ATTACH_SNS selects).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fastrpc.h"

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

size_t frpc_blob_encode(const struct iob *b, unsigned n, uint8_t *dst, size_t cap)
{
	size_t o = 0;
	for (unsigned i = 0; i < n; i++) {
		if (o + 4 > cap)
			return (size_t)-1;
		put32(dst + o, b[i].n);
		o += 4;
		if (!b[i].n)
			continue;
		size_t pad = (8 - (o & 7)) & 7;
		if (o + pad + b[i].n > cap)
			return (size_t)-1;
		memset(dst + o, 0, pad);
		o += pad;
		if (b[i].p)
			memcpy(dst + o, b[i].p, b[i].n);
		else
			memset(dst + o, 0, b[i].n);
		o += b[i].n;
	}
	return o;
}

int frpc_blob_decode(uint8_t *blob, size_t len, unsigned n, struct iob *out)
{
	size_t o = 0;
	for (unsigned i = 0; i < n; i++) {
		if (o + 4 > len)
			return -1;
		uint32_t s = get32(blob + o);
		o += 4;
		if (s)
			o += (8 - (o & 7)) & 7;
		if (o + s > len)
			return -1;
		out[i].p = blob + o;
		out[i].n = s;
		o += s;
	}
	return 0;
}

size_t frpc_payload(uint8_t *p, size_t cap, uint64_t base, uint32_t sc, const struct iob *in,
		    const uint32_t *outlen, uint32_t *arg_off)
{
	unsigned nin = SC_INBUFS(sc), nout = SC_OUTBUFS(sc), nb = nin + nout, nsc = nb + SC_HANDLES(sc);
	size_t meta = 40u * nsc + 8 * 16 + 4 * 64 + 4;
	if (nb > FRPC_MAX_ARGS || meta > cap)
		return 0;
	memset(p, 0, meta);
	uint8_t *rpra = p, *list = p + 16u * nsc, *pages = list + 8u * nsc;
	size_t off = meta;
	for (unsigned i = 0; i < nb; i++) {
		uint32_t len = i < nin ? in[i].n : outlen[i - nin];
		uint64_t pv = 0, pa = 0, psz = 0;
		arg_off[i] = 0;
		if (len) {
			off = (off + 127) & ~(size_t)127;
			if (off + len > cap)
				return 0;
			pv = base + off;
			pa = pv & ~0xfffull;
			psz = ((pv + len - 1) & ~0xfffull) - pa + 0x1000;
			if (i < nin)
				memcpy(p + off, in[i].p, len);
			else
				memset(p + off, 0, len);
			arg_off[i] = (uint32_t)off;
			off += len;
		}
		put64(rpra + 16 * i, pv);
		put64(rpra + 16 * i + 8, len);
		put32(list + 8 * i, len ? 1 : 0);
		put32(list + 8 * i + 4, i);
		put64(pages + 16 * i, pa);
		put64(pages + 16 * i + 8, psz);
	}
	return off;
}

#ifndef FRPC_PURE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../wifi/pmem.h"
#include "../wifi/link.h"

#define AREA		0x40000u	/* one invoke's payload: 256 KB */
#define INCAP		0x10000u	/* reverse-call input blob we offer the DSP */
#define PD_SENSORS	2
#define HANDLE_INIT	1
#define HANDLE_REMOTECTL 0
#define HANDLE_LISTENER	3		/* adsp_listener: init2 = 3, next2 = 4 */
#define CTX_PD_NOTIFY	0xabcdabcdull

#define LOGT(...) do { printf("[%6llu] ", (unsigned long long)link_ms()); printf(__VA_ARGS__); } while (0)

static uint64_t shm;
static uint32_t shm_sz, msw;
static const char *root = "/var/local/sensors";
static uint32_t client_pid;
static uint32_t seq;
static uint8_t pay[AREA];
unsigned frpc_reverse_calls, frpc_files_opened;

struct pending {
	int posted, done;
	uint64_t ctx;
	int32_t ret;
	unsigned area;
	uint32_t sc;
	uint32_t off[FRPC_MAX_ARGS];
	uint32_t outlen[FRPC_MAX_ARGS];
};
static struct pending sync_p, next_p;

void frpc_init(uint64_t pa, uint32_t size, uint32_t dsp_msw, const char *r)
{
	shm = pa;
	shm_sz = size;
	msw = dsp_msw;
	if (r)
		root = r;
	client_pid = (uint32_t)getpid();
	printf("fastrpc: payloads at %llx (+%#x), DSP addresses %#x:%08llx, files from %s\n", (unsigned long long)pa, size,
	       msw, (unsigned long long)pa, root);
}

void frpc_rx(const uint8_t *d, uint32_t n)
{
	if (n < 12) {
		LOGT("fastrpc: < short packet (%u bytes)\n", n);
		return;
	}
	uint64_t ctx;
	int32_t ret;
	memcpy(&ctx, d, 8);
	memcpy(&ret, d + 8, 4);
	if (ctx == CTX_PD_NOTIFY) {
		LOGT("fastrpc: < PD notification (retval %d)\n", ret);
		return;
	}
	struct pending *p[2] = { &sync_p, &next_p };
	for (int i = 0; i < 2; i++)
		if (p[i]->posted && !p[i]->done && (p[i]->ctx >> 8) == (ctx >> 8)) {
			p[i]->ret = ret;
			p[i]->done = 1;
			return;
		}
	LOGT("fastrpc: < response for no pending call: ctx %llx retval %d (%u bytes)\n", (unsigned long long)ctx, ret, n);
}

struct fastrpc_msg {
	uint32_t pid, tid;
	uint64_t ctx;
	uint32_t handle, sc;
	uint64_t addr, size;
};

static int post(struct pending *pd, unsigned area, int kernel, uint32_t handle, uint32_t sc, const struct iob *in,
		const uint32_t *outlen)
{
	if ((area + 1) * (uint64_t)AREA > shm_sz)
		return -1;
	uint64_t pa = shm + (uint64_t)area * AREA, base = ((uint64_t)msw << 32) | pa;
	unsigned nout = SC_OUTBUFS(sc);
	size_t len = frpc_payload(pay, AREA, base, sc, in, outlen, pd->off);
	if (!len) {
		printf("fastrpc: invoke %u/%#x does not fit in %u bytes\n", handle, sc, AREA);
		return -1;
	}
	pm_wr(pa, pay, (len + 3) & ~(size_t)3);
	__asm__ volatile("dsb sy" ::: "memory");
	memcpy(pd->outlen, outlen ? outlen : (const uint32_t[1]){ 0 }, 4 * (nout ? nout : 1));
	pd->area = area;
	pd->sc = sc;
	pd->done = 0;
	pd->posted = 1;
	pd->ctx = ((uint64_t)++seq << 16) | ((uint64_t)(area + 1) << 8) | PD_SENSORS;
	struct fastrpc_msg m = {
		.pid = kernel ? 0 : client_pid, .tid = client_pid, .ctx = pd->ctx, .handle = handle, .sc = sc,
		.addr = base, .size = (len + 0xfff) & ~(size_t)0xfff,
	};
	if (link_send(&m, sizeof(m))) {
		pd->posted = 0;
		return -1;
	}
	return 0;
}

/* output j of a finished call into buf (its outlen[j] bytes) */
static void fetch(const struct pending *pd, unsigned j, void *buf)
{
	unsigned nin = SC_INBUFS(pd->sc);
	uint32_t n = pd->outlen[j];
	if (!n)
		return;
	static uint8_t tmp[AREA];
	pm_rd(shm + (uint64_t)pd->area * AREA + pd->off[nin + j], tmp, (n + 3) & ~3u);
	memcpy(buf, tmp, n);
}

static int invoke(int kernel, uint32_t handle, uint32_t sc, const struct iob *in, struct iob *out, unsigned ms)
{
	uint32_t outlen[FRPC_MAX_ARGS] = { 0 };
	for (unsigned j = 0; j < SC_OUTBUFS(sc); j++)
		outlen[j] = out[j].n;
	if (post(&sync_p, 0, kernel, handle, sc, in, outlen))
		return -1000;
	uint64_t end = link_ms() + ms;
	while (!sync_p.done && link_ms() < end)
		link_poll(1);
	sync_p.posted = 0;
	if (!sync_p.done) {
		printf("fastrpc: invoke %u/%#x: no answer in %u ms\n", handle, sc, ms);
		return -1001;
	}
	for (unsigned j = 0; j < SC_OUTBUFS(sc); j++)
		fetch(&sync_p, j, out[j].p);
	return sync_p.ret;
}

int frpc_attach_sensors(void)
{
	uint32_t pid = client_pid;
	struct iob in[1] = { { (uint8_t *)&pid, 4 } };
	int rc = invoke(1, HANDLE_INIT, SC_MAKE(0, 1, 0), in, NULL, 3000);
	printf("fastrpc: attach to the sensors PD (pid %u) -> %d\n", pid, rc);
	return rc;
}

/* ---- the listener ---- */

static uint8_t obuf[AREA];		/* outputs of the reverse call being answered */
static uint8_t oblob[AREA];		/* their encoding for next2 */
static uint8_t iblob[INCAP];
static int listener_fail;

static int post_next2(uint32_t rctx, uint32_t res, const uint8_t *blob, uint32_t blen)
{
	uint32_t prim[4] = { rctx, res, blen, INCAP };
	struct iob in[2] = { { (uint8_t *)prim, 16 }, { (uint8_t *)blob, blen } };
	uint32_t outlen[2] = { 16, INCAP };
	return post(&next_p, 1, 0, HANDLE_LISTENER, SC_MAKE(4, 2, 2), in, outlen);
}

int frpc_listener_start(void)
{
	static const char name[] = "adsp_default_listener";
	uint32_t prim[2] = { sizeof(name), 256 }, ret2[2] = { 0, 0 };
	char err[256] = { 0 };
	struct iob in[2] = { { (uint8_t *)prim, 8 }, { (uint8_t *)name, sizeof(name) } };
	struct iob out[2] = { { (uint8_t *)ret2, 8 }, { (uint8_t *)err, sizeof(err) } };
	int rc = invoke(0, HANDLE_REMOTECTL, SC_MAKE(0, 2, 2), in, out, 3000);
	printf("fastrpc: remotectl open \"%s\" -> %d, handle %#x, dlerr %d \"%.64s\"\n", name, rc, ret2[0], (int)ret2[1], err);
	if (rc || ret2[1])
		return -1;
	rc = invoke(0, ret2[0], SC_MAKE(0, 0, 0), NULL, NULL, 3000);
	printf("fastrpc: adsp_default_listener register -> %d\n", rc);
	if (rc)
		return -1;
	rc = invoke(0, HANDLE_LISTENER, SC_MAKE(3, 0, 0), NULL, NULL, 3000);
	printf("fastrpc: adsp_listener init2 -> %d\n", rc);
	if (rc)
		return -1;
	if (post_next2(0, 0xffffffffu, NULL, 0))
		return -1;
	printf("[%6llu] #### FASTRPC LISTENER RUNNING (the SLPI can read its sensor configs now)\n",
	       (unsigned long long)link_ms());
	return 0;
}

/* ---- reverse calls ---- */

struct call {
	uint32_t handle, sc, method;
	unsigned nin, nout, ext;	/* ext: 1 when the method number is the first word of in[0] (>= 31) */
	struct iob in[FRPC_MAX_ARGS], out[FRPC_MAX_ARGS];
	size_t used;
};

static uint32_t W(const struct call *c, unsigned k)
{
	unsigned o = 4 * (k + c->ext);
	return c->nin && c->in[0].n >= o + 4 ? get32(c->in[0].p + o) : 0;
}

static uint64_t W64(const struct call *c, unsigned k)
{
	return W(c, k) | ((uint64_t)W(c, k + 1) << 32);
}

static uint8_t *out_buf(struct call *c, unsigned j, uint32_t n)
{
	if (j >= c->nout || c->used + n > sizeof(obuf))
		return NULL;
	c->out[j].p = obuf + c->used;
	c->out[j].n = n;
	memset(c->out[j].p, 0, n);
	c->used += (n + 7) & ~7u;
	return c->out[j].p;
}

static const char *S(const struct call *c, unsigned i)
{
	static char tmp[4][512];
	static unsigned k;
	if (i >= c->nin || !c->in[i].n)
		return "";
	if (c->in[i].p[c->in[i].n - 1] == 0)
		return (const char *)c->in[i].p;
	char *t = tmp[k++ & 3];
	uint32_t n = c->in[i].n < 511 ? c->in[i].n : 511;
	memcpy(t, c->in[i].p, n);
	t[n] = 0;
	return t;
}

static int verbose_calls(void)
{
	return frpc_reverse_calls <= 400;
}

#define CLOG(...) do { if (verbose_calls()) LOGT(__VA_ARGS__); } while (0)

static const struct { const char *dsp, *local; } maps[] = {
	{ "/mnt/vendor/persist/sensors", "persist" },
	{ "/persist/sensors", "persist" },
	{ "/system/vendor/etc/sensors", "etc" },
	{ "/vendor/etc/sensors", "etc" },
	{ "/sys/devices/soc0", "socinfo" },
	{ "/vendor/etc/acdbdata", "acdb" },
	{ "/usr/lib/qcom/adsp", "dsp" },
	{ "/vendor/lib/rfsa/adsp", "dsp" },
	{ "/vendor/dsp", "dsp" },
};

/* DSP path -> ROOT/...; relative names under the env's directory. -1 for ".." */
static int map_path(const char *p, const char *env, char *out, size_t cap)
{
	if (strstr(p, ".."))
		return -1;
	if (p[0] != '/') {
		const char *d = env && !strcmp(env, "ADSP_AVS_CFG_PATH") ? "acdb" : "dsp";
		snprintf(out, cap, "%s/%s/%s", root, d, p);
		return 0;
	}
	for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
		size_t l = strlen(maps[i].dsp);
		if (!strncmp(p, maps[i].dsp, l) && (p[l] == '/' || !p[l])) {
			snprintf(out, cap, "%s/%s%s", root, maps[i].local, p + l);
			return 0;
		}
	}
	snprintf(out, cap, "%s/other%s", root, p);
	return 0;
}

static int aee(int e)
{
	return e == ENOENT || e == ENOTDIR ? AEE_ENOSUCH : AEE_EFAILED;
}

static int open_mode(const char *mode)
{
	int plus = strchr(mode, '+') != NULL;
	switch (mode[0]) {
	case 'w': return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
	case 'a': return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
	default: return plus ? O_RDWR : O_RDONLY;
	}
}

static void mkparents(const char *path)
{
	char t[512];
	snprintf(t, sizeof(t), "%s", path);
	for (char *s = t + strlen(root) + 1; (s = strchr(s, '/')); s++) {
		*s = 0;
		mkdir(t, 0755);
		*s = '/';
	}
}

static int do_open(struct call *c, const char *name, const char *mode, const char *env)
{
	char path[512];
	uint8_t *o = out_buf(c, 0, 4);
	if (!o)
		return AEE_EBADPARM;
	if (map_path(name, env, path, sizeof(path))) {
		CLOG("frpc: < open \"%s\" refused\n", name);
		return AEE_EBADPARM;
	}
	int fl = open_mode(mode);
	if (fl & O_CREAT)
		mkparents(path);
	int fd = open(path, fl, 0644);
	int e = errno;
	CLOG("frpc: < open \"%s\" \"%s\"%s%s -> %s: %d%s%s\n", name, mode, env && *env ? " env " : "", env ? env : "", path,
	     fd, fd < 0 ? " " : "", fd < 0 ? strerror(e) : "");
	if (fd < 0)
		return aee(e);
	if (!frpc_files_opened++)
		printf("[%6llu] #### FIRST FILE OPENED BY THE SLPI: %s\n", (unsigned long long)link_ms(), name);
	put32(o, (uint32_t)fd);
	return 0;
}

#define MAX_DIRS 16
static DIR *dirs[MAX_DIRS];

static int apps_std(struct call *c)
{
	char path[512];
	struct stat st;
	uint8_t *o;
	int fd = (int)W(c, 0);
	switch (c->method) {
	case 0:		/* fopen(name, mode, rout fd) */
		return do_open(c, S(c, 1), S(c, 2), NULL);
	case 19:	/* fopen_with_env(envvar, delim, name, mode, rout fd) */
		return do_open(c, S(c, 3), S(c, 4), S(c, 1));
	case 2:		/* fflush */
	case 23:	/* fsync */
		if (c->method == 23)
			fsync(fd);
		return 0;
	case 3:		/* fclose */
		CLOG("frpc: < close %d\n", fd);
		return close(fd) ? aee(errno) : 0;
	case 4: {	/* fread(fd, rout written, rout eof, rout buf[size]) */
		uint32_t size = W(c, 1);
		uint8_t *r = out_buf(c, 0, 8), *b = out_buf(c, 1, size);
		if (!r || (size && !b))
			return AEE_ENOMEMORY;
		ssize_t n = read(fd, b, size);
		if (n < 0)
			return aee(errno);
		put32(r, (uint32_t)n);
		put32(r + 4, (uint32_t)n < size);
		CLOG("frpc: < read %d: %zd of %u\n", fd, n, size);
		return 0;
	}
	case 5: {	/* fwrite(fd, buf, rout written, rout eof) */
		uint8_t *r = out_buf(c, 0, 8);
		const struct iob *b = c->nin > 1 ? &c->in[1] : NULL;
		ssize_t n = b && b->n ? write(fd, b->p, b->n) : 0;
		if (n < 0)
			return aee(errno);
		if (r)
			put32(r, (uint32_t)n);
		CLOG("frpc: < write %d: %zd of %u\n", fd, n, b ? b->n : 0);
		return 0;
	}
	case 8:		/* ftell(fd, rout pos) */
		if ((o = out_buf(c, 0, 4)))
			put32(o, (uint32_t)lseek(fd, 0, SEEK_CUR));
		return 0;
	case 9: {	/* fseek(fd, offset, whence index) */
		static const int wh[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
		uint32_t w = W(c, 2);
		if (w > 2)
			return AEE_EBADPARM;
		return lseek(fd, (off_t)(int32_t)W(c, 1), wh[w]) < 0 ? aee(errno) : 0;
	}
	case 10:	/* flen(fd, rout u64) */
		if (fstat(fd, &st))
			return aee(errno);
		if ((o = out_buf(c, 0, 8)))
			put64(o, (uint64_t)st.st_size);
		return 0;
	case 22:	/* fileExists(path, rout boolean) */
		if (map_path(S(c, 1), NULL, path, sizeof(path)))
			return AEE_EBADPARM;
		if ((o = out_buf(c, 0, 1)))
			*o = stat(path, &st) == 0;
		CLOG("frpc: < exists \"%s\" -> %d\n", S(c, 1), o ? *o : -1);
		return 0;
	case 24:	/* fremove(name) */
		if (map_path(S(c, 1), NULL, path, sizeof(path)))
			return AEE_EBADPARM;
		CLOG("frpc: < remove \"%s\"\n", S(c, 1));
		return unlink(path) ? aee(errno) : 0;
	case 26: {	/* opendir(name, rout u64) */
		if (map_path(S(c, 1), NULL, path, sizeof(path)) || !(o = out_buf(c, 0, 8)))
			return AEE_EBADPARM;
		int k = 0;
		while (k < MAX_DIRS && dirs[k])
			k++;
		DIR *d = k < MAX_DIRS ? opendir(path) : NULL;
		int e = errno;
		CLOG("frpc: < opendir \"%s\" -> %s %s\n", S(c, 1), path, d ? "ok" : strerror(e));
		if (!d)
			return aee(e);
		dirs[k] = d;
		put64(o, (uint64_t)k + 1);
		return 0;
	}
	case 27: {	/* closedir(u64) */
		uint64_t h = W64(c, 0);
		if (h < 1 || h > MAX_DIRS || !dirs[h - 1])
			return AEE_EBADPARM;
		closedir(dirs[h - 1]);
		dirs[h - 1] = NULL;
		return 0;
	}
	case 28: {	/* readdir(u64, rout inode, rout name[256], rout eof): 264 bytes, eof at 260 */
		uint64_t h = W64(c, 0);
		if (h < 1 || h > MAX_DIRS || !dirs[h - 1] || !(o = out_buf(c, 0, 264)))
			return AEE_EBADPARM;
		struct dirent *de = readdir(dirs[h - 1]);
		if (de)
			snprintf((char *)o + 4, 255, "%s", de->d_name);
		put32(o + 260, de == NULL);
		return 0;
	}
	case 29:	/* mkdir(name, mode) */
		if (map_path(S(c, 1), NULL, path, sizeof(path)))
			return AEE_EBADPARM;
		mkparents(path);
		CLOG("frpc: < mkdir \"%s\"\n", S(c, 1));
		return mkdir(path, 0755) && errno != EEXIST ? aee(errno) : 0;
	case 31: {	/* stat(path, rout 96 bytes) */
		if (map_path(S(c, 1), "ADSP_LIBRARY_PATH", path, sizeof(path)) || !(o = out_buf(c, 0, 96)))
			return AEE_EBADPARM;
		int rc = stat(path, &st);
		CLOG("frpc: < stat \"%s\" -> %s\n", S(c, 1), rc ? strerror(errno) : "ok");
		if (rc)
			return aee(errno);
		put64(o + 8, (uint64_t)st.st_dev);
		put64(o + 16, (uint64_t)st.st_ino);
		put32(o + 24, (uint32_t)st.st_mode);
		put32(o + 28, (uint32_t)st.st_nlink);
		put64(o + 32, (uint64_t)st.st_rdev);
		put64(o + 40, (uint64_t)st.st_size);
		put64(o + 48, (uint64_t)st.st_atimespec.tv_sec);
		put64(o + 56, (uint64_t)st.st_atimespec.tv_nsec);
		put64(o + 64, (uint64_t)st.st_mtimespec.tv_sec);
		put64(o + 72, (uint64_t)st.st_mtimespec.tv_nsec);
		put64(o + 80, (uint64_t)st.st_ctimespec.tv_sec);
		put64(o + 88, (uint64_t)st.st_ctimespec.tv_nsec);
		return 0;
	}
	case 32:	/* ftrunc(fd, length) */
		return ftruncate(fd, (off_t)W(c, 1)) ? aee(errno) : 0;
	case 33: {	/* frename(old, new) */
		char to[512];
		if (map_path(S(c, 1), NULL, path, sizeof(path)) || map_path(S(c, 2), NULL, to, sizeof(to)))
			return AEE_EBADPARM;
		CLOG("frpc: < rename \"%s\" -> \"%s\"\n", S(c, 1), S(c, 2));
		return rename(path, to) ? aee(errno) : 0;
	}
	}
	return -1;	/* not implemented */
}

static int remotectl(struct call *c)
{
	if (c->method == 0) {	/* open(name, rout handle, rout err[errlen], rout dlerr) */
		static const char *names[] = { "remotectl", "apps_std", "apps_mem" };
		const char *name = S(c, 1);
		uint8_t *o = out_buf(c, 0, 8);
		out_buf(c, 1, W(c, 1));
		if (!o)
			return AEE_EBADPARM;
		for (unsigned i = 0; i < 3; i++)
			if (!strcmp(name, names[i])) {
				put32(o, i);
				CLOG("frpc: < remotectl open \"%s\" -> handle %u\n", name, i);
				return 0;
			}
		printf("[%6llu] frpc: < remotectl open \"%s\": no such local interface\n", (unsigned long long)link_ms(), name);
		put32(o + 4, (uint32_t)-5);
		return -5;
	}
	if (c->method == 1) {	/* close(handle, rout err[errlen], rout dlerr) */
		out_buf(c, 0, 4);
		out_buf(c, 1, W(c, 1));
		return 0;
	}
	return -1;
}

static void dispatch(struct call *c, int32_t *result)
{
	int rc = -1;
	if (c->handle == 0)
		rc = remotectl(c);
	else if (c->handle == 1)
		rc = apps_std(c);
	if (rc == -1) {
		/* apps_mem (handle 2) or a method we lack: say so loudly, once per method */
		static uint32_t seen[64];
		uint32_t key = (c->handle << 8) | c->method;
		int i;
		for (i = 0; i < 64 && seen[i] && seen[i] != key + 1; i++)
			;
		if (i < 64 && !seen[i]) {
			seen[i] = key + 1;
			printf("[%6llu] #### FASTRPC REVERSE CALL NOT IMPLEMENTED: handle %u method %u (sc %#x, in", (unsigned long long)link_ms(),
			       c->handle, c->method, c->sc);
			for (unsigned k = 0; k < c->nin; k++)
				printf(" %u", c->in[k].n);
			printf(")\n");
		}
		rc = AEE_EUNSUPPORTED;
	}
	*result = rc;
}

void frpc_service(void)
{
	static int busy;
	if (busy || !next_p.posted || !next_p.done)
		return;
	busy = 1;
	next_p.posted = 0;
	if (next_p.ret) {
		printf("[%6llu] fastrpc: next2 -> %d\n", (unsigned long long)link_ms(), next_p.ret);
		if (++listener_fail < 5 && post_next2(0, 0xffffffffu, NULL, 0) == 0) {
			busy = 0;
			return;
		}
		printf("[%6llu] #### FASTRPC LISTENER STOPPED\n", (unsigned long long)link_ms());
		busy = 0;
		return;
	}
	uint32_t prim[4];
	fetch(&next_p, 0, prim);
	struct call c = { .handle = prim[1], .sc = prim[2] };
	uint32_t rctx = prim[0], inlen = prim[3];
	int32_t result = 0;
	frpc_reverse_calls++;
	c.nin = SC_INBUFS(c.sc);
	c.nout = SC_OUTBUFS(c.sc);
	c.method = SC_METHOD(c.sc);
	if (inlen > INCAP || c.nin > FRPC_MAX_ARGS || c.nout > FRPC_MAX_ARGS || (c.sc & 0xff)) {
		printf("[%6llu] #### FASTRPC REVERSE CALL TOO BIG OR WITH HANDLES: handle %u sc %#x, %u bytes in\n",
		       (unsigned long long)link_ms(), c.handle, c.sc, inlen);
		result = AEE_EBADPARM;
		c.nout = 0;
	} else {
		next_p.outlen[1] = inlen;	/* only what the DSP wrote */
		fetch(&next_p, 1, iblob);
		if (frpc_blob_decode(iblob, inlen, c.nin, c.in)) {
			printf("[%6llu] fastrpc: malformed reverse call (handle %u sc %#x, %u bytes)\n", (unsigned long long)link_ms(),
			       c.handle, c.sc, inlen);
			result = AEE_EBADPARM;
		} else {
			if (c.method == 31 && c.nin && c.in[0].n >= 4) {
				c.method = get32(c.in[0].p);
				c.ext = 1;
			}
			dispatch(&c, &result);
		}
	}
	for (unsigned j = 0; j < c.nout; j++)
		if (!c.out[j].p)
			c.out[j].n = 0;
	size_t bl = frpc_blob_encode(c.out, c.nout, oblob, sizeof(oblob));
	if (bl == (size_t)-1) {
		bl = 0;
		result = AEE_ENOMEMORY;
	}
	if (post_next2(rctx, (uint32_t)result, oblob, (uint32_t)bl))
		printf("[%6llu] #### FASTRPC LISTENER STOPPED (cannot post next2)\n", (unsigned long long)link_ms());
	busy = 0;
}
#endif
