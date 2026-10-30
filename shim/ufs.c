/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 0. Nothing is ever
 * written to the device: there is no SCSI WRITE in this file.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x04804000UL	/* ufshc@4804000, topaz live FDT */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {	/* 1 s */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
		    !__builtin_strcmp(name, "cache"))
			con_str(" fs", fs_type((uint32_t)first));
	}
}

void test_ufs(void)
{
	con_str("UFS", "milestone 5 probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}

	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(0, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc)
		return;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}

/*
 * LOGVIEW (SHIM_CFLAGS=-DLOGVIEW): show what the last iOS boot wrote to the
 * console, from the persistent log of kernel patch 0047 (the last 1025 blocks
 * of "userdata": a header block, then a 4 MiB ring). Only the lines with one
 * of the words below, the newest ones last, a page every 20 s, forever.
 * READ-ONLY like the rest of this file: no SCSI WRITE is ever issued.
 */
#define LOG_RING	(4u << 20)
#define LOG_BLOCKS	(1u + LOG_RING / BLOCK)
#define LV_COLS		64
#define LV_ROWS		70
#define LV_MAX		1000
#define LV_LINE		(3 * LV_COLS)
#define LV_CHUNK	16u		/* blocks read at a time */

static uint8_t lv_chunk[LV_CHUNK * BLOCK] __attribute__((aligned(4096)));
static char lv_text[LV_MAX][LV_LINE];
static uint32_t lv_len[LV_MAX];

/* the words, separated by '|' (one string: no pointer table, the shim has no relocations) */
static const char lv_words[] = "join|wpa:|link|configd|ASSOCIATE|dropped:|keys installed|auth|assoc|boot-args|DHCP|ipconfig|panic|station says|RedwoodWLAN|IONetwork|en0";

static int lv_has_n(const uint8_t *line, uint32_t len, const char *w, uint32_t wl)
{
	for (uint32_t i = 0; i + wl <= len; i++) {
		uint32_t k = 0;
		while (k < wl && line[i + k] == (uint8_t)w[k])
			k++;
		if (k == wl)
			return 1;
	}
	return 0;
}

static int lv_has(const uint8_t *line, uint32_t len, const char *w)
{
	uint32_t wl = 0;
	while (w[wl])
		wl++;
	return lv_has_n(line, len, w, wl);
}

static int lv_wanted(const uint8_t *line, uint32_t len)
{
	const char *w = lv_words;
	while (*w) {
		uint32_t wl = 0;
		while (w[wl] && w[wl] != '|')
			wl++;
		if (wl && lv_has_n(line, len, w, wl))
			return 1;
		w += wl;
		if (*w == '|')
			w++;
	}
	return 0;
}

static void lv_wait_s(unsigned s)
{
	uint64_t t = ticks();
	while (ticks() - t < (uint64_t)s * 19200000u)
		;
}

/* The UFS host as ABL left it, our transfer list in place. 0: ready. */
static int lv_take_host(void)
{
	uint32_t hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY) || ufs_rd(REG_UTRLDBR))
		return 1;
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1))
		return 2;
	uint32_t ocs, st;
	static const uint8_t tur[6] = { 0x00 };
	for (int i = 0; i < 3; i++)
		if (!scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st))
			return 0;
	return 3;
}

/* userdata's first block and number of blocks, from the GPT. 0: found. */
static int lv_userdata(uint64_t *first, uint64_t *count)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	if (read_blocks(0, 2, data))
		return 1;
	const uint8_t *gpt = data + BLOCK;
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	if (gpt[0] != 'E' || gpt[1] != 'F' || gpt[2] != 'I' || esz != 128 || nent > 128)
		return 2;
	if (read_blocks(elba, (nent * 128 + BLOCK - 1) / BLOCK, ents))
		return 3;
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		static const char want[] = "userdata";
		int same = 1;
		for (int c = 0; c < 9; c++)
			if (e[56 + 2 * c] != (uint8_t)want[c] || e[57 + 2 * c] != 0)
				same = 0;
		if (same) {
			*first = le64(e + 32);
			*count = le64(e + 40) - *first + 1;
			return 0;
		}
	}
	return 4;
}

