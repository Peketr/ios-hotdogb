/*
 * GENI UART on QUPv3 wrapper 2 SE3 (see uart.h). Sources: Linux drivers/tty/serial/qcom_geni_serial.c
 * (port setup, termios, FIFO TX/RX), drivers/soc/qcom/qcom-geni-se.c (geni_se_init, packing),
 * drivers/clk/qcom/gcc-sm8150.c (gcc_qupv3_wrap2_s3_clk(_src), wrap_2 AHB clocks), clk-rcg2.c (DFS levels,
 * M/N/D), pinctrl-sm8150.c (gpio43-46: EAST tile, qup13 = function 1), the 7T's DT (qcom,qup_uart@0xc8c000,
 * qupv3_se13_ctsrx/rts/tx pin configs). The xnu touch driver (sweet_s3706.c) uses the same wrapper's SE0.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../wifi/pmem.h"
#include "uart.h"

#define SE_PA		0x00c8c000ull
#define WRAP_PA		0x00cc0000ull	/* qcom,qupv3_2_geni_se@cc0000 */
#define GCC_PA		0x00100000ull
#define TLMM_PA		0x03500000ull	/* EAST tile */

#define GCC_VOTE	0x52014u	/* APCS_CLOCK_BRANCH_ENA_VOTE_1 */
#define GCC_VOTE_BITS	(0x1fu | (1u << 7))	/* wrap2 core/core2x, s_ahb, m_ahb (as the touch driver), s3 */
#define GCC_RCG		0x1e4d8u	/* gcc_qupv3_wrap2_s3_clk_src */
static const uint32_t gcc_branches[] = { 0x1e004, 0x1e008, 0x1e4d4 };	/* m_ahb, s_ahb, wrap2_s3 */

/* GENI */
#define GENI_FORCE_DEFAULT	0x020
#define GENI_OUTPUT_CTRL	0x024
#define GENI_CGC_CTRL		0x028
#define SE_GENI_STATUS		0x040
#define GENI_SER_M_CLK_CFG	0x048
#define GENI_SER_S_CLK_CFG	0x04c
#define GENI_IF_DISABLE_RO	0x064
#define GENI_FW_REVISION_RO	0x068
#define SE_GENI_CLK_SEL		0x07c
#define SE_UART_LOOPBACK_CFG	0x22c
#define SE_GENI_BYTE_GRAN	0x254
#define SE_GENI_DMA_MODE_EN	0x258
#define SE_UART_TX_TRANS_CFG	0x25c
#define SE_GENI_TX_PACKING_CFG0	0x260
#define SE_GENI_TX_PACKING_CFG1	0x264
#define SE_UART_TX_WORD_LEN	0x268
#define SE_UART_TX_STOP_BIT_LEN	0x26c
#define SE_UART_TX_TRANS_LEN	0x270
#define SE_UART_RX_TRANS_CFG	0x280
#define SE_GENI_RX_PACKING_CFG0	0x284
#define SE_GENI_RX_PACKING_CFG1	0x288
#define SE_UART_RX_WORD_LEN	0x28c
#define SE_UART_RX_STALE_CNT	0x294
#define SE_UART_TX_PARITY_CFG	0x2a4
#define SE_UART_RX_PARITY_CFG	0x2a8
#define SE_UART_MANUAL_RFR	0x2ac
#define SE_GENI_M_CMD0		0x600
#define SE_GENI_M_CMD_CTRL	0x604
#define SE_GENI_M_IRQ_STATUS	0x610
#define SE_GENI_M_IRQ_EN	0x614
#define SE_GENI_M_IRQ_CLEAR	0x618
#define SE_GENI_S_CMD0		0x630
#define SE_GENI_S_CMD_CTRL	0x634
#define SE_GENI_S_IRQ_STATUS	0x640
#define SE_GENI_S_IRQ_EN	0x644
#define SE_GENI_S_IRQ_CLEAR	0x648
#define SE_GENI_TX_FIFO		0x700
#define SE_GENI_RX_FIFO		0x780
#define SE_GENI_TX_FIFO_STATUS	0x800
#define SE_GENI_RX_FIFO_STATUS	0x804
#define SE_GENI_TX_WATERMARK	0x80c
#define SE_GENI_RX_WATERMARK	0x810
#define SE_GENI_RX_RFR_WATERMARK 0x814
#define SE_GENI_IOS		0x908
#define SE_DMA_TX_IRQ_CLR	0xc44
#define SE_DMA_RX_IRQ_CLR	0xd44
#define SE_GSI_EVENT_EN		0xe18
#define SE_IRQ_EN		0xe1c
#define SE_HW_PARAM_0		0xe24
#define SE_HW_PARAM_1		0xe28
#define SE_DMA_GENERAL_CFG	0xe30

