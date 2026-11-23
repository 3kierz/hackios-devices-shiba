// SPDX-License-Identifier: GPL-2.0+
/*
 * The Pixel 8's storage (UFS) for U-Boot's SCSI layer, READ ONLY: the controller adopted as Google's bootloader leaves it.
 *
 * Read on the phone (zuma ufs, 2026-11-23): UFSHCI 3.0 at 0x13200000, enabled, the device present, both request lists
 * running and ready, nothing in flight, the PHY out of isolation -- the link is up. So nothing here resets, starts a link,
 * sends a UIC command or changes a clock or a power: the only registers written are the transfer request list's (its run
 * bit and base, while nothing is in flight, exactly as the kernel's drivers/storage/hakios.ufshc.c does) and the doorbell.
 *
 * One request at a time, in slot 0 of a list of our own, polled. Every command is checked before it goes into the slot,
 * as the kernel's driver checks them (hc_refusal): a SCSI command (no query, task management or NOP), never the W flag,
 * never data from host to device, a normal logical unit, and only READ(10), READ(16), TEST UNIT READY, REQUEST SENSE,
 * INQUIRY, READ CAPACITY(10) or READ CAPACITY(16). A write never reaches the controller: U-Boot never writes the phone's
 * storage (the only writes HakiOS ever makes are the kernel's, into darwin, past what the computer wrote).
 *
 * The data moves through a buffer of our own, cache-line aligned, flushed and invalidated around each request (U-Boot's
 * addresses are physical); the request descriptors and command descriptors the same. All three BELOW 4 GB: with them in
 * the highest bank (where this U-Boot runs) the controller took the list and never finished a command (2026-11-23: every
 * INQUIRY still in its slot after 2 s) -- the bootloader's own list is at 0xf8c3a000 and everything it reads from the
 * storage goes below 4 GB too; zuma's S2MPU in front of the UFS evidently lets it reach no higher.
 */

#include <cpu_func.h>
#include <dm.h>
#include <lmb.h>
#include <log.h>
#include <malloc.h>
#include <memalign.h>
#include <scsi.h>
#include <time.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/dma-direction.h>
#include <linux/errno.h>
#include <linux/sizes.h>
#include <linux/string.h>

#define UFSHC_CAP		0x00
#define UFSHC_VER		0x08
#define UFSHC_IS		0x20
#define UFSHC_HCS		0x30
#define UFSHC_HCE		0x34
#define UFSHC_UTRLBA		0x50
#define UFSHC_UTRLBAU		0x54
#define UFSHC_UTRLDBR		0x58
#define UFSHC_UTRLCLR		0x5c
#define UFSHC_UTRLRSR		0x60

#define CAP_64AS		BIT(24)
#define HCS_DP			BIT(0)
#define HCS_UTRLRDY		BIT(1)

#define UTRD_CT_UFS		(1u << 28)
#define UTRD_DD_MASK		(3u << 25)
#define UTRD_DD_NONE		(0u << 25)
#define UTRD_DD_OUT		(1u << 25)
#define UTRD_DD_IN		(2u << 25)
#define OCS_SUCCESS		0x0
#define OCS_INVALID		0xf

#define UPIU_COMMAND		0x01
#define UPIU_RESPONSE		0x21
#define UPIU_FLAG_READ		0x40
#define UPIU_FLAG_WRITE		0x20
#define LUN_WELL_KNOWN		0x80

#define SLOTS			32
#define UCD_RESPONSE		512
#define UCD_PRDT		1024
#define PRD_MAX			SZ_256K
#define XFER_MAX		SZ_1M		/* the most one request moves: 4 PRD entries */
#define UCD_SIZE		(UCD_PRDT + 16 * (XFER_MAX / PRD_MAX))
#define BLOCK			4096		/* every logical unit's block on this phone */
#define DMA_SIZE		SZ_2M		/* the list at +0, slot 0's command descriptor at +4 KB, the buffer at +1 MB */
#define DMA_BELOW		0xf0000000UL	/* below 4 GB, and below where hakios load puts the payload */
#define TIMEOUT_US		2000000

