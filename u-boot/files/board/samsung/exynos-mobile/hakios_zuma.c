// SPDX-License-Identifier: GPL-2.0+
/*
 * HakiOS on Google's zuma phones (Tensor G3: the Pixel 8, 8 Pro, 8a), in U-Boot's generic Exynos phone board.
 *
 * Google's bootloader starts U-Boot from RAM (fastboot boot) and leaves behind what this file deals with:
 *
 *   hk_load_base    where the bootloader loaded U-Boot, noted by dram_init() before U-Boot relocates itself: the boot
 *                   image carries the HakiOS container behind U-Boot (tools/mkubootimg.py, `container kernel`), and
 *                   `hakios load` (cmd/hakios.c) looks for it there.
 *
 *   zuma wdt        the two cluster watchdogs (watchdog_cl0@10060000, watchdog_cl1@10070000 in Android's tree), which
 *                   the bootloader leaves counting with their reset on: the Pixel 8a restarted about two minutes after
 *                   every hand-over. Both are stopped; the one named in $hakios_wdt_net is then set going again as
 *                   slowly as it can count, a net for a hang (about 2.5 minutes on the 8a). The HakiOS loader knows it
 *                   (xnuloader/loader.c: wdt_samsung) and the kernel feeds it (/chosen/watchdog-samsung).
 *
 *   pstore          a console device writing into a zone of Android's ramoops region ($hakios_pstore: address and
 *                   size; ramoops_mem@fd3ff000 on zuma: console 2 MiB, then pmsg 2 MiB), in the layout of Linux's
 *                   persistent RAM (fs/pstore/ram_core.c: "DBGC", start, size, then the text, no ECC), appended to the
 *                   log already there as Linux's console would. On zuma the console zone: Google's bootloader keeps
 *                   its own log there and, at the next start, what the previous kernel wrote (the pmsg zone came back
 *                   empty on the Pixel 8). After the restart, as root in Android: cat /sys/fs/pstore/console-ramoops-0.
 *                   What U-Boot printed before the device was there is copied in from the console record.
 *
 * Samsung watchdog registers (Linux drivers/watchdog/s3c2410_wdt.c): WTCON +0 (bits 15:8 the prescaler, 5 enable,
 * 4:3 the clock divider 16/32/64/128, 2 interrupt, 0 reset), WTDAT +4 (the reload value), WTCNT +8 (the count).
 */

#include <command.h>
#include <console.h>
#include <cpu_func.h>
#include <env.h>
#include <event.h>
#include <membuf.h>
#include <stdio_dev.h>
#include <vsprintf.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/string.h>

DECLARE_GLOBAL_DATA_PTR;

/* set before relocation (U-Boot then runs where the bootloader put it): in .data, not .bss, so that it survives */
unsigned long hk_load_base = 1;

/* ---- the watchdogs ---- */

#define WTCON			0x0
#define WTDAT			0x4
#define WTCNT			0x8
#define WTCON_PRESCALER		(0xffu << 8)
#define WTCON_ENABLE		BIT(5)
#define WTCON_DIV		(3u << 3)
#define WTCON_DIV128		(3u << 3)
#define WTCON_INT		BIT(2)
#define WTCON_RESET		BIT(0)

static void wdt_stop(ulong base)
{
	u32 con = readl(base + WTCON);

	printf("[oosexgang] watchdog %#lx: WTCON %#x, WTDAT %u, WTCNT %u: %s\n", base, con, readl(base + WTDAT),
	       readl(base + WTCNT), con & WTCON_ENABLE ? "counting, stopped" : "not counting");
	writel(con & ~(WTCON_ENABLE | WTCON_RESET | WTCON_INT), base + WTCON);
}

static void wdt_net(ulong base)
{
	u32 con = readl(base + WTCON);

	con &= ~(WTCON_PRESCALER | WTCON_DIV | WTCON_ENABLE | WTCON_RESET | WTCON_INT);
	writel(con | WTCON_PRESCALER | WTCON_DIV128, base + WTCON);	/* the slowest it counts */
	writel(0xffff, base + WTDAT);
	writel(0xffff, base + WTCNT);
	writel(con | WTCON_PRESCALER | WTCON_DIV128 | WTCON_ENABLE | WTCON_RESET, base + WTCON);
	printf("[oosexgang] watchdog %#lx: kept as a net, as slow as it counts (WTCON %#x)\n", base, readl(base + WTCON));
}

