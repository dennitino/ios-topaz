/*
 * touchlab: bare-metal test bench for the topaz touch controller, run from the
 * shim before xnu (MMU off, physical addresses, ABL's display still running).
 *
 * Same hardware path as bsd/dev/sweet_touch.c (GCC vote for QUP0 SE2, GENI I2C
 * FIFO mode at 400 kHz, TLMM pins 6/7/80/86/36), but polled with a live screen:
 * chip id and firmware, registers 0x80..0xAF, then every frame the controller
 * returns while the user touches the screen, in three phases:
 *   A  power-up like Android (reset low, AVDD high, reset high, 200 ms)
 *   B  same, plus work mode / active power mode / no monitor-mode writes
 *   C  reads starting at register 0x01 like Android's driver
 * Answers whether this panel's controller reports touches at all with the
 * display as ABL left it (before xnu touches the display).
 */
#include <stdint.h>
#include "console.h"
#include "touchlab.h"

#define SE_PA		0x04a88000UL	/* QUP0 SE2 */
#define GCC_PA		0x01400000UL
#define WEST_PA		0x00500000UL	/* TLMM WEST tile: GPIO 6/7/80/86 */
#define EAST_PA		0x00d00000UL	/* TLMM EAST tile: GPIO 36 */
#define GCC_VOTE	0x7900cU
#define GCC_RCG		0x1f3a8U
#define PIN_RESET	86U
#define PIN_IRQ		80U
#define PIN_AVDD	36U
#define GPIO_CFG(n)	((uint32_t)(n) * 0x1000U)
#define GPIO_IO(n)	(GPIO_CFG(n) + 4U)
#define FT_ADDR		0x38

#define SE_STATUS	0x040U
#define SE_MCLK		0x048U
#define SE_FW		0x068U
#define SE_CLK_SEL	0x07cU
#define SE_DMA		0x258U
#define SE_TX_LEN	0x26cU
#define SE_RX_LEN	0x270U
#define SE_SCL		0x278U
#define SE_CMD		0x600U
#define SE_CTRL		0x604U
#define SE_IRQ		0x610U
#define SE_IRQ_EN	0x614U
#define SE_IRQ_CLR	0x618U
#define SE_TX		0x700U
#define SE_RX		0x780U
#define SE_TX_COUNT	0x800U
#define SE_RX_COUNT	0x804U
#define SE_TX_WM	0x80cU
#define SE_HW_TX	0xe24U
#define SE_HW_RX	0xe28U
#define SE_RX_WM_IRQ	(1U << 26)
#define SE_RX_LAST	(1U << 27)
#define SE_TX_WM_IRQ	(1U << 30)
#define SE_DONE		(1U << 0)
#define SE_NACK		(1U << 10)
#define SE_ERRORS	(0x3eU | (1U << 9) | (1U << 10) | (1U << 12) | \
			 (1U << 13) | (3U << 24) | (3U << 28))
#define SE_EVENTS	(SE_DONE | SE_ERRORS | SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ)
#define SE_PACK0	0x0007f8feU
#define SE_PACK1	0x000ffefeU

#define E_IO		5
#define E_NXIO		6
#define E_BUSY		16
#define E_TIMEDOUT	60

static uint32_t sent, got, irq_seen;

static inline uint32_t rd(uintptr_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void wr(uintptr_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void delay_us(unsigned us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;	/* 19.2 MHz */
	while (ticks() < end)
		;
}

static int wait_clear(uintptr_t base, uint32_t off, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; i++) {
		if (!(rd(base, off) & mask))
			return 1;
		delay_us(1);
	}
	return 0;
}

/* qcom TLMM CTL: pull[1:0] func[5:2] drv[8:6] oe[9]; IO: in[0] out[1] */
static void gpio(unsigned pin, unsigned mux, unsigned pull, unsigned drive, int out, int high)
{
	uintptr_t tile = pin == PIN_AVDD ? EAST_PA : WEST_PA;
	uint32_t cfg = rd(tile, GPIO_CFG(pin));
	if (out)
		wr(tile, GPIO_IO(pin), (rd(tile, GPIO_IO(pin)) & ~2U) | (high ? 2U : 0U));
	cfg &= ~0x3ffU;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200U : 0U);
	wr(tile, GPIO_CFG(pin), cfg);
}

static int irq_pin(void)
{
	return rd(WEST_PA, GPIO_IO(PIN_IRQ)) & 1U;
}

