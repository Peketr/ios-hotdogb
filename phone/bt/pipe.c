/*
 * Skywalk user pipe for bluetoothd (see pipe.h). The os_nexus / os_channel calls are libsystem_kernel's
 * private Skywalk API (xnu-8796 bsd/skywalk/nexus/os_nexus.h, channel/os_channel.h); declared here since the
 * SDK has no headers for them. Slot mode, as bluetoothd uses it: one TX and one RX ring, 1 KB per slot.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include "pipe.h"

/* ---- libsystem_kernel Skywalk API ---- */
typedef struct nexus_controller *nexus_controller_t;
typedef struct nexus_attr *nexus_attr_t;
typedef struct channel *channel_t;
typedef struct channel_ring_desc *channel_ring_t;
typedef struct __slot_desc *channel_slot_t;
typedef uint16_t nexus_port_t;
typedef uint32_t ring_id_t;
typedef uint8_t nexus_name_t[64];
typedef struct slot_prop {
	uint16_t sp_flags;
	uint16_t sp_len;
	uint32_t sp_idx;
	uint64_t sp_ext_ptr;
	uint64_t sp_buf_ptr;
	uint64_t sp_mdata_ptr;
	uint32_t _sp_pad[8];
} slot_prop_t __attribute__((aligned(8)));
enum { NEXUS_TYPE_USER_PIPE = 0 };
enum { NEXUS_ATTR_TX_RINGS, NEXUS_ATTR_RX_RINGS, NEXUS_ATTR_TX_SLOTS, NEXUS_ATTR_RX_SLOTS, NEXUS_ATTR_SLOT_BUF_SIZE,
	NEXUS_ATTR_SLOT_META_SIZE, NEXUS_ATTR_ANONYMOUS };
enum { CHANNEL_FIRST_TX_RING, CHANNEL_LAST_TX_RING, CHANNEL_FIRST_RX_RING, CHANNEL_LAST_RX_RING };
enum { CHANNEL_SYNC_TX, CHANNEL_SYNC_RX };
#define NEXUS_PORT_USER_PIPE_CLIENT 0
#define NEXUS_PORT_USER_PIPE_SERVER 1

extern nexus_controller_t os_nexus_controller_create(void);
extern void os_nexus_controller_destroy(nexus_controller_t);
extern nexus_attr_t os_nexus_attr_create(void);
extern int os_nexus_attr_set(nexus_attr_t, int type, uint64_t value);
extern void os_nexus_attr_destroy(nexus_attr_t);
extern int os_nexus_controller_register_provider(nexus_controller_t, const nexus_name_t, int type, nexus_attr_t,
	uuid_t *prov_uuid);
extern int os_nexus_controller_alloc_provider_instance(nexus_controller_t, const uuid_t prov_uuid, uuid_t *nx_uuid);
extern int os_nexus_controller_free_provider_instance(nexus_controller_t, const uuid_t nx_uuid);
extern int os_nexus_controller_deregister_provider(nexus_controller_t, const uuid_t prov_uuid);
extern channel_t os_channel_create(const uuid_t uuid, nexus_port_t port);
extern void os_channel_destroy(channel_t);
extern int os_channel_sync(channel_t, int mode);
extern ring_id_t os_channel_ring_id(channel_t, int type);
extern channel_ring_t os_channel_tx_ring(channel_t, ring_id_t);
extern channel_ring_t os_channel_rx_ring(channel_t, ring_id_t);
extern channel_slot_t os_channel_get_next_slot(channel_ring_t, channel_slot_t, slot_prop_t *);
extern int os_channel_advance_slot(channel_ring_t, channel_slot_t);
extern void os_channel_set_slot_properties(channel_ring_t, channel_slot_t, const slot_prop_t *);

/* ---- kernel control (sys/kern_control.h, sys/sys_domain.h) ---- */
#define PF_SYSTEM_		32
#define SYSPROTO_CONTROL_	2
#define AF_SYS_CONTROL_		2
#define CTLIOCGINFO_		0xc0644e03u
struct ctl_info_ {
	uint32_t ctl_id;
	char ctl_name[96];
};
struct sockaddr_ctl_ {
	uint8_t sc_len, sc_family;
	uint16_t ss_sysaddr;
	uint32_t sc_id, sc_unit, sc_reserved[5];
};
#define BTUART_SET_UUID		0x42540001u
#define BTUART_GET_TERMIOS	0x42540002u
#define SLOT_BUF		1024
#define SLOTS			64

static nexus_controller_t nxctl;
static uuid_t prov, inst;
static channel_t ch;
static channel_ring_t txr, rxr;
static int ctl = -1;

