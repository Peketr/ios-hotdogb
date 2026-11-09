/*
 * Qualcomm SSC (Snapdragon Sensor Core) client over QMI service 400 (SNS client) on QRTR: SUID lookup of the
 * default "accel", "gyro" and "mag" sensors, continuous streams, samples to /var/local/imu.txt.
 * Messages: protobuf as postmarketOS libssc reconstructed them (ssc-common.proto, ssc-sensor-suid.proto):
 *   request { 1 uid { 1 fixed64 low, 2 fixed64 high }, 2 fixed32 msg_id, 3 config { 1 processor, 2 suspend },
 *             4 body { 2 bytes msg, 3 bool passive } }
 *   event   { 1 uid, 2 repeated { 1 fixed32 msg_id, 2 fixed64 timestamp, 3 bytes msg } }
 * QMI: request 0x20 (TLV 1 = u16 length + protobuf, TLV 0x10 = jumbo reports), response 0x20 (TLV 0x10 client id),
 * reports 0x21 / 0x22 (jumbo) (TLV 1 client id, TLV 2 = u16 length + protobuf).
 */
#ifndef SSC_H
#define SSC_H
#include <stdint.h>
#include <stddef.h>

#define SSC_SERVICE		400
#define SSC_MSG_SUID_REQ	512
#define SSC_MSG_SUID_EVENT	768
#define SSC_MSG_ENABLE		513	/* std sensor config { 1 float sample_rate } */
#define SSC_MSG_CONFIG_EVENT	768	/* physical config event of a sensor */
#define SSC_MSG_SAMPLE		1025	/* std sensor event { 1 repeated float data, 2 status } */
#define SSC_MSG_DISABLE		10
#define SSC_SUID_MAGIC		0xababababababababull

/* protobuf helpers (host-tested) */
struct pb {
	uint8_t *b;
	size_t n, cap;
};
void pb_varint(struct pb *p, unsigned field, uint64_t v);
void pb_fixed32(struct pb *p, unsigned field, uint32_t v);
void pb_fixed64(struct pb *p, unsigned field, uint64_t v);
void pb_float(struct pb *p, unsigned field, float v);
void pb_bytes(struct pb *p, unsigned field, const void *d, size_t len);
/* Iterate the fields of a message: returns 1 per field (wt 0: v, 1/5: v, 2: d/len), 0 at the end, -1 malformed. */
struct pbf {
	unsigned field, wt;
	uint64_t v;
	const uint8_t *d;
	size_t len;
};
int pb_next(const uint8_t **p, const uint8_t *end, struct pbf *f);

/* request: the protobuf of one sensor request (uid, msg_id, APSS / wakeup config, body payload) */
size_t ssc_request(uint8_t *out, size_t cap, uint64_t low, uint64_t high, uint32_t msg_id, const void *payload, size_t plen);

/* the QRTR side (sensord.c dispatches to it) */
void ssc_server(uint32_t node, uint32_t port);	/* SNS client service seen */
int ssc_port(uint32_t port);			/* one of our client ports */
void ssc_handle(uint32_t port, const uint8_t *d, uint32_t n);
void ssc_tick(void);				/* retries, the output file */

#endif