static int clocks(void)
{
	wr(GCC_PA, GCC_VOTE, rd(GCC_PA, GCC_VOTE) | 0x13c0U);
	wr(GCC_PA, GCC_RCG + 4U, 1U);
	wr(GCC_PA, GCC_RCG, rd(GCC_PA, GCC_RCG) | 1U);
	if (!wait_clear(GCC_PA, GCC_RCG, 1U))
		return 0;
	return wait_clear(GCC_PA, GCC_RCG - 4U, 1U << 31);
}

static int se_init(void)
{
	uint32_t fw = rd(SE_PA, SE_FW);
	uint32_t tx = rd(SE_PA, SE_HW_TX), rx = rd(SE_PA, SE_HW_RX);
	con_hex2("SE2", "fw=", fw, "status=", rd(SE_PA, SE_STATUS));
	if (((fw >> 8) & 0xffU) != 3U || (rd(SE_PA, 0x64) & 1U) ||
	    ((tx >> 24) & 0x3fU) != 32U || ((rx >> 24) & 0x3fU) != 32U ||
	    ((tx >> 16) & 0x3fU) < 2U || ((rx >> 16) & 0x3fU) < 2U)
		return 0;
	if (rd(SE_PA, SE_STATUS) & 0x1001U)
		return 0;
	wr(SE_PA, 0xe18, 0);
	wr(SE_PA, 0xe1c, 0);
	wr(SE_PA, 0x644, 0);
	wr(SE_PA, SE_IRQ_CLR, 0xffffffffU);
	wr(SE_PA, 0x648, 0xffffffffU);
	wr(SE_PA, SE_DMA, 0);
	wr(SE_PA, 0x28, rd(SE_PA, 0x28) | 0x7fU);
	wr(SE_PA, 0xe30, rd(SE_PA, 0xe30) | 0x0fU);
	wr(SE_PA, 0x24, 0x7f);
	wr(SE_PA, 0x20, 1);
	wr(SE_PA, SE_CLK_SEL, 0);
	wr(SE_PA, SE_MCLK, (2U << 4) | 1U);
	wr(SE_PA, SE_SCL, (5U << 20) | (11U << 10) | 22U);	/* 400 kHz */
	wr(SE_PA, 0x254, 0);
	wr(SE_PA, 0x260, SE_PACK0);
	wr(SE_PA, 0x264, SE_PACK1);
	wr(SE_PA, 0x284, SE_PACK0);
	wr(SE_PA, 0x288, SE_PACK1);
	wr(SE_PA, SE_TX_WM, 0);
	wr(SE_PA, 0x810, 0);
	wr(SE_PA, 0x814, ((rx >> 16) & 0x3fU) - 1U);
	wr(SE_PA, SE_IRQ_EN, SE_EVENTS);
	return !(rd(SE_PA, SE_TX_COUNT) & 0xfffffffU) && !(rd(SE_PA, SE_RX_COUNT) & 0x1ffffffU);
}

static void se_abort(void)
{
	int done = 0;
	wr(SE_PA, SE_CTRL, 4);
	for (unsigned i = 0; i < 1000 && !done; i++) {
		done = (rd(SE_PA, SE_IRQ) & (1U << 4)) != 0;
		delay_us(1);
	}
	if (!done) {
		wr(SE_PA, SE_CTRL, 2);
		for (unsigned i = 0; i < 1000 && !(rd(SE_PA, SE_IRQ) & (1U << 5)); i++)
			delay_us(1);
	}
	(void)wait_clear(SE_PA, SE_STATUS, 1U);
	for (unsigned i = 0; i < 64 && (rd(SE_PA, SE_RX_COUNT) & 0x1ffffffU); i++)
		(void)rd(SE_PA, SE_RX);
	wr(SE_PA, SE_IRQ_CLR, 0xffffffffU);
}

