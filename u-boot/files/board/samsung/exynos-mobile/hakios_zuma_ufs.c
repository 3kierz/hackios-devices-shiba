// SPDX-License-Identifier: GPL-2.0+
/*
 * The Pixel 8's storage (UFS) for U-Boot's SCSI layer, READ ONLY towards its contents: the controller and the device
 * brought up from cold, as Google's ufs-exynos.c and Linux's ufshcd do at boot.
 *
 * Why from cold (INSTALL-PLAN.md, phase 1, runs 1-5 on 2026-11-23): Google's bootloader leaves the controller enabled
 * and the link in Hibern8, but powers the UFS device off before it hands over -- its supply (ufs-vcc, gpp0-1) driven low
 * and the reference clock's pin (gph5-0) turned into a GPIO held low -- so the link cannot be woken; adopting it as it was
 * ended every time in PWR_FATAL_ERROR. So, in the order Google's driver and the Linux core run:
 *
 *   1. externals (exynos_ufs_config_externals): the PHY out of isolation (PMU, read; Google's bootloader already left it
 *      so) and the controller's IO coherency in SYSREG_HSI2 (+0x710, bits 1:0);
 *   2. the device powered (the tree's ufs_fixed_vcc: gpp0-1 high) and the controller's own pins back on function 2
 *      (ufs-refclk-out gph5-0, ufs-rst-n gph5-1, no pull);
 *   3. the controller stopped (HCE 0), Samsung's software reset, its host set-up (exynos_ufs_config_host), the device's
 *      reset pulse (HCI_GPIO_OUT), HCE 1, the clocks' set-up (hce_enable_notify POST_CHANGE);
 *   4. the link: Samsung's CAL pre_link (zuma's init_cfg_evt0: the PHY and UniPro tables, hakios_zuma_cal.c, Samsung's
 *      own code), DME_LINKSTARTUP (up to four tries, as Linux), CAL post_link, the lists' bases and run bits;
 *   5. the device: NOP OUT until NOP IN, fDeviceInit set and polled until the device clears it (Linux's
 *      ufshcd_verify_dev_init and ufshcd_complete_dev_init), and each logical unit's power-on UNIT ATTENTION collected.
 *
 * The link is then in PWM-G1 (slow: a few MB/s, enough for the tables and checks); one block is read as a test, and
 * "zuma ufs hs" (hk_zuma_ufs_hs; in the probe when $hakios_ufs_hs is 1, or the PWM-G1 read failed) asks for HS-G4 rate B
 * on every connected lane, with the CAL's pre_pmc and post_pmc around the power mode change.
 *
 * Run 6 (2026-11-23): the link up on both lanes, NOP IN at once, fDeviceInit done, bRefClkFreq 38.4 MHz; TEST UNIT READY,
 * INQUIRY and READ CAPACITY answered; every 4 KB READ(10) not done in 10 s (IS: a UIC error), and the ten of them the
 * watchdog's net. So: PRD entries of 4 KB as Google's driver (max_segment_size 4096), 2 s and then no more requests,
 * the error registers at the failure, and the test reads.
 *
 * What reaches the device: the UPIUs this file builds and nothing else. Device management: NOP OUT; QUERY READ
 * ATTRIBUTE and READ FLAG; and one write request, SET FLAG fDeviceInit, the device's own initialisation every host does
 * at every boot (no descriptor, attribute or data written). SCSI: every command checked before it goes into the slot, as
 * the kernel's driver checks them (hc_refusal): never the W flag, never data from host to device, a normal logical unit,
 * and only READ(10), READ(16), TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY(10) or READ CAPACITY(16). U-Boot
 * never writes the phone's storage; the only writes HakiOS ever makes are the kernel's, into darwin.
 *
 * One request at a time, in slot 0, polled. The request list, the command descriptor and the data buffer all BELOW 4 GB
 * (Google's driver sets a 32-bit DMA mask; the bootloader's own lists are at 0xf8c3a000), cache-line flushed and
 * invalidated around each request; the data copied to and from the caller's buffer.
 */

#include <cpu_func.h>
#include <dm.h>
#include <env.h>
#include <lmb.h>
#include <log.h>
#include <malloc.h>
#include <scsi.h>
#include <time.h>
#include <u-boot/crc.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/dma-direction.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include "zuma-cal/ufs-vs-mmio.h"
#include "zuma-cal/ufs-cal-if.h"

#define P			"[oosexgang] ufs: "

/* the phone's tree (zuma-ufs.dtsi ufs@13200000, its reg list in order; pinctrl, PMU and SYSREG nodes) */
#define UFS_HCI			0x13201100UL	/* Samsung's vendor HCI */
#define UFS_UNIPRO		0x13280000UL
#define UFS_UFSP		0x132a0000UL
#define UFS_PMA			0x13204000UL
#define UFS_CPORT		0x13208000UL
#define PMU_UFS_PHY		0x15463ec0UL	/* ufs-phy-iso: offset 0x3ec0, mask 1, val 1 (1 = isolation off) */
#define SYSREG_UFS_IOCC		0x13020710UL	/* ufs-iocc: sysreg_ufs@13020000 + 0x710, mask 3, val 3 */
#define GPP0			0x10840000UL	/* ufs_fixed_vcc: gpp0-1, active high (CON +0, DAT +4) */
#define GPH5			0x13060000UL	/* ufs-refclk-out gph5-0, ufs-rst-n gph5-1: function 2, no pull */