static int do_zuma_wdt(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	const char *list = env_get("hakios_wdt"), *net = env_get("hakios_wdt_net");
	char *end;

	if (!list) {
		printf("[oosexgang] watchdog: no $hakios_wdt (the watchdogs' addresses)\n");
		return CMD_RET_FAILURE;
	}
	while (*list) {
		ulong base = hextoul(list, &end);

		if (end == list)
			break;
		wdt_stop(base);
		list = end;
		while (*list == ' ')
			list++;
	}
	if (net && *net)
		wdt_net(hextoul(net, NULL));
	return CMD_RET_SUCCESS;
}

/* ---- the console in RAM (pstore) ---- */

#define PRAM_SIG		0x43474244	/* "DBGC": Linux's PERSISTENT_RAM_SIG */

struct pram_buffer {
	u32 sig;
	u32 start;			/* where the next byte goes */
	u32 size;			/* how many bytes are valid */
	u8 data[];
};

static struct pram_buffer *pram;
static u32 pram_cap, pram_found;

static void pram_flush(ulong lo, ulong hi)
{
	flush_dcache_range(ALIGN_DOWN(lo, ARCH_DMA_MINALIGN), ALIGN(hi, ARCH_DMA_MINALIGN));
}

static void pram_write(const char *s, size_t n)
{
	if (!pram || !n)
		return;
	if (n > pram_cap) {
		s += n - pram_cap;
		n = pram_cap;
	}
	while (n) {
		size_t chunk = min_t(size_t, n, pram_cap - pram->start);

		memcpy(pram->data + pram->start, s, chunk);
		pram_flush((ulong)pram->data + pram->start, (ulong)pram->data + pram->start + chunk);
		s += chunk;
		n -= chunk;
		pram->size = min(pram->size + (u32)chunk, pram_cap);
		pram->start = (pram->start + chunk) % pram_cap;
	}
	pram_flush((ulong)pram, (ulong)pram->data);
}

/*
 * The bootloader's own log buffer (bldr_log_reserved@fd800000 on zuma; $hakios_bldrlog): the one place seen to survive a
 * restart on the Pixel 8, and the next bootloader reports what it holds in console-ramoops-0 (pstore's zones do not
 * survive any restart there, not even Android's own). Its layout, read on the phone (zuma bldrlog, 2026-11-22): a header of
 * four words -- +0 the length of the text, +8 the room for it (0x7fff0) -- and the text from +0x10. HakiOS's lines are
 * appended as the bootloader's own; when it is full, the rest is dropped.
 */
struct bldr_log {
	u32 len;
	u32 rsvd0;
	u32 room;
	u32 rsvd1;
	char text[];
};

static struct bldr_log *bldr;
static u32 bldr_found, bldr_dropped;

static void bldr_write(const char *s, size_t n)
{
	size_t take;

	if (!bldr || !n)
		return;
	take = min_t(size_t, n, bldr->room - bldr->len);
	bldr_dropped += n - take;
	if (!take)
		return;
	memcpy(bldr->text + bldr->len, s, take);
	pram_flush((ulong)bldr->text + bldr->len, (ulong)bldr->text + bldr->len + take);
	bldr->len += take;
	pram_flush((ulong)bldr, (ulong)bldr->text);
}

static void log_write(const char *s, size_t n)
{
	pram_write(s, n);
	bldr_write(s, n);
}

static void pstore_putc(struct stdio_dev *dev, const char c)
{
	log_write(&c, 1);
}

static void pstore_puts(struct stdio_dev *dev, const char *s)
{
	log_write(s, strlen(s));
}

static int pstore_start(struct stdio_dev *dev)
{
	return pram || bldr ? 0 : -ENODEV;
}