#define M_GENI_CMD_ACTIVE	(1u << 0)
#define S_GENI_CMD_ACTIVE	(1u << 12)
#define GENI_CMD_CANCEL		(1u << 2)
#define GENI_CMD_ABORT		(1u << 1)
#define CMD_DONE		(1u << 0)
#define CMD_CANCEL		(1u << 4)
#define CMD_ABORT		(1u << 5)
#define RX_FIFO_WM		(1u << 26)
#define RX_FIFO_LAST		(1u << 27)
#define TX_FIFO_WM		(1u << 30)
#define M_COMMON_IRQ		(0x7eu | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define S_COMMON_IRQ		(0x3eu | (0x1fu << 9) | (1u << 24) | (1u << 25))
#define M_ERRORS		(0x0eu | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define S_ERRORS		(0x0eu | (1u << 24) | (1u << 25))
#define UART_START_TX		1u
#define UART_START_READ		1u
#define UART_CTS_MASK		(1u << 1)
#define UART_MANUAL_RFR_EN	(1u << 31)
#define UART_RFR_NOT_READY	(1u << 1)
#define RX_LAST			(1u << 31)
#define GENI_SE_UART		2u
#define QUP_SE_VERSION_2_5	0x20050000u

static unsigned tx_depth, rx_depth, sampling = 32;
static unsigned cur_baud;

static uint32_t se_rd(uint32_t o) { return rd32(SE_PA + o); }
static void se_wr(uint32_t o, uint32_t v) { wr32(SE_PA + o, v); }

static uint64_t ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int se_poll(uint32_t reg, uint32_t mask, int set, unsigned ms)
{
	uint64_t end = ms_now() + ms;
	for (;;) {
		if (!!(se_rd(reg) & mask) == set)
			return 0;
		if (ms_now() > end)
			return -1;
		usleep(50);
	}
}

/* ---- clocks ---- */

struct rate {
	uint32_t hz;
	int level;		/* DFS level (SE_GENI_CLK_SEL), or -1: program the RCG */
	uint8_t src, hid;	/* RCG CFG: source (0 XO, 1 GPLL0, 6 GPLL0 even), half-integer divider */
	uint16_t m, n;
};
static struct rate rates[16];
static int nrates, dfs;

static uint32_t parent_hz(unsigned src)
{
	return src == 0 ? 19200000u : src == 1 ? 600000000u : src == 6 ? 300000000u : 0;
}