/* the standard UFSHCI registers */
#define UFSHC_CAP		0x00
#define UFSHC_VER		0x08
#define UFSHC_IS		0x20
#define UFSHC_IE		0x24
#define UFSHC_HCS		0x30
#define UFSHC_HCE		0x34
#define UFSHC_UECPA		0x38
#define UFSHC_UECDL		0x3c
#define UFSHC_UECN		0x40
#define UFSHC_UECT		0x44
#define UFSHC_UECDME		0x48
#define UFSHC_UTRLBA		0x50
#define UFSHC_UTRLBAU		0x54
#define UFSHC_UTRLDBR		0x58
#define UFSHC_UTRLCLR		0x5c
#define UFSHC_UTRLRSR		0x60
#define UFSHC_UTMRLBA		0x70
#define UFSHC_UTMRLBAU		0x74
#define UFSHC_UTMRLRSR		0x80
#define UFSHC_UICCMD		0x90
#define UFSHC_UICCMDARG1	0x94
#define UFSHC_UICCMDARG2	0x98
#define UFSHC_UICCMDARG3	0x9c

#define HCS_DP			BIT(0)
#define HCS_UTRLRDY		BIT(1)
#define HCS_UTMRLRDY		BIT(2)
#define HCS_UCRDY		BIT(3)
#define HCS_UPMCRS(h)		(((h) >> 8) & 7)
#define UPMCRS_PWR_LOCAL	1
#define IS_UPMS			BIT(4)
#define IS_UCCS			BIT(10)

#define UIC_DME_SET		0x02
#define UIC_LINK_STARTUP	0x16
#define MIB(a)			((u32)(a) << 16)

/* UniPro attributes for the power mode change (MIPI UniPro, Linux's unipro.h) */
#define PA_ACTIVETXDATALANES	0x1560
#define PA_TXGEAR		0x1568
#define PA_TXTERMINATION	0x1569
#define PA_HSSERIES		0x156a
#define PA_PWRMODE		0x1571
#define PA_ACTIVERXDATALANES	0x1580
#define PA_RXGEAR		0x1583
#define PA_RXTERMINATION	0x1584

/* Samsung's vendor HCI (ufs-vs-regs.h) */
#define HCI_TXPRDT_ENTRY_SIZE	0x00
#define HCI_RXPRDT_ENTRY_SIZE	0x04
#define HCI_1US_TO_CNT_VAL	0x0c
#define HCI_UTRL_NEXUS_TYPE	0x40		/* a bit a slot: 1 = a SCSI command, 0 = device management */
#define HCI_UTMRL_NEXUS_TYPE	0x44
#define HCI_SW_RST		0x50
#define  UFS_SW_RST_MASK	(BIT(1) | BIT(0))
#define HCI_DATA_REORDER	0x60
#define HCI_AXIDMA_RWDATA_BURST_LEN 0x6c
#define  WLU_EN			BIT(31)
#define  BURST_LEN(x)		((x) << 27 | (x))
#define HCI_GPIO_OUT		0x70		/* bit 0: the device's RST_n */
#define HCI_ERROR_EN_DL_LAYER	0x7c
#define HCI_ERROR_EN_N_LAYER	0x80
#define HCI_ERROR_EN_T_LAYER	0x84
#define HCI_UFSHCI_V2P1_CTRL	0x8c
#define  IA_TICK_SEL		BIT(16)
#define HCI_CLKSTOP_CTRL	0xb0
#define  CLK_STOP_ALL		(BIT(4) | BIT(2) | BIT(1) | BIT(0))
#define HCI_FORCE_HCS		0xb4
#define  REFCLKOUT_STOP_EN	BIT(11)
#define  MPHY_APBCLK_STOP_EN	BIT(10)
#define  UFSP_DRCG_EN		BIT(8)
#define  REFCLK_STOP_EN		BIT(7)
#define  UNIPRO_PCLK_STOP_EN	BIT(6)
#define  UNIPRO_MCLK_STOP_EN	BIT(5)
#define  HCI_CORECLK_STOP_EN	BIT(4)
#define  CLK_STOP_CTRL_EN_ALL	(UFSP_DRCG_EN | MPHY_APBCLK_STOP_EN | REFCLKOUT_STOP_EN | REFCLK_STOP_EN | \
				 UNIPRO_PCLK_STOP_EN | UNIPRO_MCLK_STOP_EN)
#define HCI_IOP_ACG_DISABLE	0x100
#define DFES_ERR_EN		BIT(31)

/* UniPro (ufs-vs-regs.h; Google's two debug suites as the phone's ufs-exynos-gs.ko writes them) */
#define UNIP_PA_DBG_OPTION_SUITE_1 0x39a8
#define  DBG_SUITE1_ENABLE	0x90913c1c
#define UNIP_PA_DBG_OPTION_SUITE_2 0x39b4
#define  DBG_SUITE2_ENABLE	0xe01c115f
#define UNIP_PA_AVAILTXDATALANES 0x3080
#define UNIP_PA_AVAILRXDATALANES 0x3100
#define UNIP_PA_ACTIVETXDATALANES 0x3180
#define UNIP_PA_CONNECTEDTXDATALANES 0x3184
#define UNIP_PA_ACTIVERXDATALANES 0x3200
#define UNIP_PA_CONNECTEDRXDATALANES 0x3204
#define UNIP_PA_MAXRXHSGEAR	0x321c