static int ctl_open(void)
{
	struct ctl_info_ ci;
	struct sockaddr_ctl_ sc;
	int fd = socket(PF_SYSTEM_, SOCK_STREAM, SYSPROTO_CONTROL_);
	if (fd < 0) {
		printf("pipe: control socket: %s\n", strerror(errno));
		return -1;
	}
	memset(&ci, 0, sizeof(ci));
	strlcpy(ci.ctl_name, "com.apple.uart.sk.bluetooth", sizeof(ci.ctl_name));
	if (ioctl(fd, CTLIOCGINFO_, &ci)) {
		printf("pipe: no kernel control com.apple.uart.sk.bluetooth (%s): kernel 0163+ needed\n", strerror(errno));
		close(fd);
		return -1;
	}
	memset(&sc, 0, sizeof(sc));
	sc.sc_len = sizeof(sc);
	sc.sc_family = PF_SYSTEM_;
	sc.ss_sysaddr = AF_SYS_CONTROL_;
	sc.sc_id = ci.ctl_id;
	if (connect(fd, (struct sockaddr *)&sc, sizeof(sc))) {
		printf("pipe: connect control: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int publish(const uuid_t u)
{
	if (setsockopt(ctl, SYSPROTO_CONTROL_, (int)BTUART_SET_UUID, u, sizeof(uuid_t))) {
		printf("pipe: BTUART_SET_UUID: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

int pipe_open(void)
{
	nexus_name_t name = "org.iosonpixel.btuart";
	char s[37];
	if ((ctl = ctl_open()) < 0)
		return -1;
	if (!(nxctl = os_nexus_controller_create())) {
		printf("pipe: os_nexus_controller_create: %s\n", strerror(errno));
		return -1;
	}
	nexus_attr_t a = os_nexus_attr_create();
	os_nexus_attr_set(a, NEXUS_ATTR_ANONYMOUS, 1);
	os_nexus_attr_set(a, NEXUS_ATTR_TX_SLOTS, SLOTS);
	os_nexus_attr_set(a, NEXUS_ATTR_RX_SLOTS, SLOTS);
	os_nexus_attr_set(a, NEXUS_ATTR_SLOT_BUF_SIZE, SLOT_BUF);
	int rc = os_nexus_controller_register_provider(nxctl, name, NEXUS_TYPE_USER_PIPE, a, &prov);
	os_nexus_attr_destroy(a);
	if (rc) {
		printf("pipe: register user pipe provider: %d (%s)\n", rc, strerror(errno));
		return -1;
	}
	if ((rc = os_nexus_controller_alloc_provider_instance(nxctl, prov, &inst))) {
		printf("pipe: alloc instance: %d (%s)\n", rc, strerror(errno));
		return -1;
	}
	if (!(ch = os_channel_create(inst, NEXUS_PORT_USER_PIPE_SERVER))) {
		printf("pipe: open server port: %s\n", strerror(errno));
		return -1;
	}
	txr = os_channel_tx_ring(ch, os_channel_ring_id(ch, CHANNEL_FIRST_TX_RING));
	rxr = os_channel_rx_ring(ch, os_channel_ring_id(ch, CHANNEL_FIRST_RX_RING));
	if (!txr || !rxr) {
		printf("pipe: no rings\n");
		return -1;
	}
	uuid_unparse_upper(inst, s);
	printf("pipe: user pipe %s, server port open\n", s);
	return publish(inst);
}

void pipe_close(void)
{
	uuid_t z = { 0 };
	if (ctl >= 0)
		publish(z);
	if (ch)
		os_channel_destroy(ch);
	if (nxctl) {
		os_nexus_controller_free_provider_instance(nxctl, inst);
		os_nexus_controller_deregister_provider(nxctl, prov);
		os_nexus_controller_destroy(nxctl);
	}
	ch = NULL;
	nxctl = NULL;
}

size_t pipe_read(uint8_t *buf, size_t max)
{
	slot_prop_t p;
	channel_slot_t s, last = NULL;
	size_t n = 0;
	if (os_channel_sync(ch, CHANNEL_SYNC_RX))
		return 0;
	while (n + SLOT_BUF <= max && (s = os_channel_get_next_slot(rxr, last, &p)) != NULL) {
		memcpy(buf + n, (const void *)(uintptr_t)p.sp_buf_ptr, p.sp_len);
		n += p.sp_len;
		last = s;
	}
	if (last)
		os_channel_advance_slot(rxr, last);
	return n;
}

int pipe_write(const uint8_t *buf, size_t len)
{
	slot_prop_t p;
	channel_slot_t s, last = NULL;
	size_t off = 0;
	for (int tries = 0; off < len && tries < 200;) {
		s = os_channel_get_next_slot(txr, last, &p);
		if (!s) {
			if (last) {
				os_channel_advance_slot(txr, last);
				last = NULL;
			}
			os_channel_sync(ch, CHANNEL_SYNC_TX);
			usleep(500);
			tries++;
			continue;
		}
		size_t c = len - off < SLOT_BUF ? len - off : SLOT_BUF;
		memcpy((void *)(uintptr_t)p.sp_buf_ptr, buf + off, c);
		p.sp_len = (uint16_t)c;
		os_channel_set_slot_properties(txr, s, &p);
		off += c;
		last = s;
	}
	if (last)
		os_channel_advance_slot(txr, last);
	os_channel_sync(ch, CHANNEL_SYNC_TX);
	return off == len ? 0 : -1;
}

int pipe_termios_speed(uint64_t *speed)
{
	uint8_t t[72];
	socklen_t l = sizeof(t);
	if (getsockopt(ctl, SYSPROTO_CONTROL_, (int)BTUART_GET_TERMIOS, t, &l) || l != sizeof(t))
		return -1;
	memcpy(speed, t + 64, 8);
	return 0;
}
