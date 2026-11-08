/*
 * The hotdogb Bluetooth UART (qupv3_se13_4uart: QUPv3 wrapper 2 SE3 at 0xc8c000, GPIO 43 CTS / 44 RTS /
 * 45 TX / 46 RX), driven from userland through /dev/pmem: GCC clocks, TLMM pins, GENI UART in FIFO mode,
 * polled (no IRQ, no DMA). Ported from Linux qcom_geni_serial.c / qcom-geni-se.c / gcc-sm8150.c.
 */
#ifndef BT_UART_H
#define BT_UART_H
#include <stddef.h>
#include <stdint.h>

void uart_regs(void);			/* read-only dump (clocks voted first so the SE can be read) */
int uart_init(void);			/* clocks, pins, SE as 8N1 UART; RX running. 0 = ok */
int uart_baud_ok(unsigned baud);	/* 1 if the SE clock can make this baud rate (2%) */
int uart_set_baud(unsigned baud);	/* 0 = ok */
void uart_flow(int on);			/* 1: RTS/CTS by hardware; 0: RTS deasserted, CTS ignored */
int uart_write(const void *buf, size_t len, unsigned timeout_ms);	/* 0 = all sent */
size_t uart_read(uint8_t *buf, size_t max);	/* what the RX FIFO holds now (no wait) */
void uart_flush_rx(void);

#endif
