/*
 * btd's end of the Bluetooth UART bridge: the kernel control "com.apple.uart.bluetooth" (kernel 0165,
 * bsd/dev/sweet_btuart.c), claimed as the chip side. bluetoothd connects to the same control as the host
 * (BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3000000); the kernel copies H4 bytes between them.
 */
#ifndef BT_PIPE_H
#define BT_PIPE_H
#include <stddef.h>
#include <stdint.h>

int pipe_open(void);			/* connect, claim the chip side; 0 = ok */
void pipe_close(void);
size_t pipe_read(uint8_t *buf, size_t max);	/* bytes bluetoothd sent (no wait) */
int pipe_write(const uint8_t *buf, size_t len);	/* to bluetoothd; 0 = ok */
int pipe_termios_speed(uint64_t *speed);	/* the speed bluetoothd last set (0 = ok) */

int pipe_host(uint32_t st[4]);		/* host unit, host pid, drops up, drops down (0 = ok) */

#endif
