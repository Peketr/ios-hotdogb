/*
 * btd's end of the Bluetooth UART bridge (see pipe.h): a socket to the kernel control
 * "com.apple.uart.bluetooth" (kernel 0165, bsd/dev/sweet_btuart.c), claimed as the chip side. What the host
 * (bluetoothd, BlueTool) writes to its socket arrives here; what btd writes goes to the host.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "pipe.h"

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
#define BTUART_CTL_NAME		"com.apple.uart.bluetooth"
#define BTUART_GET_TERMIOS	0x42540002u
#define BTUART_CLAIM_CHIP	0x42540010u
#define BTUART_GET_STATS	0x42540011u

static int ctl = -1;

int pipe_open(void)
{
	struct ctl_info_ ci;
	struct sockaddr_ctl_ sc;
	int one = 1;
	if ((ctl = socket(PF_SYSTEM_, SOCK_STREAM, SYSPROTO_CONTROL_)) < 0) {
		printf("pipe: control socket: %s\n", strerror(errno));
		return -1;
	}
	memset(&ci, 0, sizeof(ci));
	strlcpy(ci.ctl_name, BTUART_CTL_NAME, sizeof(ci.ctl_name));
	if (ioctl(ctl, CTLIOCGINFO_, &ci)) {
		printf("pipe: no kernel control %s (%s): kernel 0165+ needed\n", BTUART_CTL_NAME, strerror(errno));
		goto fail;
	}
	memset(&sc, 0, sizeof(sc));
	sc.sc_len = sizeof(sc);
	sc.sc_family = PF_SYSTEM_;
	sc.ss_sysaddr = AF_SYS_CONTROL_;
	sc.sc_id = ci.ctl_id;
	if (connect(ctl, (struct sockaddr *)&sc, sizeof(sc))) {
		printf("pipe: connect control: %s\n", strerror(errno));
		goto fail;
	}
	if (setsockopt(ctl, SYSPROTO_CONTROL_, (int)BTUART_CLAIM_CHIP, &one, sizeof(one))) {
		printf("pipe: BTUART_CLAIM_CHIP: %s\n", strerror(errno));
		goto fail;
	}
	fcntl(ctl, F_SETFL, fcntl(ctl, F_GETFL) | O_NONBLOCK);
	printf("pipe: chip side of %s claimed\n", BTUART_CTL_NAME);
	return 0;
fail:
	close(ctl);
	ctl = -1;
	return -1;
}

void pipe_close(void)
{
	if (ctl >= 0)
		close(ctl);
	ctl = -1;
}

size_t pipe_read(uint8_t *buf, size_t max)
{
	ssize_t n = recv(ctl, buf, max, 0);
	return n > 0 ? (size_t)n : 0;
}

int pipe_write(const uint8_t *buf, size_t len)
{
	size_t off = 0;
	for (int tries = 0; off < len && tries < 200;) {
		ssize_t n = send(ctl, buf + off, len - off, 0);
		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == ENXIO)
			return -1;	/* no host connected: bytes dropped by the kernel anyway */
		usleep(500);	/* EAGAIN: the host is behind */
		tries++;
	}
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

int pipe_host(uint32_t st[4])
{
	socklen_t l = 4 * sizeof(uint32_t);
	return getsockopt(ctl, SYSPROTO_CONTROL_, (int)BTUART_GET_STATS, st, &l);
}