/* One FIFO message: TX at most 3 bytes, RX at most 90 (sweet_touch.c). */
static int msg(uint8_t addr, const uint8_t *txb, unsigned nt, uint8_t *rxb, unsigned nr, int stretch)
{
	sent = got = 0;
	if (nt > 3 || nr > 90 || (nt && nr) || (!nt && !nr))
		return E_IO;
	if (!wait_clear(SE_PA, SE_STATUS, 1U))
		return E_BUSY;
	wr(SE_PA, SE_IRQ_CLR, 0xffffffffU);
	wr(SE_PA, SE_TX_WM, nt ? 1U : 0U);
	wr(SE_PA, nr ? SE_RX_LEN : SE_TX_LEN, nr ? nr : nt);
	wr(SE_PA, SE_CMD, ((nr ? 2U : 1U) << 27) | ((uint32_t)addr << 9) | (stretch ? 4U : 0U));
	int err = E_TIMEDOUT;
	for (unsigned i = 0; i < 20000; i++) {
		uint32_t irq = rd(SE_PA, SE_IRQ);
		irq_seen |= irq;
		if (irq & SE_ERRORS) {
			err = (irq & SE_NACK) ? E_NXIO : E_IO;
			for (unsigned j = 0; j < 5000 && !(irq & SE_DONE); j++) {
				delay_us(1);
				irq = rd(SE_PA, SE_IRQ);
			}
			if ((irq & SE_DONE) && !(rd(SE_PA, SE_STATUS) & 1U) &&
			    !(rd(SE_PA, SE_TX_COUNT) & 0xfffffffU) &&
			    !(rd(SE_PA, SE_RX_COUNT) & 0x1ffffffU)) {
				wr(SE_PA, SE_IRQ_CLR, irq);
				return err;
			}
			break;
		}
		if (nt && !sent && (irq & SE_TX_WM_IRQ)) {
			uint32_t word = 0;
			for (unsigned j = 0; j < nt; j++)
				word |= (uint32_t)txb[j] << (8U * j);
			wr(SE_PA, SE_TX, word);
			sent = nt;
			wr(SE_PA, SE_TX_WM, 0);
		}
		uint32_t count = rd(SE_PA, SE_RX_COUNT) & 0x1ffffffU;
		if (count > 64 || count > (nr - got + 3U) / 4U) {
			err = E_IO;
			break;
		}
		while (count--) {
			uint32_t word = rd(SE_PA, SE_RX);
			for (unsigned j = 0; j < 4 && got < nr; j++)
				rxb[got++] = (uint8_t)(word >> (8U * j));
		}
		if (irq & SE_DONE) {
			irq = rd(SE_PA, SE_IRQ);
			if (irq & SE_ERRORS)
				continue;
			wr(SE_PA, SE_IRQ_CLR, irq);
			if (got == nr && sent == nt)
				return 0;
			err = E_IO;
			break;
		}
		wr(SE_PA, SE_IRQ_CLR, irq & (SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ));
		delay_us(1);
	}
	se_abort();
	return err;
}

static int ft_read(uint8_t reg, uint8_t *buf, unsigned len)
{
	int err = msg(FT_ADDR, &reg, 1, 0, 0, 1);
	if (err)
		return err;
	return msg(FT_ADDR, 0, 0, buf, len, 0);
}

static int ft_write(uint8_t reg, uint8_t val)
{
	uint8_t b[2];
	b[0] = reg;
	b[1] = val;
	return msg(FT_ADDR, b, 2, 0, 0, 0);
}

/* ---- screen helpers ---- */

static char hexd(unsigned v)
{
	v &= 15;
	return (char)(v < 10 ? '0' + v : 'A' + v - 10);
}

/* "xx xx xx ..." for n bytes (n <= 12) */
static void bytes_str(char *out, const uint8_t *b, unsigned n)
{
	unsigned k = 0;
	for (unsigned i = 0; i < n; i++) {
		out[k++] = hexd(b[i] >> 4);
		out[k++] = hexd(b[i]);
		out[k++] = ' ';
	}
	out[k ? k - 1 : 0] = 0;
}

static void show_regs(void)
{
	uint8_t v[12];
	char line[40];
	static const uint8_t bases[4] = { 0x80, 0x8c, 0xa0, 0xac };
	for (unsigned k = 0; k < 4; k++) {
		for (unsigned i = 0; i < 12; i++) {
			v[i] = 0xee;
			(void)ft_read((uint8_t)(bases[k] + i), &v[i], 1);
		}
		bytes_str(line, v, 12);
		char label[4];
		label[0] = hexd(bases[k] >> 4);
		label[1] = hexd(bases[k]);
		label[2] = ':';
		label[3] = 0;
		con_str(label, line);
	}
}

static void ident(const char *label)
{
	uint8_t id1 = 0, id2 = 0, fw = 0, mode = 0xee, pm = 0xee;
	int e1 = ft_read(0xa3, &id1, 1);
	int e2 = ft_read(0x9f, &id2, 1);
	(void)ft_read(0xa6, &fw, 1);
	(void)ft_read(0x00, &mode, 1);
	(void)ft_read(0xa5, &pm, 1);
	con_hex2(label, "id=", (uint32_t)id1 << 8 | id2, "err=", (uint32_t)(e1 << 8 | e2));
	con_hex2("", "fw=", fw, "mode/pm=", (uint32_t)mode << 8 | pm);
}

