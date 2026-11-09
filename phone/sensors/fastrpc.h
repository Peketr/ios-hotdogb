/*
 * FastRPC from userland (no kernel driver): the apps side of Qualcomm's adsprpc protocol on the
 * "fastrpcglink-apps-dsp" GLINK channel, and the reverse-RPC listener with an apps_std file server, as
 * Android's sscrpcd / postmarketOS's hexagonrpcd run it for the SLPI's sensors PD.
 * Wire format after Linux drivers/misc/fastrpc.c (fastrpc_msg, payload metadata) and linux-msm/hexagonrpc
 * (adsp_listener next2, inbuf/outbuf blobs, remotectl + apps_std method layouts).
 */
#ifndef FASTRPC_H
#define FASTRPC_H
#include <stdint.h>
#include <stddef.h>

#define SC_MAKE(m, in, out)	((((uint32_t)(m) & 0x1f) << 24) | (((uint32_t)(in) & 0xff) << 16) | (((uint32_t)(out) & 0xff) << 8))
#define SC_METHOD(sc)		(((sc) >> 24) & 0x1f)
#define SC_INBUFS(sc)		(((sc) >> 16) & 0xff)
#define SC_OUTBUFS(sc)		(((sc) >> 8) & 0xff)
#define SC_HANDLES(sc)		((((sc) >> 4) & 0xf) + ((sc) & 0xf))

/* AEE error codes (hexagonrpc include/libhexagonrpc/error.h) */
#define AEE_EFAILED		1
#define AEE_ENOMEMORY		2
#define AEE_EBADPARM		14
#define AEE_EUNSUPPORTED	20
#define AEE_ENOSUCH		39

#define FRPC_MAX_ARGS		16

struct iob {
	uint8_t *p;
	uint32_t n;
};

/* ---- pure encoders/decoders (host-tested in test_sensord.c) ---- */

/* The listener blobs: per buffer [u32 size][pad to 8 if size != 0][data]. Returns bytes, (size_t)-1 if too big. */
size_t frpc_blob_encode(const struct iob *b, unsigned n, uint8_t *dst, size_t cap);
/* Splits a blob into n buffers (pointers into blob); -1 if malformed. */
int frpc_blob_decode(uint8_t *blob, size_t len, unsigned n, struct iob *out);
/* Payload of one invoke at dsp_base: metadata (remote args, invoke bufs, pages, fd/crc lists) then each non-empty
 * buffer at a 128-byte boundary; inputs copied in. Fills arg_off[i] (offset of buffer i). Returns the length, 0 if
 * it does not fit. */
size_t frpc_payload(uint8_t *p, size_t cap, uint64_t dsp_base, uint32_t sc, const struct iob *in,
		    const uint32_t *outlen, uint32_t *arg_off);

/* ---- the transport ---- */

/* shm_pa: a physically contiguous block outside XNU's RAM (DSP-visible through a translation-off SMMU bank);
 * dsp_msw: the upper address word the DSP needs (the context bank: SMMU stream 0x5a1 & 0xf = 1). */
void frpc_init(uint64_t shm_pa, uint32_t shm_size, uint32_t dsp_msw, const char *root);
void frpc_rx(const uint8_t *d, uint32_t n);	/* every packet on the fastrpc channel */
int frpc_attach_sensors(void);			/* 0 = the sensors PD accepted us */
int frpc_listener_start(void);			/* adsp_default_listener register + init2 + first next2 */
void frpc_service(void);			/* from the poll loop: answers reverse calls */
extern unsigned frpc_reverse_calls, frpc_files_opened;

#endif
