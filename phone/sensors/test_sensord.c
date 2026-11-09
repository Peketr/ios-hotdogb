/*
 * Host test of sensord's encoders (no phone): FastRPC listener blobs, invoke payload layout, the SSC request
 * protobuf. Build + run (Linux/WSL): cc -DFRPC_PURE -DSSC_PURE -o /tmp/t test_sensord.c fastrpc.c ssc.c && /tmp/t
 */
#include <stdio.h>
#include <string.h>
#include "fastrpc.h"
#include "ssc.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t g32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t g64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static void test_blob(void)
{
	uint8_t a[16], c[5] = { 1, 2, 3, 4, 5 }, d[20];
	for (int i = 0; i < 16; i++)
		a[i] = (uint8_t)i;
	memset(d, 0xee, sizeof(d));
	struct iob b[4] = { { a, 16 }, { NULL, 0 }, { c, 5 }, { d, 20 } };
	uint8_t blob[256];
	size_t n = frpc_blob_encode(b, 4, blob, sizeof(blob));
	/* [16][pad4][a] = 0..23, [0] 24..27, [5] 28..31 data 32..36, [20] 37..40 pad 41..47 data 48..67 */
	CHECK(n == 68);
	CHECK(g32(blob) == 16 && blob[8] == 0 && blob[23] == 15);
	CHECK(g32(blob + 24) == 0 && g32(blob + 28) == 5 && blob[32] == 1 && blob[36] == 5);
	CHECK(g32(blob + 37) == 20 && blob[48] == 0xee && blob[67] == 0xee);
	struct iob o[4];
	CHECK(frpc_blob_decode(blob, n, 4, o) == 0);
	CHECK(o[0].n == 16 && o[0].p == blob + 8 && o[1].n == 0 && o[2].n == 5 && o[2].p == blob + 32);
	CHECK(o[3].n == 20 && o[3].p == blob + 48);
	CHECK(frpc_blob_decode(blob, n - 1, 4, o) == -1);
	CHECK(frpc_blob_encode(b, 4, blob, 60) == (size_t)-1);
	CHECK(frpc_blob_encode(b, 0, blob, 0) == 0);
}

static void test_payload(void)
{
	static uint8_t p[0x40000];
	uint32_t prim[4] = { 7, 0xffffffff, 5, 256 };
	uint8_t prev[5] = { 9, 9, 9, 9, 9 };
	struct iob in[2] = { { (uint8_t *)prim, 16 }, { prev, 5 } };
	uint32_t outlen[2] = { 16, 256 }, off[4];
	uint64_t base = 0x19e200000ull + 0x40000;
	uint32_t sc = SC_MAKE(4, 2, 2);
	CHECK(sc == 0x04020200);
	size_t n = frpc_payload(p, sizeof(p), base, sc, in, outlen, off);
	size_t meta = 40 * 4 + 128 + 256 + 4;	/* 548 */
	CHECK(off[0] == 640 && off[1] == 768 && off[2] == 896 && off[3] == 1024);
	CHECK(n == 1024 + 256);
	/* remote args */
	CHECK(g64(p + 0) == base + 640 && g64(p + 8) == 16);
	CHECK(g64(p + 16) == base + 768 && g64(p + 24) == 5);
	CHECK(g64(p + 48) == base + 1024 && g64(p + 56) == 256);
	/* invoke bufs at 64: { num, pgidx } */
	CHECK(g32(p + 64) == 1 && g32(p + 68) == 0 && g32(p + 64 + 8 * 3 + 4) == 3);
	/* pages at 96: page of the buffer, one page each */
	CHECK(g64(p + 96) == (base & ~0xfffull) && g64(p + 104) == 0x1000);
	CHECK(p[640] == 7 && p[768] == 9 && p[772] == 9);
	CHECK(meta <= 640);
	/* a buffer across a page boundary spans two pages */
	uint8_t big[0x1000];
	memset(big, 1, sizeof(big));
	struct iob in2[1] = { { big, sizeof(big) } };
	n = frpc_payload(p, sizeof(p), base, SC_MAKE(5, 1, 0), in2, NULL, off);
	CHECK(off[0] == 512 && n == 512 + 0x1000 && g64(p + 24) == ((base + 512) & ~0xfffull) && g64(p + 32) == 0x2000);
	CHECK(frpc_payload(p, 600, base, SC_MAKE(5, 1, 0), in2, NULL, off) == 0);
}

static void test_protobuf(void)
{
	uint8_t pl[64], out[256];
	struct pb p = { pl, 0, sizeof(pl) };
	pb_bytes(&p, 1, "accel", 5);
	pb_varint(&p, 2, 0);
	pb_varint(&p, 3, 1);
	static const uint8_t want_pl[] = { 0x0a, 5, 'a', 'c', 'c', 'e', 'l', 0x10, 0, 0x18, 1 };
	CHECK(p.n == sizeof(want_pl) && !memcmp(pl, want_pl, p.n));
	size_t n = ssc_request(out, sizeof(out), SSC_SUID_MAGIC, SSC_SUID_MAGIC, 512, pl, p.n);
	/* 0a 12 (09 ab*8 11 ab*8) 15 00 02 00 00 1a 04 (08 01 10 00) 22 0d (12 0b <payload>) */
	CHECK(n == 2 + 18 + 5 + 6 + 2 + 2 + 11);
	CHECK(out[0] == 0x0a && out[1] == 0x12 && out[2] == 0x09 && out[3] == 0xab && out[11] == 0x11);
	CHECK(out[20] == 0x15 && g32(out + 21) == 512);
	CHECK(out[25] == 0x1a && out[26] == 4 && out[27] == 0x08 && out[28] == 1 && out[29] == 0x10 && out[30] == 0);
	CHECK(out[31] == 0x22 && out[32] == 13 && out[33] == 0x12 && out[34] == 11 && !memcmp(out + 35, want_pl, 11));
	/* decode it back */
	const uint8_t *q = out;
	struct pbf f;
	int k = 0;
	while (pb_next(&q, out + n, &f) > 0)
		k++;
	CHECK(k == 4 && q == out + n);
	/* a sample: unpacked + packed floats, status */
	uint8_t ev[64];
	struct pb e = { ev, 0, sizeof(ev) };
	pb_float(&e, 1, 9.81f);
	float pk[2] = { 1.5f, -2.25f };
	pb_bytes(&e, 1, pk, 8);
	pb_varint(&e, 2, 3);
	q = ev;
	float got[3];
	unsigned nv = 0;
	while (pb_next(&q, ev + e.n, &f) > 0) {
		if (f.field == 1 && f.wt == 5) {
			uint32_t u = (uint32_t)f.v;
			memcpy(&got[nv++], &u, 4);
		} else if (f.field == 1 && f.wt == 2) {
			for (size_t o = 0; o + 4 <= f.len; o += 4)
				memcpy(&got[nv++], f.d + o, 4);
		} else
			CHECK(f.field == 2 && f.v == 3);
	}
	CHECK(nv == 3 && got[0] == 9.81f && got[1] == 1.5f && got[2] == -2.25f);
	uint8_t bad[2] = { 0x0a, 5 };
	q = bad;
	CHECK(pb_next(&q, bad + 2, &f) == -1);
}

int main(void)
{
	test_blob();
	test_payload();
	test_protobuf();
	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails != 0;
}
