/*
 * bluetoothd's side of the Bluetooth UART: an anonymous Skywalk user pipe that btd registers and serves
 * (port 1), its UUID handed to the kernel control "com.apple.uart.sk.bluetooth" (kernel 0163,
 * bsd/dev/sweet_btuart.c), which gives it to bluetoothd (port 0). H4 bytes travel in the pipe's slots.
 */
#ifndef BT_PIPE_H
#define BT_PIPE_H
#include <stddef.h>
#include <stdint.h>

int pipe_open(void);			/* register, open port 1, publish the UUID; 0 = ok */
void pipe_close(void);			/* unpublish and destroy */
size_t pipe_read(uint8_t *buf, size_t max);	/* bytes bluetoothd sent (no wait) */
int pipe_write(const uint8_t *buf, size_t len);	/* to bluetoothd; 0 = ok */
int pipe_termios_speed(uint64_t *speed);	/* the speed bluetoothd last set (0 = ok) */

#endif