void logview_run(void)
{
	uint64_t first = 0, count = 0;
	int rc;

	con_skip();
	con_str("LOGVIEW", "log of the last iOS boot");
	if ((rc = lv_take_host()) != 0) {
		con_hex2("LOGVIEW", "no UFS host, rc=", (uint32_t)rc, 0, 0);
		return;
	}
	if ((rc = lv_userdata(&first, &count)) != 0 || count < 2 * LOG_BLOCKS) {
		con_hex2("LOGVIEW", "no userdata, rc=", (uint32_t)rc, 0, 0);
		return;
	}
	uint64_t base = first + count - LOG_BLOCKS;
	if (read_blocks((uint32_t)base, 1, data)) {
		con_str("LOGVIEW", "header read error");
		return;
	}
	static const char magic[] = "SWEETLOG";
	for (int i = 0; i < 8; i++)
		if (data[i] != (uint8_t)magic[i]) {
			con_str("LOGVIEW", "no log there (boot iOS once first)");
			return;
		}
	uint32_t size = data[12] | data[13] << 8 | data[14] << 16 | (uint32_t)data[15] << 24;
	uint64_t total = le64(data + 16);
	if (size != LOG_RING) {
		con_hex2("LOGVIEW", "odd ring size=", size, 0, 0);
		return;
	}
	con_hex2("LOGVIEW", "bytes=", (uint32_t)total, "MiB in=", (uint32_t)(count >> 8));

	/* the lines kept: [from, total) of the byte stream, ring offset n % size, read a chunk at a time */
	uint64_t from = total > LOG_RING ? total - LOG_RING : 0;
	static uint8_t line[512];
	uint32_t n = 0, kept = 0, loaded = ~0u;
	for (uint64_t p = from; p < total; p++) {
		uint32_t off = (uint32_t)(p % LOG_RING), chunk = off / (LV_CHUNK * BLOCK);
		if (chunk != loaded) {
			if (read_blocks((uint32_t)(base + 1 + (uint64_t)chunk * LV_CHUNK), LV_CHUNK, lv_chunk)) {
				con_hex2("LOGVIEW", "read error at=", chunk, 0, 0);
				return;
			}
			loaded = chunk;
		}
		uint8_t ch = lv_chunk[off % (LV_CHUNK * BLOCK)];
		if (ch != '\n') {
			if (n < sizeof(line))
				line[n++] = ch;
			continue;
		}
		int want = n && lv_wanted(line, n);
		/* wifid's own questions are too many to read: only configd's and the station's */
		if (want && lv_has(line, n, "wifid gets"))
			want = 0;
		if (want) {
			uint32_t slot = kept % LV_MAX, m = n > LV_LINE ? LV_LINE : n;
			for (uint32_t c = 0; c < m; c++)
				lv_text[slot][c] = (char)line[c];
			lv_len[slot] = m;
			kept++;
		}
		n = 0;
	}

	/* rows of 64 characters, a long line on up to 3 rows; newest page last */
	uint32_t oldest = kept > LV_MAX ? kept - LV_MAX : 0;
	for (;;) {
		/* pages, counted back from the end */
		uint32_t starts[64], pages = 0, i = kept;
		while (i > oldest && pages < 64) {
			uint32_t rows = 1;  /* the page title */
			while (i > oldest) {
				uint32_t len = lv_len[(i - 1) % LV_MAX], r = (len + LV_COLS - 1) / LV_COLS;
				r = r > 3 ? 3 : r ? r : 1;
				if (rows + r > LV_ROWS)
					break;
				rows += r;
				i--;
			}
			starts[pages++] = i;
		}
		if (!pages) {
			con_str("LOGVIEW", "no Wi-Fi lines in the log");
			return;
		}
		for (uint32_t pg = pages; pg-- > 0;) {
			uint32_t s = starts[pg], e = pg + 1 < pages + 1 && pg > 0 ? starts[pg - 1] : kept;
			con_clear(0xff000000U);
			char title[LV_COLS];
			static const char t[] = "LOGVIEW page   /   (last boot, Wi-Fi lines)";
			int tl = 0;
			while (t[tl]) {
				title[tl] = t[tl];
				tl++;
			}
			uint32_t shown = pages - pg;
			title[13] = (char)('0' + shown / 10);
			title[14] = (char)('0' + shown % 10);
			title[16] = (char)('0' + pages / 10);
			title[17] = (char)('0' + pages % 10);
			con_small(24, title, tl, YELLOW, 0xff000000U);
			int y = 24 + 32;
			for (uint32_t k = s; k < e; k++) {
				const char *txt = lv_text[k % LV_MAX];
				uint32_t len = lv_len[k % LV_MAX];
				for (uint32_t r = 0; r < 3 && r * LV_COLS < len; r++) {
					uint32_t m = len - r * LV_COLS;
					m = m > LV_COLS ? LV_COLS : m;
					con_small(y, txt + r * LV_COLS, (int)m, r ? 0xffc0c0c0U : WHITE, 0xff000000U);
					y += 32;
				}
			}
			lv_wait_s(20);
		}
	}
}