static void clk_rates(void)
{
	uint64_t b = GCC_PA + GCC_RCG;
	nrates = 0;
	dfs = rd32(b + 0x14) & 1;
	if (dfs) {
		for (int l = 0; l < 8; l++) {
			uint32_t cfg = rd32(b + 0x1c + 4 * l);
			struct rate r = { 0, l, (cfg >> 8) & 7, cfg & 0x1f, 0, 0 };
			uint64_t hz = parent_hz(r.src);
			if (!hz)
				continue;
			if (r.hid)
				hz = hz * 2 / (r.hid + 1);
			if ((cfg >> 12) & 3) {
				r.m = rd32(b + 0x5c + 4 * l) & 0xffff;
				r.n = (uint16_t)((~rd32(b + 0x9c + 4 * l) & 0xffff) + r.m);
				if (r.n)
					hz = hz * r.m / r.n;
			}
			r.hz = (uint32_t)hz;
			rates[nrates++] = r;
		}
		return;
	}
	/* ftbl_gcc_qupv3_wrap0_s0_clk_src (F(rate, src, div, m, n): hid = 2 * div - 1) */
	static const struct rate tbl[] = {
		{ 7372800, -1, 6, 1, 384, 15625 }, { 14745600, -1, 6, 1, 768, 15625 }, { 19200000, -1, 0, 1, 0, 0 },
		{ 29491200, -1, 6, 1, 1536, 15625 }, { 32000000, -1, 6, 1, 8, 75 }, { 48000000, -1, 6, 1, 4, 25 },
		{ 64000000, -1, 6, 1, 16, 75 }, { 80000000, -1, 6, 1, 4, 15 }, { 96000000, -1, 6, 1, 8, 25 },
		{ 100000000, -1, 6, 5, 0, 0 }, { 102400000, -1, 6, 1, 128, 375 },
	};
	for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
		rates[nrates++] = tbl[i];
}

/* the rate and divider closest to want (within 2%), as find_clk_rate_in_tol */
static const struct rate *clk_pick(unsigned want, unsigned *divp)
{
	const struct rate *best = NULL;
	uint64_t besterr = ~0ull;
	for (int i = 0; i < nrates; i++) {
		unsigned div = (rates[i].hz + want / 2) / want;
		if (div < 1 || div > 0xfff)
			continue;
		uint64_t got = rates[i].hz / div;
		uint64_t err = got > want ? got - want : want - got;
		if (err * 50 > want)
			continue;
		if (err < besterr) {
			besterr = err;
			best = &rates[i];
			*divp = div;
		}
	}
	return best;
}

static int rcg_set(const struct rate *r)
{
	uint64_t b = GCC_PA + GCC_RCG;
	if (r->n) {
		wr32(b + 0x8, r->m);
		wr32(b + 0xc, ~(uint32_t)(r->n - r->m) & 0xffff);
		wr32(b + 0x10, ~(uint32_t)r->n & 0xffff);
	}
	uint32_t cfg = rd32(b + 4) & ~(0x371fu | (1u << 20));
	cfg |= r->hid | ((uint32_t)r->src << 8) | ((r->n && r->m != r->n) ? (2u << 12) : 0);
	wr32(b + 4, cfg);
	wr32(b, rd32(b) | 1);
	if (wait_bit(b, 1, 0, 100)) {
		printf("uart: RCG update timeout (cmd %#x)\n", rd32(b));
		return -1;
	}
	return 0;
}

static int clocks_on(void)
{
	wr32(GCC_PA + GCC_VOTE, rd32(GCC_PA + GCC_VOTE) | GCC_VOTE_BITS);
	for (unsigned i = 0; i < sizeof(gcc_branches) / sizeof(gcc_branches[0]); i++) {
		uint64_t a = GCC_PA + gcc_branches[i];
		if (rd32(a) & 2)	/* hardware-controlled */
			continue;
		if (wait_bit(a, 1u << 31, 0, 100)) {
			printf("uart: GCC branch %#x stays off (%#x)\n", gcc_branches[i], rd32(a));
			return -1;
		}
	}
	return 0;
}

/* ---- pins ---- */

/* TLMM GPIO_CFG: pull [1:0] (0 none, 1 down, 3 up), function [5:2], drive [8:6] = mA / 2 - 1, OE [9] */
static void pin(unsigned n, unsigned func, unsigned pull)
{
	uint64_t a = TLMM_PA + (uint64_t)n * 0x1000;
	uint32_t v = rd32(a) & ~0x3ffu;
	wr32(a, v | (func << 2) | pull);
}