#define UTRD_CT_UFS		(1u << 28)
#define UTRD_DD_MASK		(3u << 25)
#define UTRD_DD_NONE		(0u << 25)
#define UTRD_DD_OUT		(1u << 25)
#define UTRD_DD_IN		(2u << 25)
#define OCS_SUCCESS		0x0
#define OCS_INVALID		0xf

#define UPIU_NOP_OUT		0x00
#define UPIU_COMMAND		0x01
#define UPIU_QUERY		0x16
#define UPIU_NOP_IN		0x20
#define UPIU_RESPONSE		0x21
#define UPIU_QUERY_RESPONSE	0x36
#define UPIU_FLAG_READ		0x40
#define UPIU_FLAG_WRITE		0x20
#define LUN_WELL_KNOWN		0x80
#define QUERY_READ		0x01		/* query functions */
#define QUERY_WRITE		0x81
#define QOP_READ_ATTR		0x03
#define QOP_READ_FLAG		0x05
#define QOP_SET_FLAG		0x06
#define FLAG_DEVICE_INIT	0x01
#define ATTR_REF_CLK_FREQ	0x0a

#define UCD_RESPONSE		512
#define UCD_PRDT		1024
#define PRD_MAX			SZ_4K		/* Google's max_segment_size, the controller's PRDT_SET_SIZE(12) */
#define XFER_MAX		SZ_1M		/* the most one request moves: 256 PRD entries */
#define UCD_SIZE		(UCD_PRDT + 16 * (XFER_MAX / PRD_MAX))
#define BLOCK			4096		/* every logical unit's block on this phone */
#define DMA_SIZE		SZ_2M		/* the list at +0, the task list at +1 KB, the command descriptor at +4 KB,
						 * the buffer at +1 MB */
#define DMA_BELOW		0xf0000000UL	/* below 4 GB, and below where hakios load puts the payload */
#define TIMEOUT_US		2000000		/* a request (the watchdog's net is about 100 s for everything) */

struct zuma_ufs {
	void __iomem *base;
	struct ufs_vs_handle h;
	struct ufs_cal_param cal;
	struct uic_pwr_mode pmd;
	u32 *list;			/* the request list: 32 descriptors of 32 bytes, slot 0 used */
	u32 *tmlist;			/* the task management list (unused, but the controller wants one) */
	u32 *ucd;			/* slot 0's command descriptor */
	u8 *data;			/* the transfer buffer */
	u32 mclk;
	u8 lanes;
	bool up, hs, broken;		/* broken: a request timed out, nothing more is sent */
	u32 refused, done;
};

static struct zuma_ufs *zuma_ufs_up;
int hk_zuma_ufs_hs(void);	/* for "zuma ufs hs" */

static u32 rd(struct zuma_ufs *u, u32 reg) { return readl(u->base + reg); }
static void wr(struct zuma_ufs *u, u32 reg, u32 v) { writel(v, u->base + reg); }
static u32 hrd(struct zuma_ufs *u, u32 reg) { return hci_readl(&u->h, reg); }
static void hwr(struct zuma_ufs *u, u32 reg, u32 v) { hci_writel(&u->h, v, reg); }
static u32 urd(struct zuma_ufs *u, u32 reg) { return unipro_readl(&u->h, reg); }
static void uwr(struct zuma_ufs *u, u32 reg, u32 v) { unipro_writel(&u->h, v, reg); }

static void cal_udelay(u32 us) { udelay(us); }

static int wait_bits(struct zuma_ufs *u, u32 reg, u32 mask, u32 val, u32 us)
{
	ulong start = timer_get_us();

	while ((rd(u, reg) & mask) != val)
		if (timer_get_us() - start > us)
			return -ETIMEDOUT;
	return 0;
}

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

static void dump(struct zuma_ufs *u, const char *when)
{
	printf(P "%s: HCE %#x HCS %#x IS %#x UIC %#x/%#x UEC PA %#x DL %#x N %#x T %#x DME %#x\n", when, rd(u, UFSHC_HCE),
	       rd(u, UFSHC_HCS), rd(u, UFSHC_IS), rd(u, UFSHC_UICCMD), rd(u, UFSHC_UICCMDARG2), rd(u, UFSHC_UECPA),
	       rd(u, UFSHC_UECDL), rd(u, UFSHC_UECN), rd(u, UFSHC_UECT), rd(u, UFSHC_UECDME));
}

/* Google's ufs_call_cal: the UniPro and M-PHY APB clocks kept running around Samsung's CAL, the PHY's let stop after */
static int call_cal(struct zuma_ufs *u, const char *what, enum ufs_cal_errno (*fn)(struct ufs_cal_param *))
{
	enum ufs_cal_errno e;

	hwr(u, HCI_FORCE_HCS, hrd(u, HCI_FORCE_HCS) & ~(UNIPRO_MCLK_STOP_EN | MPHY_APBCLK_STOP_EN));
	e = fn(&u->cal);
	hwr(u, HCI_FORCE_HCS, hrd(u, HCI_FORCE_HCS) | MPHY_APBCLK_STOP_EN);
	if (e)
		printf(P "Samsung's CAL %s: error %d\n", what, e);
	return e ? -EIO : 0;
}