/* the bootloader's log buffer at $hakios_bldrlog, if it has the header seen on the phone */
static void bldr_init(void)
{
	const char *where = env_get("hakios_bldrlog");
	ulong addr = where ? hextoul(where, NULL) : 0;
	struct bldr_log *b = (struct bldr_log *)addr;

	if (!addr)
		return;
	invalidate_dcache_range(ALIGN_DOWN(addr, ARCH_DMA_MINALIGN), ALIGN(addr + sizeof(*b), ARCH_DMA_MINALIGN));
	if (b->room < 0x1000 || b->room > 0x100000 || b->len > b->room) {
		printf("[oosexgang] pstore: no bootloader log at %#lx (length %#x, room %#x): not written\n", addr, b->len, b->room);
		return;
	}
	bldr = b;
	bldr_found = b->len;
}

/* set up from $hakios_pstore and registered as the console device "pstore", once U-Boot is up (before preboot) */
static int pstore_init(void)
{
	static struct stdio_dev dev = {
		.name = "pstore",
		.flags = DEV_FLAGS_OUTPUT,
		.start = pstore_start,
		.putc = pstore_putc,
		.puts = pstore_puts,
	};
	const char *where = env_get("hakios_pstore");
	ulong addr = 0, size = 0;
	char *end;

	bldr_init();
	if (where) {
		addr = hextoul(where, &end);
		while (*end == ' ')
			end++;
		size = hextoul(end, NULL);
		if (!addr || size <= sizeof(*pram) + 4096) {
			printf("[oosexgang] pstore: $hakios_pstore is not an address and a size: %s\n", where);
			addr = 0;
		}
	}
	if (!addr)
		goto zone_done;
	pram = (struct pram_buffer *)addr;
	pram_cap = size - sizeof(*pram);
	/*
	 * A log already there (the console zone: Google's bootloader writes its own log into it, as Linux's console does,
	 * and keeps what the previous kernel left at the next start) is written on, as Linux would: so the next bootloader
	 * keeps HakiOS's lines where the kernel's would be. Anything else is started afresh.
	 */
	invalidate_dcache_range(ALIGN_DOWN(addr, ARCH_DMA_MINALIGN), ALIGN(addr + sizeof(*pram), ARCH_DMA_MINALIGN));
	if (pram->sig == PRAM_SIG && pram->size <= pram_cap && pram->start <= pram_cap) {
		pram_found = pram->size;
		if (pram->start == pram_cap)
			pram->start = 0;
	} else {
		pram->sig = PRAM_SIG;
		pram->start = 0;
		pram->size = 0;
		pram_flush(addr, addr + sizeof(*pram));
	}
zone_done:
	if (!pram && !bldr)
		return 0;
	{
		static const char mark[] = "\n\n===== HakiOS U-Boot on zuma: from here, U-Boot, the loader and the kernel =====\n";

		log_write(mark, sizeof(mark) - 1);
	}

#if CONFIG_IS_ENABLED(CONSOLE_RECORD)
	/* what was printed before this device existed: the console record, which holds it all (not consumed) */
	{
		struct membuf copy = gd->console_out;
		char *data;
		int n;

		while ((n = membuf_getraw(&copy, INT_MAX, true, &data)) > 0)
			log_write(data, n);
	}
#endif
	if (stdio_register(&dev))
		printf("[oosexgang] pstore: could not register the console device\n");
	return 0;
}
EVENT_SPY_SIMPLE(EVT_LAST_STAGE_INIT, pstore_init);

static int do_zuma_pstore(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (bldr)
		printf("pstore: the bootloader's log at %#lx: %u of %u bytes (%u the bootloader's), %u dropped\n", (ulong)bldr,
		       bldr->len, bldr->room, bldr_found, bldr_dropped);
	if (!pram) {
		printf("pstore: no ramoops zone ($hakios_pstore)\n");
		return bldr ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
	}
	printf("pstore: %u of %u bytes at %#lx, the next at %u (%u there before U-Boot)\n", pram->size, pram_cap, (ulong)pram,
	       pram->start, pram_found);
	return CMD_RET_SUCCESS;
}