static void pins_uart(void)
{
	pin(43, 1, 0);	/* CTS: qup13, bias-disable */
	pin(44, 1, 1);	/* RTS: qup13, pull-down */
	pin(45, 1, 3);	/* TX: qup13, pull-up */
	pin(46, 1, 0);	/* RX: qup13, bias-disable */
}

/* ---- GENI ---- */

static void packing(void)
{
	/* geni_se_config_packing(se, 8, 4, false, true, true) */
	uint32_t cfg[4];
	for (int i = 0; i < 4; i++)
		cfg[i] = ((uint32_t)i * 8 << 5) | (7u << 1);
	cfg[3] |= 1;	/* stop */
	uint32_t c0 = cfg[0] | (cfg[1] << 10), c1 = cfg[2] | (cfg[3] << 10);
	se_wr(SE_GENI_TX_PACKING_CFG0, c0);
	se_wr(SE_GENI_TX_PACKING_CFG1, c1);
	se_wr(SE_GENI_RX_PACKING_CFG0, c0);
	se_wr(SE_GENI_RX_PACKING_CFG1, c1);
}

static void rx_drain(void)
{
	for (int i = 0; i < 256 && (se_rd(SE_GENI_RX_FIFO_STATUS) & 0x1ffffff); i++)
		(void)se_rd(SE_GENI_RX_FIFO);
}