/*
 * Poll for `secs` seconds. Shows, updated in place: reads, errors, IRQ-low
 * samples, frames with a touch count, the last frame and the last frame that
 * had any non-zero byte. start = first register (0x00 like sweet_touch.c,
 * 0x01 like Android's driver).
 */
static void poll(const char *phase, uint8_t start, unsigned secs)
{
	uint8_t buf[63], last_nz[12] = { 0 };
	uint32_t reads = 0, errs = 0, lowirq = 0, samples = 0, touched = 0, nz = 0, maxn = 0;
	int lasterr = 0;
	char line[40];
	int row0 = con_get_row();
	unsigned len = start ? 62 : 63;
	unsigned td = start ? 1 : 2;	/* TD_STATUS index in buf */
	uint64_t end = ticks() + (uint64_t)secs * 19200000, next_draw = 0;

	while (ticks() < end) {
		samples++;
		if (!irq_pin())
			lowirq++;
		for (unsigned i = 0; i < len; i++)
			buf[i] = 0xee;
		int err = ft_read(start, buf, len);
		reads++;
		if (err) {
			errs++;
			lasterr = err;
		} else {
			unsigned n = buf[td] & 15U;
			if (n && n <= 10) {
				touched++;
				if (n > maxn)
					maxn = n;
			}
			int any = 0;
			for (unsigned i = 0; i < len; i++)
				if (buf[i] && buf[i] != 0xff)
					any = 1;
			if (any) {
				nz++;
				for (unsigned i = 0; i < 12; i++)
					last_nz[i] = buf[i];
			}
		}
		if (ticks() >= next_draw) {
			uint32_t left = (uint32_t)((end - ticks()) / 19200000);
			con_set_row(row0);
			con_hex2(phase, "reg=", start, "seg=", left);
			con_hex2("LEIT", "total=", reads, "erros=", errs);
			con_hex2("", "ultimo=", (uint32_t)lasterr, "irqlo=", lowirq);
			con_hex2("TOQUE", "quadros=", touched, "maxdedos=", maxn);
			con_hex2("", "nao-zero=", nz, "amostras=", samples);
			bytes_str(line, buf, 12);
			con_str("ULTIMO", line);
			bytes_str(line, last_nz, 12);
			con_str("NAOZERO", line);
			con_str("", touched ? "TOQUE DETECTADO!" : "toque na tela...");
			next_draw = ticks() + 19200000 / 4;
		}
		delay_us(10000);
	}
	con_skip();
}

void touchlab_run(void)
{
	con_clear(BLUE);
	con_str("LAB", "touchlab: toque detectado?");
	con_str("", "toque e arraste a tela o tempo todo");
	con_skip();
	gpio(PIN_IRQ, 0, 3, 3, 0, 0);
	gpio(6, 1, 0, 0, 0, 0);
	gpio(7, 1, 0, 0, 0, 0);
	con_hex2("PINOS", "reset=", rd(WEST_PA, GPIO_IO(PIN_RESET)), "avdd=", rd(EAST_PA, GPIO_IO(PIN_AVDD)));
	if (!clocks()) {
		con_str("ERRO", "clock do SE2");
		return;
	}
	if (!se_init()) {
		con_str("ERRO", "SE2 I2C init");
		return;
	}
	ident("SEM RST");

	/* A: Android order (fts_power_source_ctrl + fts_reset_proc) */
	gpio(PIN_RESET, 0, 3, 3, 1, 0);
	delay_us(1000);
	gpio(PIN_AVDD, 0, 3, 3, 1, 1);
	delay_us(5000);
	gpio(PIN_RESET, 0, 3, 3, 1, 0);
	delay_us(1000);
	gpio(PIN_RESET, 0, 3, 3, 1, 1);
	delay_us(200000);
	for (unsigned i = 0; i < 20; i++) {	/* fts_wait_tp_to_valid */
		uint8_t id = 0;
		if (!ft_read(0xa3, &id, 1) && id == 0x54)
			break;
		delay_us(10000);
	}
	ident("APOS RST");
	show_regs();
	con_skip();
	poll("FASE A", 0x00, 25);

	/* B: work mode, active power mode, no auto monitor mode */
	con_hex2("ESCRITA", "00:", (uint32_t)ft_write(0x00, 0x00), "a5:", (uint32_t)ft_write(0xa5, 0x00));
	con_hex2("", "86:", (uint32_t)ft_write(0x86, 0x00), "8b:", (uint32_t)ft_write(0x8b, 0x01));
	poll("FASE B", 0x00, 20);

	/* C: Android's read (register 0x01) */
	poll("FASE C", 0x01, 20);

	con_str("LAB", "fim. Power+Vol- para sair");
}