struct zuma_ufs {
	void __iomem *base;
	u32 *list;			/* SLOTS request descriptors of 32 bytes */
	u32 *ucd;			/* slot 0's command descriptor */
	u8 *data;			/* the transfer buffer */
	u32 old_lo, old_hi;		/* the bootloader's list */
	u32 refused, done;
};

static u32 rd(struct zuma_ufs *u, u32 reg) { return readl(u->base + reg); }
static void wr(struct zuma_ufs *u, u32 reg, u32 v) { writel(v, u->base + reg); }

static void dma_flush(const void *p, size_t n)
{
	ulong a = (ulong)p;

	flush_dcache_range(ALIGN_DOWN(a, ARCH_DMA_MINALIGN), ALIGN(a + n, ARCH_DMA_MINALIGN));
}

static void dma_inval(const void *p, size_t n)
{
	ulong a = (ulong)p;

	invalidate_dcache_range(ALIGN_DOWN(a, ARCH_DMA_MINALIGN), ALIGN(a + n, ARCH_DMA_MINALIGN));
}

/* the kernel's hc_refusal, for one command UPIU and its descriptor's first word: NULL if it may go in a slot */
static const char *refusal(const u8 *upiu, u32 dw0)
{
	const u8 *cdb = upiu + 16;
	u32 dd = dw0 & UTRD_DD_MASK;
	u64 expected = (u64)upiu[12] << 24 | (u64)upiu[13] << 16 | (u64)upiu[14] << 8 | upiu[15];

	if (dd == UTRD_DD_OUT || dd == UTRD_DD_MASK)
		return "data from host to device";
	if ((upiu[0] & 0x3f) != UPIU_COMMAND)
		return "not a SCSI command";
	if (upiu[1] & UPIU_FLAG_WRITE)
		return "the W flag";
	if (upiu[2] & LUN_WELL_KNOWN)
		return "a well-known logical unit";
	if (upiu[2] >= 8)
		return "no such logical unit";
	switch (cdb[0]) {
	case 0x28:	/* READ(10) */
	case 0x88: {	/* READ(16) */
		u64 blocks = cdb[0] == 0x28 ? ((u64)cdb[7] << 8 | cdb[8]) :
			     ((u64)cdb[10] << 24 | (u64)cdb[11] << 16 | (u64)cdb[12] << 8 | cdb[13]);

		if (dd != UTRD_DD_IN || !(upiu[1] & UPIU_FLAG_READ))
			return "a read without data from the device";
		if (!blocks || expected != blocks * BLOCK)
			return "a read whose length is not what the UPIU expects";
		return NULL;
	}
	case 0x00:	/* TEST UNIT READY */
		return dd == UTRD_DD_NONE && !expected ? NULL : "TEST UNIT READY with data";
	case 0x03:	/* REQUEST SENSE */
	case 0x12:	/* INQUIRY */
	case 0x25:	/* READ CAPACITY(10) */
		return NULL;
	case 0x9e:	/* SERVICE ACTION IN(16): READ CAPACITY(16) only */
		return (cdb[1] & 0x1f) == 0x10 ? NULL : "a SERVICE ACTION IN other than READ CAPACITY(16)";
	default:
		return "a SCSI operation not on the list";
	}
}