static void stop_rx(void)
{
	if (!(se_rd(SE_GENI_STATUS) & S_GENI_CMD_ACTIVE))
		return;
	se_wr(SE_GENI_S_CMD_CTRL, GENI_CMD_CANCEL);
	se_poll(SE_GENI_S_IRQ_STATUS, CMD_CANCEL, 1, 20);
	rx_drain();
	se_wr(SE_GENI_S_IRQ_CLEAR, se_rd(SE_GENI_S_IRQ_STATUS));
	if (se_rd(SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) {
		se_wr(SE_GENI_S_CMD_CTRL, GENI_CMD_ABORT);
		se_poll(SE_GENI_S_IRQ_STATUS, CMD_ABORT, 1, 20);
		se_wr(SE_GENI_S_IRQ_CLEAR, se_rd(SE_GENI_S_IRQ_STATUS));
		if (se_rd(SE_GENI_STATUS) & S_GENI_CMD_ACTIVE)
			printf("uart: RX will not stop (status %#x)\n", se_rd(SE_GENI_STATUS));
	}
}

static void start_rx(void)
{
	stop_rx();
	se_wr(SE_GENI_S_CMD0, UART_START_READ << 27);
	se_wr(SE_GENI_S_IRQ_EN, se_rd(SE_GENI_S_IRQ_EN) | RX_FIFO_WM | RX_FIFO_LAST);
	se_wr(SE_GENI_M_IRQ_EN, se_rd(SE_GENI_M_IRQ_EN) | RX_FIFO_WM | RX_FIFO_LAST);
}

static void stop_tx(void)
{
	if (!(se_rd(SE_GENI_STATUS) & M_GENI_CMD_ACTIVE))
		return;
	se_wr(SE_GENI_M_CMD_CTRL, GENI_CMD_CANCEL);
	if (se_poll(SE_GENI_M_IRQ_STATUS, CMD_CANCEL, 1, 20)) {
		se_wr(SE_GENI_M_CMD_CTRL, GENI_CMD_ABORT);
		se_poll(SE_GENI_M_IRQ_STATUS, CMD_ABORT, 1, 20);
	}
	se_wr(SE_GENI_M_IRQ_CLEAR, CMD_CANCEL | CMD_ABORT | CMD_DONE);
}

void uart_regs(void)
{
	pm_init();
	printf("gcc vote %#x rcg cmd %#x cfg %#x dfs %#x branches", rd32(GCC_PA + GCC_VOTE), rd32(GCC_PA + GCC_RCG),
	       rd32(GCC_PA + GCC_RCG + 4), rd32(GCC_PA + GCC_RCG + 0x14));
	for (unsigned i = 0; i < 3; i++)
		printf(" %#x=%#x", gcc_branches[i], rd32(GCC_PA + gcc_branches[i]));
	printf("\npins:");
	for (unsigned n = 43; n <= 46; n++)
		printf(" gpio%u cfg %#x io %#x", n, rd32(TLMM_PA + n * 0x1000ull), rd32(TLMM_PA + n * 0x1000ull + 4));
	printf("\n");
	if (clocks_on()) {
		printf("clocks did not come on: not reading the SE\n");
		return;
	}
	clk_rates();
	printf("rcg %s:", dfs ? "DFS levels" : "table");
	for (int i = 0; i < nrates; i++)
		printf(" %d:%u", rates[i].level, rates[i].hz);
	printf("\nqup hw %#x\n", rd32(WRAP_PA + 4));
	static const uint32_t regs[] = { SE_GENI_STATUS, GENI_SER_M_CLK_CFG, GENI_IF_DISABLE_RO, GENI_FW_REVISION_RO,
		SE_GENI_CLK_SEL, SE_GENI_DMA_MODE_EN, SE_UART_TX_TRANS_CFG, SE_UART_RX_TRANS_CFG, SE_UART_MANUAL_RFR,
		SE_GENI_M_IRQ_STATUS, SE_GENI_M_IRQ_EN, SE_GENI_S_IRQ_STATUS, SE_GENI_S_IRQ_EN, SE_GENI_TX_FIFO_STATUS,
		SE_GENI_RX_FIFO_STATUS, SE_GENI_IOS, SE_HW_PARAM_0, SE_HW_PARAM_1 };
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
		printf("se %#05x = %#x\n", regs[i], se_rd(regs[i]));
}

int uart_init(void)
{
	pm_init();
	if (clocks_on())
		return -1;
	clk_rates();
	uint32_t ver = rd32(WRAP_PA + 4);
	sampling = ver >= QUP_SE_VERSION_2_5 ? 16 : 32;
	uint32_t fw = se_rd(GENI_FW_REVISION_RO), p0 = se_rd(SE_HW_PARAM_0), p1 = se_rd(SE_HW_PARAM_1);
	printf("uart: qup hw %#x (sampling %u), se fw %#x proto %u, if-disable %#x, status %#x, hw params %#x %#x, "
	       "clock %s (%d rates)\n", ver, sampling, fw, (fw >> 8) & 0xff, se_rd(GENI_IF_DISABLE_RO),
	       se_rd(SE_GENI_STATUS), p0, p1, dfs ? "DFS" : "RCG table", nrates);
	if (((fw >> 8) & 0xff) != GENI_SE_UART) {
		printf("uart: SE13 has no UART firmware (protocol %u): stopping\n", (fw >> 8) & 0xff);
		return -1;
	}
	if (se_rd(GENI_IF_DISABLE_RO) & 1) {
		printf("uart: the FIFO interface is disabled (GSI only): stopping\n");
		return -1;
	}
	if (((p0 >> 24) & 0x3f) != 32 || ((p1 >> 24) & 0x3f) != 32) {
		printf("uart: FIFO width is not 32 bits: stopping\n");
		return -1;
	}
	tx_depth = (p0 >> 16) & 0x3f;
	rx_depth = (p1 >> 16) & 0x3f;
	if (tx_depth < 4 || rx_depth < 4) {
		printf("uart: FIFO depth %u/%u too small: stopping\n", tx_depth, rx_depth);
		return -1;
	}
	pins_uart();

	stop_tx();
	stop_rx();
	se_wr(SE_UART_RX_STALE_CNT, 10 * 16);
	packing();
	se_wr(SE_GENI_BYTE_GRAN, 0);
	/* geni_se_init(se, UART_RX_WM = 2, rx_depth - 2) */
	se_wr(SE_GSI_EVENT_EN, 0);
	se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	se_wr(SE_DMA_TX_IRQ_CLR, 0xffffffffu);
	se_wr(SE_DMA_RX_IRQ_CLR, 0xffffffffu);
	se_wr(SE_IRQ_EN, 0xffffffffu);
	se_wr(GENI_CGC_CTRL, se_rd(GENI_CGC_CTRL) | 0x7f);
	se_wr(SE_DMA_GENERAL_CFG, se_rd(SE_DMA_GENERAL_CFG) | 0xf);
	se_wr(GENI_OUTPUT_CTRL, 0x7f);
	se_wr(GENI_FORCE_DEFAULT, 1);
	se_wr(SE_IRQ_EN, se_rd(SE_IRQ_EN) | 0xf);
	se_wr(SE_GENI_DMA_MODE_EN, se_rd(SE_GENI_DMA_MODE_EN) & ~1u);
	se_wr(SE_GSI_EVENT_EN, 0);
	se_wr(SE_GENI_RX_WATERMARK, 2);
	se_wr(SE_GENI_RX_RFR_WATERMARK, rx_depth - 2);
	se_wr(SE_GENI_M_IRQ_EN, se_rd(SE_GENI_M_IRQ_EN) | M_COMMON_IRQ | CMD_DONE);
	se_wr(SE_GENI_S_IRQ_EN, se_rd(SE_GENI_S_IRQ_EN) | S_COMMON_IRQ);
	/* 8N1, no loopback */
	se_wr(SE_UART_LOOPBACK_CFG, 0);
	se_wr(SE_UART_TX_TRANS_CFG, (se_rd(SE_UART_TX_TRANS_CFG) & ~1u) | UART_CTS_MASK);
	se_wr(SE_UART_RX_TRANS_CFG, se_rd(SE_UART_RX_TRANS_CFG) & ~(1u << 3));
	se_wr(SE_UART_TX_PARITY_CFG, se_rd(SE_UART_TX_PARITY_CFG) & ~1u);
	se_wr(SE_UART_RX_PARITY_CFG, se_rd(SE_UART_RX_PARITY_CFG) & ~1u);
	se_wr(SE_UART_TX_WORD_LEN, 8);
	se_wr(SE_UART_RX_WORD_LEN, 8);
	se_wr(SE_UART_TX_STOP_BIT_LEN, 0);
	se_wr(SE_UART_MANUAL_RFR, UART_MANUAL_RFR_EN | UART_RFR_NOT_READY);
	if (uart_set_baud(115200))
		return -1;
	printf("uart: ready, FIFO %u/%u words\n", tx_depth, rx_depth);
	return 0;
}

int uart_baud_ok(unsigned baud)
{
	unsigned div;
	return clk_pick(baud * sampling, &div) != NULL;
}

int uart_set_baud(unsigned baud)
{
	unsigned div = 0;
	const struct rate *r = clk_pick(baud * sampling, &div);
	if (!r) {
		printf("uart: no clock for %u baud\n", baud);
		return -1;
	}
	if (se_poll(SE_GENI_STATUS, M_GENI_CMD_ACTIVE, 0, 200))
		stop_tx();
	stop_rx();
	if (r->level < 0) {
		if (rcg_set(r))
			return -1;
		se_wr(SE_GENI_CLK_SEL, 0);
	} else {
		se_wr(SE_GENI_CLK_SEL, (uint32_t)r->level);
	}
	se_wr(GENI_SER_M_CLK_CFG, (div << 4) | 1);
	se_wr(GENI_SER_S_CLK_CFG, (div << 4) | 1);
	start_rx();
	if (baud != cur_baud)
		printf("uart: %u baud (%u Hz / %u, sampling %u)\n", baud, r->hz, div, sampling);
	cur_baud = baud;
	return 0;
}

void uart_flow(int on)
{
	uint32_t t = se_rd(SE_UART_TX_TRANS_CFG);
	if (on) {
		se_wr(SE_UART_MANUAL_RFR, 0);
		t &= ~UART_CTS_MASK;
	} else {
		se_wr(SE_UART_MANUAL_RFR, UART_MANUAL_RFR_EN | UART_RFR_NOT_READY);
		t |= UART_CTS_MASK;
	}
	se_wr(SE_UART_TX_TRANS_CFG, t);
}

int uart_write(const void *buf, size_t len, unsigned timeout_ms)
{
	const uint8_t *p = buf;
	uint64_t end = ms_now() + timeout_ms;
	if (!len)
		return 0;
	if (se_poll(SE_GENI_STATUS, M_GENI_CMD_ACTIVE, 0, timeout_ms)) {
		printf("uart: TX still busy\n");
		stop_tx();
	}
	se_wr(SE_GENI_M_IRQ_CLEAR, CMD_DONE | CMD_CANCEL | CMD_ABORT | TX_FIFO_WM | M_ERRORS);
	se_wr(SE_UART_TX_TRANS_LEN, (uint32_t)len);
	se_wr(SE_GENI_M_CMD0, UART_START_TX << 27);
	size_t i = 0;
	while (i < len) {
		if ((se_rd(SE_GENI_TX_FIFO_STATUS) & 0xfffffff) >= tx_depth) {
			if (ms_now() > end)
				goto timeout;
			usleep(20);
			continue;
		}
		uint32_t w = 0;
		memcpy(&w, p + i, len - i < 4 ? len - i : 4);
		se_wr(SE_GENI_TX_FIFO, w);
		i += 4;
	}
	for (;;) {
		uint32_t st = se_rd(SE_GENI_M_IRQ_STATUS);
		if (st & CMD_DONE) {
			se_wr(SE_GENI_M_IRQ_CLEAR, CMD_DONE | TX_FIFO_WM);
			return 0;
		}
		if (ms_now() > end)
			goto timeout;
		usleep(20);
	}
timeout:
	printf("uart: TX timeout after %zu of %zu bytes (m irq %#x, tx fifo %#x, ios %#x)\n", i, len,
	       se_rd(SE_GENI_M_IRQ_STATUS), se_rd(SE_GENI_TX_FIFO_STATUS), se_rd(SE_GENI_IOS));
	stop_tx();
	return -1;
}

size_t uart_read(uint8_t *buf, size_t max)
{
	static unsigned errs;
	uint32_t s = se_rd(SE_GENI_S_IRQ_STATUS), m = se_rd(SE_GENI_M_IRQ_STATUS);
	if (((s & S_ERRORS) || (m & (M_ERRORS & ~0x0eu))) && errs++ < 10)
		printf("uart: RX irq s %#x m %#x\n", s, m);
	uint32_t st = se_rd(SE_GENI_RX_FIFO_STATUS);
	uint32_t wc = st & 0x1ffffff, last = (st >> 28) & 7;
	size_t n = 0;
	for (uint32_t i = 0; i < wc && n + 4 <= max; i++) {
		uint32_t w = se_rd(SE_GENI_RX_FIFO);
		unsigned nb = (i == wc - 1 && (st & RX_LAST) && last) ? last : 4;
		memcpy(buf + n, &w, nb);
		n += nb;
	}
	if (s & (RX_FIFO_WM | RX_FIFO_LAST | S_ERRORS))
		se_wr(SE_GENI_S_IRQ_CLEAR, s & (RX_FIFO_WM | RX_FIFO_LAST | S_ERRORS));
	if (m & (RX_FIFO_WM | RX_FIFO_LAST | (M_ERRORS & ~0x0eu)))
		se_wr(SE_GENI_M_IRQ_CLEAR, m & (RX_FIFO_WM | RX_FIFO_LAST | (M_ERRORS & ~0x0eu)));
	return n;
}

void uart_flush_rx(void)
{
	uint8_t b[256];
	while (uart_read(b, sizeof(b)))
		;
}
