/*
 * GENI I2C in FIFO mode, polled, from userland (Linux i2c-qcom-geni.c / qcom-geni-se.c): the speaker amps' bus,
 * QUPv3 wrap0 SE4 at 0x890000 (stock DT qupv3_se4_i2c, pins 51/52 = qup4, already muxed with pull-ups).
 * As XBL/ABL leave it (2026-11-08): wrap0 + s4 clocks voted (GCC 0x5200c bits 6-9, 14), source XO 19.2 MHz,
 * SER_M_CLK_CFG 0x21 (div 2), GENI_IF_DISABLE_RO = 0 (FIFO mode allowed, unlike the touch SE), firmware 0x303
 * (I2C). Never access SE + 0x2000..: on the touch SE that region reset the phone (TrustZone).
 */
#include <stdio.h>
#include <string.h>
#include "../wifi/pmem.h"
#include "i2c.h"

#define SE		0x00890000ull
#define SE_MCLK		0x048
#define SE_CLK_SEL	0x07c
#define SE_DMA		0x258
#define SE_TX_PACK0	0x260
#define SE_TX_PACK1	0x264
#define SE_I2C_TX_LEN	0x26c
#define SE_I2C_RX_LEN	0x270
#define SE_I2C_SCL	0x278
#define SE_RX_PACK0	0x284
#define SE_RX_PACK1	0x288
#define SE_CMD		0x600
#define SE_CMD_CTRL	0x604
#define SE_IRQ		0x610
#define SE_IRQ_EN	0x614
#define SE_IRQ_CLR	0x618
#define SE_TX		0x700
#define SE_RX		0x780
#define SE_TX_COUNT	0x800
#define SE_RX_COUNT	0x804

#define PACK0		0x0007f8feu	/* 8-bit words, 4 per FIFO entry, LSB first */
#define PACK1		0x000ffefeu
#define MCLK_400K	((2u << 4) | 1u)
#define SCL_400K	((5u << 20) | (12u << 10) | 24u)
#define CMD_WRITE	1u
#define CMD_READ	2u
#define STOP_STRETCH	(1u << 2)
#define IRQ_DONE	(1u << 0)
#define IRQ_NACK	(1u << 10)	/* M_GP_IRQ_1 */
#define IRQ_ERRORS	(0x3eu | (0x3fu << 9) | (3u << 28))
#define IRQ_EVENTS	(IRQ_DONE | IRQ_ERRORS | (3u << 26) | (1u << 30))

static int ready;

int i2c_init(void)
{
	wr32(SE + SE_DMA, 0);
	wr32(SE + SE_CLK_SEL, 0);
	wr32(SE + SE_MCLK, MCLK_400K);
	wr32(SE + SE_I2C_SCL, SCL_400K);
	wr32(SE + SE_TX_PACK0, PACK0);
	wr32(SE + SE_TX_PACK1, PACK1);
	wr32(SE + SE_RX_PACK0, PACK0);
	wr32(SE + SE_RX_PACK1, PACK1);
	wr32(SE + SE_IRQ_CLR, 0xffffffffu);
	wr32(SE + SE_IRQ_EN, IRQ_EVENTS);
	ready = 1;
	return 0;
}

/* wait for the command to finish; returns 0, -1 = NACK, -2 = other error, -3 = timeout */
static int wait_done(uint32_t *irq_out)
{
	uint32_t irq = 0;
	for (int i = 0; i < 20000; i++) {
		irq = rd32(SE + SE_IRQ);
		if (irq & (IRQ_DONE | IRQ_ERRORS))
			break;
	}
	wr32(SE + SE_IRQ_CLR, irq);
	if (irq_out)
		*irq_out = irq;
	if (irq & IRQ_NACK)
		return -1;
	if (irq & IRQ_ERRORS)
		return -2;
	return (irq & IRQ_DONE) ? 0 : -3;
}

static void abort_cmd(void)
{
	wr32(SE + SE_CMD_CTRL, 1u << 1);	/* M_GENI_CMD_ABORT */
	for (int i = 0; i < 10000; i++)
		if (rd32(SE + SE_IRQ) & (1u << 5))	/* M_CMD_ABORT */
			break;
	wr32(SE + SE_IRQ_CLR, 0xffffffffu);
}

static int xfer_write(uint8_t addr, const uint8_t *buf, unsigned n, int stretch, uint32_t *irq)
{
	wr32(SE + SE_IRQ_CLR, 0xffffffffu);
	wr32(SE + SE_I2C_TX_LEN, n);
	wr32(SE + SE_CMD, (CMD_WRITE << 27) | ((uint32_t)addr << 9) | (stretch ? STOP_STRETCH : 0));
	for (unsigned i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (unsigned k = 0; k < 4 && i + k < n; k++)
			w |= (uint32_t)buf[i + k] << (8 * k);
		wr32(SE + SE_TX, w);
	}
	int rc = wait_done(irq);
	if (rc == -3)
		abort_cmd();
	return rc;
}

static int xfer_read(uint8_t addr, uint8_t *buf, unsigned n, uint32_t *irq)
{
	wr32(SE + SE_IRQ_CLR, 0xffffffffu);
	wr32(SE + SE_I2C_RX_LEN, n);
	wr32(SE + SE_CMD, (CMD_READ << 27) | ((uint32_t)addr << 9));
	unsigned got = 0;
	for (int i = 0; i < 20000 && got < n; i++) {
		uint32_t words = rd32(SE + SE_RX_COUNT) & 0x1ffffffu;
		while (words-- && got < n) {
			uint32_t w = rd32(SE + SE_RX);
			for (unsigned k = 0; k < 4 && got < n; k++)
				buf[got++] = (uint8_t)(w >> (8 * k));
		}
		if (rd32(SE + SE_IRQ) & IRQ_ERRORS)
			break;
	}
	int rc = wait_done(irq);
	if (rc == -3)
		abort_cmd();
	return rc ? rc : (got == n ? 0 : -2);
}

int i2c_write(uint8_t addr, const uint8_t *buf, unsigned n)
{
	uint32_t irq;
	if (!ready)
		i2c_init();
	int rc = xfer_write(addr, buf, n, 0, &irq);
	if (rc)
		printf("i2c: write %02x (%u bytes) -> %d (irq %#x)\n", addr, n, rc, irq);
	return rc;
}

int i2c_write_read(uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	uint32_t irq;
	if (!ready)
		i2c_init();
	int rc = xfer_write(addr, w, wn, 1, &irq);
	if (rc) {
		if (rc != -1)
			printf("i2c: %02x write part -> %d (irq %#x)\n", addr, rc, irq);
		return rc;
	}
	rc = xfer_read(addr, r, rn, &irq);
	if (rc)
		printf("i2c: %02x read part -> %d (irq %#x)\n", addr, rc, irq);
	return rc;
}