static int zuma_ufs_exec(struct udevice *dev, struct scsi_cmd *pccb)
{
	struct zuma_ufs *u = dev_get_priv(dev);
	u32 *utrd = u->list;
	u8 upiu[32] = { 0 };
	const u8 *r = (const u8 *)u->ucd + UCD_RESPONSE;
	bool in = pccb->dma_dir == DMA_FROM_DEVICE && pccb->datalen;
	u32 dw0 = UTRD_CT_UFS | (in ? UTRD_DD_IN : UTRD_DD_NONE), n = 0;
	const char *why;
	ulong start;

	if (pccb->dma_dir == DMA_TO_DEVICE) {
		u->refused++;
		return -EPERM;		/* never: U-Boot does not write the phone's storage */
	}
	if (pccb->datalen > XFER_MAX || pccb->cmdlen > 16)
		return -EINVAL;
	upiu[0] = UPIU_COMMAND;
	upiu[1] = in ? UPIU_FLAG_READ : 0;
	upiu[2] = pccb->lun;
	upiu[3] = 0;			/* the tag: slot 0 */
	upiu[12] = (u8)(pccb->datalen >> 24);
	upiu[13] = (u8)(pccb->datalen >> 16);
	upiu[14] = (u8)(pccb->datalen >> 8);
	upiu[15] = (u8)pccb->datalen;
	memcpy(upiu + 16, pccb->cmd, pccb->cmdlen);
	why = refusal(upiu, dw0);
	if (why) {
		u->refused++;
		log_debug("zuma ufs: refused (%s): CDB %02x lun %u\n", why, upiu[16], upiu[2]);
		return -EPERM;
	}
	if (rd(u, UFSHC_UTRLDBR) & 1)
		return -EBUSY;

	memset(u->ucd, 0, UCD_SIZE);
	memcpy(u->ucd, upiu, sizeof(upiu));
	for (u32 at = 0; at < (in ? pccb->datalen : 0); at += PRD_MAX, n++) {
		u32 *prd = u->ucd + (UCD_PRDT + 16 * n) / 4;
		u64 pa = (ulong)u->data + at;
		u32 len = min_t(u32, pccb->datalen - at, PRD_MAX);

		prd[0] = (u32)pa;
		prd[1] = (u32)(pa >> 32);
		prd[2] = 0;
		prd[3] = len - 1;
	}
	memset(utrd, 0, 32);
	utrd[0] = dw0;
	utrd[2] = OCS_INVALID;
	utrd[4] = (u32)(ulong)u->ucd;
	utrd[5] = (u32)((u64)(ulong)u->ucd >> 32);
	utrd[6] = ((UCD_RESPONSE / 4) << 16) | (UCD_RESPONSE / 4);
	utrd[7] = ((UCD_PRDT / 4) << 16) | n;
	/* what the slot holds checked again before the doorbell */
	if (memcmp(u->ucd, upiu, sizeof(upiu)) || utrd[0] != dw0 || refusal((const u8 *)u->ucd, utrd[0])) {
		u->refused++;
		return -EPERM;
	}
	dma_flush(u->ucd, UCD_SIZE);
	dma_flush(utrd, 32);
	if (in)
		dma_inval(u->data, pccb->datalen);
	dsb();
	wr(u, UFSHC_UTRLDBR, 1);

	start = timer_get_us();
	while (rd(u, UFSHC_UTRLDBR) & 1) {
		if (timer_get_us() - start > TIMEOUT_US) {
			printf("zuma ufs: CDB %02x lun %u not done in %u ms (IS %#x HCS %#x): the slot cleared\n", upiu[16],
			       upiu[2], TIMEOUT_US / 1000, rd(u, UFSHC_IS), rd(u, UFSHC_HCS));
			wr(u, UFSHC_UTRLCLR, ~1u);
			return -ETIMEDOUT;
		}
	}
	dma_inval(utrd, 32);
	dma_inval(u->ucd, UCD_SIZE);
	pccb->status = r[7];
	if ((utrd[2] & 0xff) != OCS_SUCCESS || (r[0] & 0x3f) != UPIU_RESPONSE || r[6]) {
		printf("zuma ufs: CDB %02x lun %u: OCS %#x, response %02x %02x, status %02x\n", upiu[16], upiu[2],
		       utrd[2] & 0xff, r[0], r[6], r[7]);
		return -EIO;
	}
	if (r[7]) {			/* CHECK CONDITION and the like: the sense data after the header and its length */
		memcpy(pccb->sense_buf, r + 34, min_t(size_t, sizeof(pccb->sense_buf), 18));
		return -EIO;
	}
	if (in) {
		dma_inval(u->data, pccb->datalen);
		memcpy(pccb->pdata, u->data, pccb->datalen);
		pccb->trans_bytes = pccb->datalen;
	}
	u->done++;
	return 0;
}

static int zuma_ufs_bus_reset(struct udevice *dev)
{
	return 0;			/* never: the link is the bootloader's, and stays as it is */
}

