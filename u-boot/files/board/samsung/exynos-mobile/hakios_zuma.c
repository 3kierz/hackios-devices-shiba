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

static void pstore_putc(struct stdio_dev *dev, const char c)
{
	pram_write(&c, 1);
}

static void pstore_puts(struct stdio_dev *dev, const char *s)
{
	pram_write(s, strlen(s));
}

static int pstore_start(struct stdio_dev *dev)
{
	return pram ? 0 : -ENODEV;
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
	ulong addr, size;
	char *end;

	if (!where)
		return 0;
	addr = hextoul(where, &end);
	size = hextoul(end, NULL);
	if (!addr || size <= sizeof(*pram) + 4096) {
		printf("[oosexgang] pstore: $hakios_pstore is not an address and a size: %s\n", where);
		return 0;
	}
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
	{
		static const char mark[] = "\n\n===== HakiOS U-Boot on zuma: from here, U-Boot, the loader and the kernel =====\n";

		pram_write(mark, sizeof(mark) - 1);
	}

#if CONFIG_IS_ENABLED(CONSOLE_RECORD)
	/* what was printed before this device existed: the console record, which holds it all (not consumed) */
	{
		struct membuf copy = gd->console_out;
		char *data;
		int n;

		while ((n = membuf_getraw(&copy, INT_MAX, true, &data)) > 0)
			pram_write(data, n);
	}
#endif
	if (stdio_register(&dev))
		printf("[oosexgang] pstore: could not register the console device\n");
	return 0;
}
EVENT_SPY_SIMPLE(EVT_LAST_STAGE_INIT, pstore_init);

static int do_zuma_pstore(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (!pram) {
		printf("pstore: not set up ($hakios_pstore)\n");
		return CMD_RET_FAILURE;
	}
	printf("pstore: %u of %u bytes at %#lx, the next at %u (%u there before U-Boot)\n", pram->size, pram_cap, (ulong)pram,
	       pram->start, pram_found);
	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_zuma_sub[] = {
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

U_BOOT_CMD(zuma, 2, 0, do_zuma,
	"HakiOS on Google's zuma phones",
	"wdt - stop the watchdogs in $hakios_wdt, keep $hakios_wdt_net as a slow net\n"
	"zuma pstore - where the console in RAM is"
);