/* one UIC command; its result (ARGUMENT2 bits 7:0) must be 0 */
static int uic(struct zuma_ufs *u, u32 cmd, u32 arg1, u32 arg3, const char *what)
{
	u32 res;

	if (wait_bits(u, UFSHC_HCS, HCS_UCRDY, HCS_UCRDY, 100000)) {
		printf(P "%s: the UIC is not ready\n", what);
		return -EBUSY;
	}
	wr(u, UFSHC_IS, IS_UCCS);		/* (write 1 to clear) */
	wr(u, UFSHC_UICCMDARG1, arg1);
	wr(u, UFSHC_UICCMDARG2, 0);
	wr(u, UFSHC_UICCMDARG3, arg3);
	wr(u, UFSHC_UICCMD, cmd);
	if (wait_bits(u, UFSHC_IS, IS_UCCS, IS_UCCS, 500000)) {
		printf(P "%s: no completion in 500 ms\n", what);
		return -ETIMEDOUT;
	}
	res = rd(u, UFSHC_UICCMDARG2) & 0xff;
	wr(u, UFSHC_IS, IS_UCCS);
	if (res) {
		printf(P "%s: UIC result %u\n", what, res);
		return -EIO;
	}
	return 0;
}

static int dme_set(struct zuma_ufs *u, u32 attr, u32 val, const char *what)
{
	return uic(u, UIC_DME_SET, MIB(attr), val, what);
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

/* the device management this file sends: NULL if it may go in a slot */
static const char *dev_refusal(const u8 *upiu)
{
	switch (upiu[0]) {
	case UPIU_NOP_OUT:
		return NULL;
	case UPIU_QUERY:
		if (upiu[5] == QUERY_READ && (upiu[12] == QOP_READ_ATTR || upiu[12] == QOP_READ_FLAG))
			return NULL;
		if (upiu[5] == QUERY_WRITE && upiu[12] == QOP_SET_FLAG && upiu[13] == FLAG_DEVICE_INIT)
			return NULL;
		return "a query other than reading, or setting fDeviceInit";
	default:
		return "not NOP OUT or QUERY";
	}
}

/* slot 0 rung and waited for; the descriptor's OCS returned, or a negative error */
static int ring(struct zuma_ufs *u, u32 us)
{
	ulong start;

	dma_flush(u->ucd, UCD_SIZE);
	dma_flush(u->list, 32);
	wr(u, UFSHC_IS, rd(u, UFSHC_IS));	/* what follows is this request's */
	dsb();
	wr(u, UFSHC_UTRLDBR, 1);
	start = timer_get_us();
	while (rd(u, UFSHC_UTRLDBR) & 1) {
		if (timer_get_us() - start > us) {
			wr(u, UFSHC_UTRLCLR, ~1u);	/* (0 clears a slot) */
			return -ETIMEDOUT;
		}
	}
	dma_inval(u->list, 32);
	dma_inval(u->ucd, UCD_SIZE);
	return u->list[2] & 0xff;
}

static void slot0(struct zuma_ufs *u, u32 dw0, u32 prds)
{
	u32 *utrd = u->list;

	memset(utrd, 0, 32);
	utrd[0] = dw0;
	utrd[2] = OCS_INVALID;
	utrd[4] = (u32)(ulong)u->ucd;
	utrd[5] = (u32)((u64)(ulong)u->ucd >> 32);
	utrd[6] = ((UCD_RESPONSE / 4) << 16) | (UCD_RESPONSE / 4);
	utrd[7] = ((UCD_PRDT / 4) << 16) | prds;
}

/* one device management request (NOP OUT or QUERY), slot 0 marked as such for its duration (Google's nexus types) */
static int dev_cmd(struct zuma_ufs *u, u8 type, u8 func, u8 op, u8 idn, u32 *value, u32 us)
{
	u8 *q = (u8 *)u->ucd;
	const u8 *r = q + UCD_RESPONSE;
	const char *why;
	int ocs;

	memset(u->ucd, 0, UCD_SIZE);
	q[0] = type;
	if (type == UPIU_QUERY) {
		q[5] = func;
		q[12] = op;
		q[13] = idn;
	}
	why = dev_refusal(q);
	if (why) {
		u->refused++;
		printf(P "refused (%s)\n", why);
		return -EPERM;
	}
	slot0(u, UTRD_CT_UFS | UTRD_DD_NONE, 0);
	hwr(u, HCI_UTRL_NEXUS_TYPE, hrd(u, HCI_UTRL_NEXUS_TYPE) & ~1u);
	ocs = ring(u, us);
	hwr(u, HCI_UTRL_NEXUS_TYPE, hrd(u, HCI_UTRL_NEXUS_TYPE) | 1u);
	if (ocs < 0)
		return ocs;
	if (ocs != OCS_SUCCESS || r[0] != (type == UPIU_QUERY ? UPIU_QUERY_RESPONSE : UPIU_NOP_IN) || r[6]) {
		printf(P "UPIU %#x op %#x idn %#x: OCS %#x, response %02x, code %02x\n", type, op, idn, ocs, r[0], r[6]);
		return -EIO;
	}
	if (value)
		*value = (u32)r[20] << 24 | (u32)r[21] << 16 | (u32)r[22] << 8 | r[23];
	return 0;
}

static int zuma_ufs_exec(struct udevice *dev, struct scsi_cmd *pccb)
{
	struct zuma_ufs *u = dev_get_priv(dev);
	u8 upiu[32] = { 0 };
	const u8 *r = (const u8 *)u->ucd + UCD_RESPONSE;
	bool in = pccb->dma_dir == DMA_FROM_DEVICE && pccb->datalen;
	u32 dw0 = UTRD_CT_UFS | (in ? UTRD_DD_IN : UTRD_DD_NONE), n = 0;
	const char *why;
	int ocs;

	if (!u->up)
		return -ENODEV;
	if (u->broken)
		return -EIO;
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
	slot0(u, dw0, n);
	/* what the slot holds checked again before the doorbell, and slot 0 a SCSI command's */
	if (memcmp(u->ucd, upiu, sizeof(upiu)) || u->list[0] != dw0 || refusal((const u8 *)u->ucd, u->list[0])) {
		u->refused++;
		return -EPERM;
	}
	if (!(hrd(u, HCI_UTRL_NEXUS_TYPE) & 1))
		hwr(u, HCI_UTRL_NEXUS_TYPE, hrd(u, HCI_UTRL_NEXUS_TYPE) | 1u);
	if (in)
		dma_inval(u->data, pccb->datalen);
	ocs = ring(u, TIMEOUT_US);
	if (ocs == -ETIMEDOUT) {
		printf("zuma ufs: CDB %02x lun %u (%lu bytes) not done in %u ms: the slot cleared, no more requests\n",
		       upiu[16], upiu[2], pccb->datalen, TIMEOUT_US / 1000);
		dump(u, "after the timeout");
		u->broken = true;
		return ocs;
	}
	pccb->status = r[7];
	if (ocs != OCS_SUCCESS || (r[0] & 0x3f) != UPIU_RESPONSE) {
		printf("zuma ufs: CDB %02x lun %u: OCS %#x, UPIU %02x\n", upiu[16], upiu[2], ocs, r[0]);
		dump(u, "after the error");
		return -EIO;
	}
	if (r[7]) {			/* CHECK CONDITION and the like (the response byte then 1): the sense data after the
					 * header and its length */
		memcpy(pccb->sense_buf, r + 34, min_t(size_t, sizeof(pccb->sense_buf), 18));
		return -EIO;
	}
	if (r[6]) {
		printf("zuma ufs: CDB %02x lun %u: response %02x with GOOD status\n", upiu[16], upiu[2], r[6]);
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
	return 0;
}

/* 1. exynos_ufs_config_externals */
static int externals(struct zuma_ufs *u)
{
	u32 iso = readl(PMU_UFS_PHY), iocc = readl(SYSREG_UFS_IOCC);

	if (!(iso & 1)) {
		/* (the PMU's registers are the secure world's to write on zuma; the bootloader has always left it so) */
		printf(P "the PHY is isolated (PMU %#x): not brought up\n", iso);
		return -EIO;
	}
	if ((iocc & 3) != 3)
		writel(iocc | 3, SYSREG_UFS_IOCC);
	printf(P "PHY out of isolation (PMU %#x); IO coherency %#x -> %#x\n", iso, iocc, readl(SYSREG_UFS_IOCC));
	return 0;
}

/* 2. the device's supply on, and the controller's pins */
static void power_device(struct zuma_ufs *u)
{
	u32 c0 = readl(GPP0), d0 = readl(GPP0 + 4), c5 = readl(GPH5), p5 = readl(GPH5 + 8);

	writel(d0 | BIT(1), GPP0 + 4);
	writel((c0 & ~(0xfu << 4)) | (1u << 4), GPP0);
	writel(p5 & ~0xffu, GPH5 + 8);
	writel((c5 & ~0xffu) | 0x22, GPH5);
	mdelay(10);
	printf(P "device powered: vcc gpp0 CON %#x DAT %#x -> %#x %#x; gph5 CON %#x PUD %#x -> %#x %#x\n", c0, d0,
	       readl(GPP0), readl(GPP0 + 4), c5, p5, readl(GPH5), readl(GPH5 + 8));
}

/* 3. Google's exynos_ufs_config_host */
static void config_host(struct zuma_ufs *u)
{
	hwr(u, HCI_FORCE_HCS, hrd(u, HCI_FORCE_HCS) & ~HCI_CORECLK_STOP_EN);
	hwr(u, HCI_UFSHCI_V2P1_CTRL, hrd(u, HCI_UFSHCI_V2P1_CTRL) | IA_TICK_SEL);
	hwr(u, HCI_1US_TO_CNT_VAL, (u->mclk / 1000000) & 0x3ff);
	hwr(u, HCI_DATA_REORDER, 0xa);
	hwr(u, HCI_TXPRDT_ENTRY_SIZE, BIT(31) | 12);
	hwr(u, HCI_RXPRDT_ENTRY_SIZE, 12);
	hwr(u, HCI_UTRL_NEXUS_TYPE, 0xffffffff);
	hwr(u, HCI_UTMRL_NEXUS_TYPE, 0xffffffff);
	hwr(u, HCI_AXIDMA_RWDATA_BURST_LEN, WLU_EN | BURST_LEN(3));
	hwr(u, HCI_IOP_ACG_DISABLE, hrd(u, HCI_IOP_ACG_DISABLE) & ~1u);
	uwr(u, UNIP_PA_DBG_OPTION_SUITE_1, DBG_SUITE1_ENABLE);
	uwr(u, UNIP_PA_DBG_OPTION_SUITE_2, DBG_SUITE2_ENABLE);
}

/* 3. Linux's ufshcd_hba_enable with Google's hce_enable_notify around it */
static int hba_enable(struct zuma_ufs *u)
{
	ulong start;
	u32 tx, rx;

	if (rd(u, UFSHC_HCE) & 1) {
		wr(u, UFSHC_HCE, 0);
		if (wait_bits(u, UFSHC_HCE, 1, 0, 10000))
			printf(P "the controller did not stop (HCE %#x)\n", rd(u, UFSHC_HCE));
	}
	hwr(u, HCI_SW_RST, UFS_SW_RST_MASK);
	start = timer_get_us();
	while (hrd(u, HCI_SW_RST) & UFS_SW_RST_MASK)
		if (timer_get_us() - start > 1000) {
			printf(P "Samsung's software reset did not finish (%#x)\n", hrd(u, HCI_SW_RST));
			return -ETIMEDOUT;
		}
	config_host(u);
	hwr(u, HCI_GPIO_OUT, 0);		/* the device's reset pulse (exynos_ufs_dev_hw_reset) */
	udelay(5);
	hwr(u, HCI_GPIO_OUT, 1);

	wr(u, UFSHC_HCE, 1);
	mdelay(1);
	if (wait_bits(u, UFSHC_HCE, 1, 1, 50000)) {
		dump(u, "the controller did not enable");
		return -EIO;
	}
	hwr(u, HCI_FORCE_HCS, hrd(u, HCI_FORCE_HCS) | CLK_STOP_CTRL_EN_ALL);
	hwr(u, HCI_CLKSTOP_CTRL, hrd(u, HCI_CLKSTOP_CTRL) & ~CLK_STOP_ALL);
	tx = urd(u, UNIP_PA_AVAILTXDATALANES);
	rx = urd(u, UNIP_PA_AVAILRXDATALANES);
	if (!rx || rx != tx || rx > MAX_LANE) {
		printf(P "the host's lanes: rx %u tx %u, not usable\n", rx, tx);
		return -EIO;
	}
	u->lanes = rx;
	return 0;
}

/* 4. the link: Linux's ufshcd_link_startup with Google's link_startup_notify around DME_LINKSTARTUP */
static int link_up(struct zuma_ufs *u)
{
	u32 ctx, crx, maxg;
	int ret = -EIO;

	for (int t = 0; t < 4; t++) {
		ret = hba_enable(u);
		if (ret)
			return ret;
		hwr(u, HCI_ERROR_EN_DL_LAYER, DFES_ERR_EN | 0x20 | 0x2000);	/* RX_BUF_OF, PA_INIT */
		hwr(u, HCI_ERROR_EN_N_LAYER, DFES_ERR_EN | 0x7);
		hwr(u, HCI_ERROR_EN_T_LAYER, DFES_ERR_EN | 0x1 | 0x2 | 0x4 | 0x10);
		uwr(u, UNIP_PA_DBG_OPTION_SUITE_1, DBG_SUITE1_ENABLE);
		uwr(u, UNIP_PA_DBG_OPTION_SUITE_2, DBG_SUITE2_ENABLE);
		u->cal.mclk_rate = u->mclk;
		u->cal.available_lane = u->lanes;
		ret = call_cal(u, "pre_link", ufs_cal_pre_link);
		if (ret)
			return ret;
		ret = uic(u, UIC_LINK_STARTUP, 0, 0, "DME_LINKSTARTUP");
		if (!ret)
			break;
		dump(u, "link start-up failed");
	}
	if (ret)
		return ret;
	if (!(rd(u, UFSHC_HCS) & HCS_DP)) {
		dump(u, "link up, but no device present");
		return -ENXIO;
	}
	ctx = urd(u, UNIP_PA_CONNECTEDTXDATALANES);
	crx = urd(u, UNIP_PA_CONNECTEDRXDATALANES);
	maxg = urd(u, UNIP_PA_MAXRXHSGEAR);
	if (!crx || crx != ctx)
		printf(P "asymmetric connected lanes: rx %u tx %u\n", crx, ctx);
	u->cal.connected_tx_lane = crx;
	u->cal.connected_rx_lane = crx;
	u->cal.active_tx_lane = crx;
	u->cal.active_rx_lane = crx;
	u->cal.max_gear = clamp_t(u32, maxg, GEAR_1, GEAR_4);
	ret = call_cal(u, "post_link", ufs_cal_post_link);
	if (ret)
		return ret;
	rd(u, UFSHC_UECPA);			/* (cleared once: the line reset during start-up) */

	/* operational: the lists' bases, then their run bits (ufshcd_make_hba_operational) */
	wr(u, UFSHC_UTRLBA, (u32)(ulong)u->list);
	wr(u, UFSHC_UTRLBAU, (u32)((u64)(ulong)u->list >> 32));
	wr(u, UFSHC_UTMRLBA, (u32)(ulong)u->tmlist);
	wr(u, UFSHC_UTMRLBAU, (u32)((u64)(ulong)u->tmlist >> 32));
	if ((rd(u, UFSHC_HCS) & (HCS_UTRLRDY | HCS_UTMRLRDY | HCS_UCRDY)) != (HCS_UTRLRDY | HCS_UTMRLRDY | HCS_UCRDY)) {
		dump(u, "the lists are not ready");
		return -EIO;
	}
	wr(u, UFSHC_UTMRLRSR, 1);
	wr(u, UFSHC_UTRLRSR, 1);
	printf(P "link up: %u of %u lanes connected, the device's max HS gear %u (PWM-G1 for now)\n", crx, u->lanes, maxg);
	return 0;
}

/* 5. the device: ufshcd_verify_dev_init and ufshcd_complete_dev_init */
static int device_init(struct zuma_ufs *u)
{
	static const char *const ref[] = { "19.2", "26", "38.4", "52" };
	ulong start;
	u32 v = 0;
	int ret = -EIO, t;

	for (t = 0; t < 50; t++) {
		ret = dev_cmd(u, UPIU_NOP_OUT, 0, 0, 0, NULL, 100000);
		if (!ret)
			break;
	}
	if (ret) {
		dump(u, "no NOP IN from the device");
		return ret;
	}
	printf(P "the device answers (NOP IN after %d tries)\n", t + 1);
	ret = dev_cmd(u, UPIU_QUERY, QUERY_WRITE, QOP_SET_FLAG, FLAG_DEVICE_INIT, NULL, 500000);
	if (ret)
		return ret;
	start = timer_get_us();
	do {
		ret = dev_cmd(u, UPIU_QUERY, QUERY_READ, QOP_READ_FLAG, FLAG_DEVICE_INIT, &v, 500000);
		if (ret)
			return ret;
		if (!(v & 1))
			break;
		mdelay(5);
	} while (timer_get_us() - start < 3000000);
	if (v & 1) {
		printf(P "fDeviceInit not cleared by the device in 3 s\n");
		return -ETIMEDOUT;
	}
	printf(P "device initialised in %lu ms\n", (timer_get_us() - start) / 1000);
	if (!dev_cmd(u, UPIU_QUERY, QUERY_READ, QOP_READ_ATTR, ATTR_REF_CLK_FREQ, &v, 500000))
		printf(P "the device's reference clock: %s MHz (bRefClkFreq %u; the PHY's tables are for 38.4)\n",
		       v < 4 ? ref[v] : "?", v);
	return 0;
}

/* each logical unit's power-on UNIT ATTENTION collected, so the SCSI scan's first commands do not meet it */
static void unit_attentions(struct udevice *dev)
{
	for (int lun = 0; lun < 8; lun++)
		for (int t = 0; t < 2; t++) {
			struct scsi_cmd c = { 0 };

			c.lun = lun;
			c.cmdlen = 6;
			c.dma_dir = DMA_NONE;
			if (!zuma_ufs_exec(dev, &c))
				break;
		}
}

/* LUN 0's block 0 read, timed */
static int test_read(struct udevice *dev, const char *mode)
{
	static u8 buf[BLOCK] __aligned(ARCH_DMA_MINALIGN);
	struct scsi_cmd c = { 0 };
	ulong start = timer_get_us();
	int ret;

	c.cmd[0] = 0x28;		/* READ(10), block 0, 1 block */
	c.cmd[8] = 1;
	c.cmdlen = 10;
	c.dma_dir = DMA_FROM_DEVICE;
	c.datalen = BLOCK;
	c.pdata = buf;
	ret = zuma_ufs_exec(dev, &c);
	if (ret)
		printf(P "%s: LUN 0 block 0 NOT read (%d, status %#x, sense key %#x asc %#x/%#x)\n", mode, ret, c.status,
		       c.sense_buf[2] & 0xf, c.sense_buf[12], c.sense_buf[13]);
	else
		printf(P "%s: LUN 0 block 0 read in %lu us: crc32 %08x, bytes 0x1fe %02x %02x\n", mode,
		       timer_get_us() - start, crc32(0, buf, BLOCK), buf[0x1fe], buf[0x1ff]);
	return ret;
}

static int zuma_ufs_probe(struct udevice *dev)
{
	struct zuma_ufs *u = dev_get_priv(dev);
	struct scsi_plat *plat = dev_get_uclass_plat(dev);
	ulong envm = env_get_ulong("hakios_ufs_mclk", 10, 0);
	bool pwm_ok;
	u32 bl;
	int ret;

	u->base = dev_read_addr_ptr(dev);
	if (!u->base)
		return -EINVAL;
	u->h.std = u->base;
	u->h.hci = (void *)UFS_HCI;
	u->h.ufsp = (void *)UFS_UFSP;
	u->h.unipro = (void *)UFS_UNIPRO;
	u->h.pma = (void *)UFS_PMA;
	u->h.cport = (void *)UFS_CPORT;
	u->h.udelay = cal_udelay;

	/* the UniPro clock: as the bootloader set the controller's 1 us count from it, unless $hakios_ufs_mclk says (Hz) */
	bl = hrd(u, HCI_1US_TO_CNT_VAL) & 0x3ff;
	u->mclk = envm ? envm : bl ? bl * 1000000 : 177750000;
	dump(u, "as the bootloader left it");
	printf(P "UniPro clock %u Hz (%s; the bootloader's 1 us count %u)\n", u->mclk,
	       envm ? "$hakios_ufs_mclk" : bl ? "from the bootloader's count" : "a guess", bl);

	{
		phys_addr_t dma = DMA_BELOW;

		if (lmb_alloc_mem(LMB_MEM_ALLOC_MAX, SZ_64K, &dma, DMA_SIZE, LMB_NONE) || dma + DMA_SIZE > DMA_BELOW) {
			printf(P "no %u KB of memory below %#lx for the controller\n", DMA_SIZE / 1024, DMA_BELOW);
			return -ENOMEM;
		}
		u->list = (u32 *)(ulong)dma;
		u->tmlist = (u32 *)(ulong)(dma + SZ_1K);
		u->ucd = (u32 *)(ulong)(dma + SZ_4K);
		u->data = (u8 *)(ulong)(dma + SZ_1M);
	}
	memset(u->list, 0, SZ_4K);
	dma_flush(u->list, SZ_4K);

	/* Samsung's CAL, as Google's driver fills it in for this phone: the embedded host's tables, board 1 (the tree's
	 * brd-for-cal), EVT0 (the tree has no evt-ver), no auto-Hibern8 */
	u->cal.handle = &u->h;
	u->cal.tbl = HOST_EMBD;
	u->cal.board = 1;
	u->cal.evt_ver = 0;
	u->cal.support_ah8_cal = 0;
	ufs_cal_init(&u->cal, 0);

	ret = externals(u);
	if (ret)
		return ret;
	if (rd(u, UFSHC_HCE) & 1) {		/* the bootloader's controller stopped before the device is powered */
		wr(u, UFSHC_HCE, 0);
		wait_bits(u, UFSHC_HCE, 1, 0, 10000);
	}
	power_device(u);
	ret = link_up(u);
	if (ret)
		return ret;
	ret = device_init(u);
	if (ret)
		return ret;
	u->up = true;
	zuma_ufs_up = u;
	unit_attentions(dev);

	/*
	 * One block read before the SCSI scan, to see the data path on its own: in PWM-G1, and in HS-G4 when $hakios_ufs_hs
	 * is 1 or PWM-G1 failed (the controller and the device brought up again first, as after any failed request).
	 */
	pwm_ok = !test_read(dev, "PWM-G1");
	if (!pwm_ok) {
		u->up = false;
		u->broken = false;
		ret = link_up(u);
		if (!ret)
			ret = device_init(u);
		if (ret)
			return ret;
		u->up = true;
		unit_attentions(dev);
	}
	if (!pwm_ok || env_get_yesno("hakios_ufs_hs") == 1) {
		if (!hk_zuma_ufs_hs())
			test_read(dev, "HS-G4");
	}

	plat->max_id = 1;
	plat->max_lun = 8;
	plat->max_bytes_per_req = XFER_MAX;
	printf(P "up from cold (UFSHCI %u.%u, list at %p), read only\n", (rd(u, UFSHC_VER) >> 8) & 0xf,
	       (rd(u, UFSHC_VER) >> 4) & 0xf, u->list);
	return 0;
}

/* "zuma ufs hs": the link to HS-G4 rate B on every connected lane (Google's pwr_change_notify around Linux's
 * ufshcd_change_power_mode; Exynos skips the core's default UniPro timeouts, the CAL's calib tables write its own) */
int hk_zuma_ufs_hs(void)
{
	struct zuma_ufs *u = zuma_ufs_up;
	u32 lanes, gear, atx, arx;
	int ret;

	if (!u || !u->up) {
		printf(P "not up (scsi scan first)\n");
		return -ENODEV;
	}
	lanes = u->cal.connected_rx_lane;
	gear = u->cal.max_gear;
	u->pmd.lane = lanes;
	u->pmd.gear = gear;
	u->pmd.mode = 1;		/* FAST_MODE */
	u->pmd.hs_series = 2;		/* rate B */
	u->cal.pmd = &u->pmd;
	ret = call_cal(u, "pre_pmc", ufs_cal_pre_pmc);
	if (ret)
		return ret;
	if (dme_set(u, PA_RXGEAR, gear, "PA_RxGear") || dme_set(u, PA_ACTIVERXDATALANES, lanes, "PA_ActiveRxDataLanes") ||
	    dme_set(u, PA_RXTERMINATION, 1, "PA_RxTermination") || dme_set(u, PA_TXGEAR, gear, "PA_TxGear") ||
	    dme_set(u, PA_ACTIVETXDATALANES, lanes, "PA_ActiveTxDataLanes") ||
	    dme_set(u, PA_TXTERMINATION, 1, "PA_TxTermination") || dme_set(u, PA_HSSERIES, 2, "PA_HSSeries"))
		return -EIO;
	wr(u, UFSHC_IS, IS_UPMS);
	ret = dme_set(u, PA_PWRMODE, 1 << 4 | 1, "PA_PWRMode");
	if (ret)
		return ret;
	if (wait_bits(u, UFSHC_IS, IS_UPMS, IS_UPMS, 500000) || HCS_UPMCRS(rd(u, UFSHC_HCS)) != UPMCRS_PWR_LOCAL) {
		dump(u, "the power mode change did not complete");
		return -EIO;
	}
	wr(u, UFSHC_IS, IS_UPMS);
	atx = urd(u, UNIP_PA_ACTIVETXDATALANES);
	arx = urd(u, UNIP_PA_ACTIVERXDATALANES);
	u->cal.active_tx_lane = atx;
	u->cal.active_rx_lane = arx;
	ret = call_cal(u, "post_pmc", ufs_cal_post_pmc);
	if (ret)
		return ret;
	u->hs = true;
	printf(P "HS-G%u rate B, %u/%u lanes active\n", gear, arx, atx);
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