static int zuma_ufs_probe(struct udevice *dev)
{
	struct zuma_ufs *u = dev_get_priv(dev);
	struct scsi_plat *plat = dev_get_uclass_plat(dev);
	u32 cap, hcs, db, rsr;
	ulong start;

	u->base = dev_read_addr_ptr(dev);
	if (!u->base)
		return -EINVAL;
	cap = rd(u, UFSHC_CAP);
	hcs = rd(u, UFSHC_HCS);
	db = rd(u, UFSHC_UTRLDBR);
	rsr = rd(u, UFSHC_UTRLRSR);
	u->old_lo = rd(u, UFSHC_UTRLBA);
	u->old_hi = rd(u, UFSHC_UTRLBAU);
	if (!(rd(u, UFSHC_HCE) & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY) || !(rsr & 1) || db || !(cap & CAP_64AS)) {
		printf("[oosexgang] ufs: not adopted: HCE %#x HCS %#x run %#x doorbell %#x CAP %#x (not left up as expected)\n",
		       rd(u, UFSHC_HCE), hcs, rsr, db, cap);
		return -ENODEV;
	}
	{
		phys_addr_t dma = DMA_BELOW;

		if (lmb_alloc_mem(LMB_MEM_ALLOC_MAX, SZ_64K, &dma, DMA_SIZE, LMB_NONE) || dma + DMA_SIZE > DMA_BELOW) {
			printf("[oosexgang] ufs: not adopted: no %u KB of memory below %#lx for the controller\n", DMA_SIZE / 1024,
			       DMA_BELOW);
			return -ENOMEM;
		}
		u->list = (u32 *)(ulong)dma;
		u->ucd = (u32 *)(ulong)(dma + SZ_4K);
		u->data = (u8 *)(ulong)(dma + SZ_1M);
	}
	memset(u->list, 0, SLOTS * 32);
	dma_flush(u->list, SLOTS * 32);

	/* the list switched while nothing is in flight: stopped, pointed at ours, started (hakios.ufshc.c's way) */
	wr(u, UFSHC_UTRLRSR, 0);
	for (start = timer_get_us(); (rd(u, UFSHC_UTRLRSR) & 1) && timer_get_us() - start < 5000;)
		;
	if (rd(u, UFSHC_UTRLRSR) & 1) {
		wr(u, UFSHC_UTRLRSR, 1);
		printf("[oosexgang] ufs: not adopted: the bootloader's list did not stop\n");
		return -EBUSY;
	}
	wr(u, UFSHC_UTRLBA, (u32)(ulong)u->list);
	wr(u, UFSHC_UTRLBAU, (u32)((u64)(ulong)u->list >> 32));
	wr(u, UFSHC_UTRLRSR, 1);
	if (rd(u, UFSHC_UTRLBA) != (u32)(ulong)u->list || !(rd(u, UFSHC_UTRLRSR) & 1)) {
		wr(u, UFSHC_UTRLRSR, 0);
		wr(u, UFSHC_UTRLBA, u->old_lo);
		wr(u, UFSHC_UTRLBAU, u->old_hi);
		wr(u, UFSHC_UTRLRSR, 1);
		printf("[oosexgang] ufs: not adopted: the controller did not take our list (the bootloader's put back)\n");
		return -EIO;
	}
	plat->max_id = 1;
	plat->max_lun = 8;
	plat->max_bytes_per_req = XFER_MAX;
	printf("[oosexgang] ufs: adopted as the bootloader left it (UFSHCI %u.%u, list %#x:%08x -> %p), read only\n",
	       (rd(u, UFSHC_VER) >> 8) & 0xf, (rd(u, UFSHC_VER) >> 4) & 0xf, u->old_hi, u->old_lo, u->list);
	return 0;
}

static const struct scsi_ops zuma_ufs_ops = {
	.exec = zuma_ufs_exec,
	.bus_reset = zuma_ufs_bus_reset,
};

static const struct udevice_id zuma_ufs_ids[] = {
	{ .compatible = "hakios,zuma-ufs" },
	{ }
};

U_BOOT_DRIVER(zuma_ufs) = {
	.name = "zuma_ufs",
	.id = UCLASS_SCSI,
	.of_match = zuma_ufs_ids,
	.ops = &zuma_ufs_ops,
	.probe = zuma_ufs_probe,
	.priv_auto = sizeof(struct zuma_ufs),
};