/*
 * zuma bldrlog [ADDR SIZE]: a look at the bootloader's own log buffer (bldr_log_reserved@fd800000, 1 MiB), which survives a
 * restart and which Google's bootloader reports from at the next start: its first words (a header?), and where its last
 * "Starting Linux kernel" line is -- the end of what the bootloader wrote -- with every header word that could be that
 * length or that end. For learning its layout, so that HakiOS's lines can go into it.
 */
static int do_zuma_bldrlog(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	ulong base = argc > 2 ? hextoul(argv[1], NULL) : 0xfd800000, size = argc > 2 ? hextoul(argv[2], NULL) : 0x100000;
	static const char key[] = "Starting Linux kernel";
	const u8 *p = (const u8 *)base;
	const u32 *w = (const u32 *)base;
	long last = -1, first = -1, hits = 0;
	ulong i, end;

	invalidate_dcache_range(base, base + size);
	printf("bldrlog at %#lx, %#lx bytes; the first words:\n", base, size);
	for (i = 0; i < 16; i++)
		printf("  +%02lx: %08x%s", i * 4, w[i], i % 4 == 3 ? "\n" : "");
	printf("  as text: \"");
	for (i = 0; i < 48; i++)
		putc(p[i] >= 32 && p[i] < 127 ? p[i] : '.');
	printf("\"\n");
	for (i = 0; i + sizeof(key) - 1 <= size; i++) {
		if (p[i] == 'S' && !memcmp(p + i, key, sizeof(key) - 1)) {
			if (first < 0)
				first = i;
			last = i;
			hits++;
		}
	}
	if (last < 0) {
		printf("no \"%s\" in it\n", key);
		return CMD_RET_SUCCESS;
	}
	for (end = last; end < size && p[end] && p[end] != '\n'; end++)
		;
	if (end < size && p[end] == '\n')
		end++;
	printf("\"%s\": %ld times, the first at +%#lx, the last at +%#lx; that line ends at +%#lx\n", key, hits, first, last, end);
	printf("  after it: \"");
	for (i = end; i < end + 40 && i < size; i++)
		putc(p[i] >= 32 && p[i] < 127 ? p[i] : '.');
	printf("\"\n  (bytes there: %02x %02x %02x %02x)\n", p[end], p[end + 1], p[end + 2], p[end + 3]);
	printf("  header words near that end (+%#lx):", end);
	for (i = 0; i < 16; i++) {
		long d = (long)w[i] - (long)end;

		if (d > -64 && d < 64)
			printf(" +%02lx=%#x (%+ld)", i * 4, w[i], d);
	}
	printf("\n  ... or near base + end (%#lx):", base + end);
	for (i = 0; i < 16; i++) {
		long d = (long)w[i] - (long)(base + end);

		if (d > -64 && d < 64)
			printf(" +%02lx=%#x (%+ld)", i * 4, w[i], d);
	}
	printf("\n");
	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_zuma_sub[] = {
	U_BOOT_CMD_MKENT(bldrlog, 3, 0, do_zuma_bldrlog, "", ""),
	U_BOOT_CMD_MKENT(wdt, 1, 0, do_zuma_wdt, "", ""),
	U_BOOT_CMD_MKENT(pstore, 1, 0, do_zuma_pstore, "", ""),
};

static int do_zuma(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct cmd_tbl *c;

	if (argc < 2)
		return CMD_RET_USAGE;
	c = find_cmd_tbl(argv[1], cmd_zuma_sub, ARRAY_SIZE(cmd_zuma_sub));
	if (!c)
		return CMD_RET_USAGE;
	return c->cmd(cmdtp, flag, argc - 1, argv + 1);
}

U_BOOT_CMD(zuma, 4, 0, do_zuma,
	"HakiOS on Google's zuma phones",
	"wdt - stop the watchdogs in $hakios_wdt, keep $hakios_wdt_net as a slow net\n"
	"zuma pstore - where the console in RAM is\n"
	"zuma bldrlog [addr size] - a look at the bootloader's log buffer (default bldr_log_reserved@fd800000)"
);
