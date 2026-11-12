/*
 * The Exynos 8890's USB port, the SoC's half for the device-neutral gadget driver (drivers/core/hakios.dwc3.c, which owns
 * the boot-args: usb_gadget=N usb_gadget_soc=exynos8890). This file is the glue (hakios.usb_glue.h).
 *
 * The hardware (Samsung's device tree, exynos8890.dtsi): the DWC3 (USB 3.0 DRD) at 0x15400000, interrupt SPI 213 (the
 * gadget driver polls); its PHY control ("USBCON", Samsung's USB3 CAL version 0x0101, "JF 3.0") at 0x15500000. Power:
 * the PMU's USBDEV_PHY_CONTROL (0x105C0000 + 0x704) bit 0 takes the PHY out of isolation. Clocks: CMU_TOP's
 * SCLK_FSYS0_USBDRD30 (mux +0x30C, divider +0x46C, enable +0x918 at 0x10570000) into CMU_FSYS0 (0x10E90000): the user
 * muxes of the bus clock (+0x200), the controller's clock (+0x204) and the PHY's two clocks back (+0x218 UTMI 60 MHz,
 * +0x21C PIPE 125 MHz), select in bit 12; the gates of ACLK_FSYS0_200 (+0x800, manual mode +0x1800: bits 18 the
 * controller, 12 its AXI, 9 its AHB bridge) and of the four USB clocks (+0x840 suspend, +0x84C ref, +0x850 UTMI, +0x854
 * PIPE; manual +0x1840...); the Q-channels +0x201C (controller) and +0x2040 (PHY control).
 *
 * What S-Boot leaves (step 1, seen 8 November 2026): every one of those clocks running in automatic gating, the user
 * muxes on their sources; the PHY isolated; USBCON at what look like reset values; the DWC3 (2.80a) halted in device
 * mode with no event buffer. So no clock is touched here.
 *
 * THE SEQUENCE (step 2), each register read back and said; Samsung's order (phy_power_on, then phy_init's
 * samsung_exynos_cal_usb3phy_enable, then phy_tune's _tune_dev) with the 8890's values (version 0x0101, reference
 * USBPHY_REFSEL_DIFF_INTERNAL from the 26 MHz crystal, SSC on, range 0, use_io_for_ovc):
 *   a  PMU +0x704 bit 0 set: the PHY out of isolation
 *   b  PHYRESUME: the Q-channel workaround (its other sources off, FORCE_QACT clear, 500 us, then set)
 *   c  PHYCLKRST with PORTRESET held: reference select and FSEL (the 26 MHz differential value 0x82: FSEL 2, MPLL
 *      multiplier 0x60, REF_CLKDIV2 clear), COMMONONN and EN_UTMISUSPEND clear, RETENABLEN, REF_SSP_EN and SSC_EN set;
 *      PHYPARAM0 REF_USE_PAD clear; PHYREG0 SSC_REFCLKSEL 0x108, SSC range 0; PHYPIPE PHY_CLOCK_SEL (UTMI from the
 *      free clock); 10 us, then PORTRESET released
 *   d  PHYTEST: POWERDOWN_HSP and POWERDOWN_SSP clear (both PHYs powered, as Samsung's kernel does); 500 us
 *   e  VBUS forced valid to the link (LINKSYSTEM FORCE_BVALID, FORCE_VBUSVALID) and the PHY (PHYUTMI VBUSVLDEXTSEL,
 *      VBUSVLDEXT): Samsung's path for a board whose VBUS pad is not used. The Note FE's is (no is_not_vbus_pad), but
 *      nothing here watches VBUS: the log link is only ever run with a host attached
 *   f  PHYUTMI: FORCESLEEP, FORCESUSPEND, DP/DM pull-downs and DRVVBUS clear, OTGDISABLE set
 *   g  LINKPORT: the over-current inputs from IO (OVCR_U3_SEL, OVCR_U2_SEL clear)
 *   h  _tune_dev: LINKSYSTEM FLADJ 0x20 and XHCI_VERSION_CONTROL; PHYPARAM0's high-speed fields (TXVREF 0xB, TXRISE 3,
 *      TXRES 2, TXPREEMPAMP 3, TXPREEMPPULSE 0, TXHSXV 0, TXFSLS 3, SQRX 7, OTG 4, COMPDIS 0; use_additional_tuning
 *      changes nothing in device mode)
 * Not done: the SuperSpeed tuning (PHYPARAM1/2, PCSVAL, late_enable's CR port and fixed RX EQ) and OTP tuning: the link
 * runs at high speed (the gadget driver's DCFG).
 *
 * Sources: Samsung's kernel for the 8890 (LineageOS android_kernel_samsung_universal8890): arch/arm64/boot/dts/
 * exynos8890.dtsi (usb@15400000, phy@15500000), drivers/phy/phy-exynos-usbdrd.c and .h (USBDEV_PHY_CONTROL, version,
 * reference clock), drivers/phy/phy-samsung-usb3-cal.c and .h (the sequence and registers), drivers/soc/samsung/pwrcal/
 * S5E8890/S5E8890-cmusfr.h, -cmu.c, -vclk.c (the clocks). GPL-2.0: read for facts, no code taken.
 *
 * Status: run on the phone, 8 November 2026 (usb_gadget=5 with max77854_usbsw=1): the PHY up, the host (macOS)
 * enumerates the gadget and the kernel's log streams to /dev/cu.usbmodem* for the whole run.
 */
#include <stdint.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/systm.h>

#include "hakios.usb_glue.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);

#define DWC3_PA                 0x15400000u
#define DWC3_SIZE               0x10000u
#define USBCON_PA               0x15500000u
#define PMU_PA                  0x105C0000u
#define PMU_USBDEV_PHY          0x0704u         /* bit 0: out of isolation */
#define CMU_TOP_PA              0x10570000u
#define TOP_MUX_USBDRD30        0x030Cu
#define TOP_DIV_USBDRD30        0x046Cu
#define TOP_ENABLE_USBDRD30     0x0918u
#define CMU_FSYS0_PA            0x10E90000u
#define FSYS0_MUX_ACLK_200      0x0200u
#define FSYS0_MUX_USBDRD30      0x0204u
#define FSYS0_MUX_UTMI          0x0218u
#define FSYS0_MUX_PIPE          0x021Cu
#define  MUX_SEL                (1u << 12)
#define FSYS0_CG_ACLK_200       0x0800u
#define  CG_ACLK_USBDRD30       (1u << 18)
#define  CG_ACLK_AXI_USBDRD30   (1u << 12)
#define  CG_ACLK_AHB_USBDRD30   (1u << 9)
#define FSYS0_CG_SUSPEND        0x0840u
#define FSYS0_CG_REF            0x084Cu
#define FSYS0_CG_UTMI           0x0850u
#define FSYS0_CG_PIPE           0x0854u
#define FSYS0_CG_MAN            0x1000u         /* each CG_CTRL_VAL's manual-mode twin */
#define FSYS0_QCH_USBDRD30      0x201Cu
#define FSYS0_QCH_PHYCTRL       0x2040u

/* USBCON (phy-samsung-usb3-cal.h) */
#define LINKSYSTEM              0x04u
#define  LS_XHCI_VERSION_CONTROL (1u << 27)
#define  LS_FORCE_VBUSVALID     (1u << 8)
#define  LS_FORCE_BVALID        (1u << 7)
#define  LS_FLADJ_MASK          (0x3Fu << 1)
#define  LS_FLADJ(x)            ((x) << 1)
#define PHYUTMI                 0x08u
#define  UTMI_VBUSVLDEXTSEL     (1u << 10)
#define  UTMI_VBUSVLDEXT        (1u << 9)
#define  UTMI_OTGDISABLE        (1u << 6)
#define  UTMI_DRVVBUS           (1u << 4)
#define  UTMI_DPPULLDOWN        (1u << 3)
#define  UTMI_DMPULLDOWN        (1u << 2)
#define  UTMI_FORCESUSPEND      (1u << 1)
#define  UTMI_FORCESLEEP        (1u << 0)
#define PHYPIPE                 0x0Cu
#define  PIPE_PHY_CLOCK_SEL     (1u << 4)
#define PHYCLKRST               0x10u
#define  CR_EN_UTMISUSPEND      (1u << 31)
#define  CR_SSC_EN              (1u << 20)
#define  CR_REF_SSP_EN          (1u << 19)
#define  CR_REF_CLKDIV2         (1u << 18)
#define  CR_MPLL_MULT_MASK      (0x7Fu << 11)
#define  CR_MPLL_MULT(x)        ((x) << 11)
#define  CR_FSEL_MASK           (0x3Fu << 5)
#define  CR_FSEL(x)             ((x) << 5)
#define  CR_RETENABLEN          (1u << 4)
#define  CR_REFCLKSEL_MASK      (0x3u << 2)
#define  CR_PORTRESET           (1u << 1)
#define  CR_COMMONONN           (1u << 0)
#define PHYREG0                 0x14u
#define  REG0_SSC_REFCLKSEL_MASK (0x1FFu << 23)
#define  REG0_SSC_REFCLKSEL(x)  ((x) << 23)
#define  REG0_SSC_RANGE_MASK    (0x7u << 20)
#define PHYPARAM0               0x1Cu
#define  P0_REF_USE_PAD         (1u << 31)
#define  P0_HS_TUNE_MASK        0x03FFFFFFu     /* TXVREF [25:22] ... COMPDIS [2:0] */
#define PHYTEST                 0x28u
#define  TEST_POWERDOWN_SSP     (1u << 3)
#define  TEST_POWERDOWN_HSP     (1u << 2)
#define PHYRESUME               0x34u
#define  RES_DIS_LINKGATE_QACT  (1u << 13)
#define  RES_DIS_ID0_QACT       (1u << 12)
#define  RES_DIS_VBUSVALID_QACT (1u << 11)
#define  RES_DIS_BVALID_QACT    (1u << 10)
#define  RES_FORCE_QACT         (1u << 9)
#define LINKPORT                0x44u
#define  LP_OVCR_U3_SEL         (1u << 3)
#define  LP_OVCR_U2_SEL         (1u << 2)

#define REFSEL_DIFF_INTERNAL    4u              /* Samsung's value, shifted into REFCLKSEL as its CAL does */
#define FSEL_DIFF_26MHZ         0x02u           /* USBPHY_REFCLK_DIFF_26MHZ (0x82) & 0x3F */
/* the 8890's high-speed tuning (exynos_usbdrd_fill_hstune), as PHYPARAM0's fields */
#define HS_TUNE                 ((0xBu << 22) | (3u << 20) | (2u << 18) | (3u << 15) | (0u << 13) | (3u << 9) | \
	                         (7u << 6) | (4u << 3) | 0u)

static vm_offset_t usbcon, pmu, top, fsys0;
static bool mapped;

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* one USBCON register: set and clear bits, read back, said */
static void
change(usb_glue_say_t say, const char *step, const char *name, uint32_t off, uint32_t set, uint32_t clear)
{
	uint32_t before = rd(usbcon, off), want = (before & ~clear) | set;

	wr(usbcon, off, want);
	uint32_t after = rd(usbcon, off);
	say("usb: exynos8890: 2%s: %s %08x -> %08x%s\n", step, name, before, after, after == want ? "" : " (NOT as written)");
}

static bool
map_all(usb_glue_say_t say)
{
	if (mapped) {
		return true;
	}
	pmu = ml_io_map(PMU_PA, 0x1000);
	top = ml_io_map(CMU_TOP_PA, 0x1000);
	fsys0 = ml_io_map(CMU_FSYS0_PA, 0x3000);
	usbcon = ml_io_map(USBCON_PA, 0x1000);
	if (pmu == 0 || top == 0 || fsys0 == 0 || usbcon == 0) {
		say("usb: exynos8890: could not map the PMU, CMU_TOP, CMU_FSYS0 or USBCON\n");
		return false;
	}
	mapped = true;
	return true;
}

static void
say_usbcon(usb_glue_say_t say, const char *when)
{
	say("usb: exynos8890: USBCON %s: LINKSYSTEM %08x PHYUTMI %08x PHYPIPE %08x PHYCLKRST %08x PHYREG0 %08x\n", when,
	    rd(usbcon, LINKSYSTEM), rd(usbcon, PHYUTMI), rd(usbcon, PHYPIPE), rd(usbcon, PHYCLKRST), rd(usbcon, PHYREG0));
	say("usb: exynos8890: USBCON %s: PHYPARAM0 %08x PHYTEST %08x PHYRESUME %08x\n", when, rd(usbcon, PHYPARAM0),
	    rd(usbcon, PHYTEST), rd(usbcon, PHYRESUME));
}

/* the clocks as the boot left them; true when the controller's bus clock is not shut (its registers can be read) */
static bool
say_clocks(usb_glue_say_t say)
{
	uint32_t cg = rd(fsys0, FSYS0_CG_ACLK_200), man = rd(fsys0, FSYS0_CG_ACLK_200 + FSYS0_CG_MAN);
	bool shut = (man & CG_ACLK_USBDRD30) && !(cg & CG_ACLK_USBDRD30);

	say("usb: exynos8890: CMU_TOP SCLK_FSYS0_USBDRD30: mux %08x div %08x enable %08x\n", rd(top, TOP_MUX_USBDRD30),
	    rd(top, TOP_DIV_USBDRD30), rd(top, TOP_ENABLE_USBDRD30));
	say("usb: exynos8890: CMU_FSYS0 user muxes: ACLK_200 %08x USBDRD30 %08x UTMI %08x PIPE %08x (bit 12: 1 the source, 0 OSCCLK)\n",
	    rd(fsys0, FSYS0_MUX_ACLK_200), rd(fsys0, FSYS0_MUX_USBDRD30), rd(fsys0, FSYS0_MUX_UTMI), rd(fsys0, FSYS0_MUX_PIPE));
	say("usb: exynos8890: CMU_FSYS0 ACLK_200 gates %08x manual %08x (controller %u/%u, AXI %u/%u, AHB %u/%u: on/manual)%s\n",
	    cg, man, (cg & CG_ACLK_USBDRD30) ? 1 : 0, (man & CG_ACLK_USBDRD30) ? 1 : 0, (cg & CG_ACLK_AXI_USBDRD30) ? 1 : 0,
	    (man & CG_ACLK_AXI_USBDRD30) ? 1 : 0, (cg & CG_ACLK_AHB_USBDRD30) ? 1 : 0, (man & CG_ACLK_AHB_USBDRD30) ? 1 : 0,
	    shut ? ": the controller's SHUT" : "");
	say("usb: exynos8890: CMU_FSYS0 gates (value/manual): suspend %x/%x ref %x/%x UTMI %x/%x PIPE %x/%x; QCH %08x PHYCTRL QCH %08x\n",
	    rd(fsys0, FSYS0_CG_SUSPEND), rd(fsys0, FSYS0_CG_SUSPEND + FSYS0_CG_MAN), rd(fsys0, FSYS0_CG_REF),
	    rd(fsys0, FSYS0_CG_REF + FSYS0_CG_MAN), rd(fsys0, FSYS0_CG_UTMI), rd(fsys0, FSYS0_CG_UTMI + FSYS0_CG_MAN),
	    rd(fsys0, FSYS0_CG_PIPE), rd(fsys0, FSYS0_CG_PIPE + FSYS0_CG_MAN), rd(fsys0, FSYS0_QCH_USBDRD30),
	    rd(fsys0, FSYS0_QCH_PHYCTRL));
	return !shut;
}

static bool
exynos8890_look(usb_glue_say_t say, __unused uint32_t flags)
{
	if (!map_all(say)) {
		return false;
	}
	uint32_t iso = rd(pmu, PMU_USBDEV_PHY);

	say("usb: exynos8890: PMU USBDEV_PHY_CONTROL %08x (%s)\n", iso, (iso & 1) ? "out of isolation" : "isolated");
	if (!say_clocks(say)) {
		say("usb: exynos8890: the controller's bus clock is shut: USBCON and the DWC3 are not read\n");
		return false;
	}
	say_usbcon(say, "as the boot left it");
	return true;
}

static int
exynos8890_phy_on(usb_glue_say_t say, __unused uint32_t flags)
{
	if (!map_all(say)) {
		return ENXIO;
	}
	say_usbcon(say, "before");

	uint32_t iso = rd(pmu, PMU_USBDEV_PHY);                                                /* a */
	wr(pmu, PMU_USBDEV_PHY, iso | 1u);
	say("usb: exynos8890: 2a: PMU +0x704 %08x -> %08x (the PHY out of isolation)\n", iso, rd(pmu, PMU_USBDEV_PHY));

	uint32_t v = rd(usbcon, PHYRESUME);                                                    /* b */
	v |= RES_DIS_ID0_QACT | RES_DIS_VBUSVALID_QACT | RES_DIS_BVALID_QACT | RES_DIS_LINKGATE_QACT;
	v &= ~RES_FORCE_QACT;
	delay(500);
	wr(usbcon, PHYRESUME, v);
	delay(500);
	v = rd(usbcon, PHYRESUME) | RES_FORCE_QACT;
	delay(500);
	wr(usbcon, PHYRESUME, v);
	say("usb: exynos8890: 2b: PHYRESUME %08x (Q-channel: QACT forced, its other sources off)\n", rd(usbcon, PHYRESUME));

	uint32_t clkrst = rd(usbcon, PHYCLKRST);                                               /* c */
	clkrst |= CR_PORTRESET;
	clkrst = (clkrst & ~CR_REFCLKSEL_MASK) | (REFSEL_DIFF_INTERNAL << 2);
	clkrst = (clkrst & ~CR_FSEL_MASK) | CR_FSEL(FSEL_DIFF_26MHZ);
	clkrst &= ~(CR_COMMONONN | CR_EN_UTMISUSPEND | CR_REF_CLKDIV2);
	clkrst |= CR_RETENABLEN | CR_REF_SSP_EN | CR_SSC_EN;
	clkrst = (clkrst & ~CR_MPLL_MULT_MASK) | CR_MPLL_MULT(0x60u);
	change(say, "c", "PHYPARAM0", PHYPARAM0, 0, P0_REF_USE_PAD);
	uint32_t reg0 = rd(usbcon, PHYREG0);
	reg0 = (reg0 & ~(REG0_SSC_REFCLKSEL_MASK | REG0_SSC_RANGE_MASK)) | REG0_SSC_REFCLKSEL(0x108u);
	change(say, "c", "PHYPIPE", PHYPIPE, PIPE_PHY_CLOCK_SEL, 0);
	change(say, "c", "PHYREG0", PHYREG0, reg0, ~reg0);
	change(say, "c", "PHYCLKRST", PHYCLKRST, clkrst, ~clkrst);
	delay(10);
	change(say, "c", "PHYCLKRST", PHYCLKRST, 0, CR_PORTRESET);

	change(say, "d", "PHYTEST", PHYTEST, 0, TEST_POWERDOWN_HSP | TEST_POWERDOWN_SSP);       /* d */
	delay(500);

	change(say, "e", "LINKSYSTEM", LINKSYSTEM, LS_FORCE_BVALID | LS_FORCE_VBUSVALID, 0);     /* e, f */
	change(say, "f", "PHYUTMI", PHYUTMI, UTMI_VBUSVLDEXTSEL | UTMI_VBUSVLDEXT | UTMI_OTGDISABLE,
	    UTMI_FORCESLEEP | UTMI_FORCESUSPEND | UTMI_DPPULLDOWN | UTMI_DMPULLDOWN | UTMI_DRVVBUS);
	change(say, "g", "LINKPORT", LINKPORT, 0, LP_OVCR_U3_SEL | LP_OVCR_U2_SEL);              /* g */

	change(say, "h", "LINKSYSTEM", LINKSYSTEM, LS_FLADJ(0x20u) | LS_XHCI_VERSION_CONTROL, LS_FLADJ_MASK);  /* h */
	change(say, "h", "PHYPARAM0", PHYPARAM0, HS_TUNE, P0_HS_TUNE_MASK);
	say_usbcon(say, "after");
	return 0;
}

static void
exynos8890_phy_state(char *out, unsigned size)
{
	if (!mapped) {
		snprintf(out, size, "(not mapped)");
		return;
	}
	snprintf(out, size, "PHYUTMI %08x LINKSYSTEM %08x PHYCLKRST %08x PHYTEST %08x", rd(usbcon, PHYUTMI),
	    rd(usbcon, LINKSYSTEM), rd(usbcon, PHYCLKRST), rd(usbcon, PHYTEST));
}

/* the PHYs suspended and powered down, VBUS no longer forced, isolated again (samsung_exynos_cal_usb3phy_disable, phy_power_off) */
static void
exynos8890_phy_off(void)
{
	if (!mapped) {
		return;
	}
	wr(usbcon, PHYUTMI, (rd(usbcon, PHYUTMI) | UTMI_FORCESUSPEND | UTMI_FORCESLEEP) &
	    ~(UTMI_VBUSVLDEXTSEL | UTMI_VBUSVLDEXT | UTMI_DRVVBUS));
	wr(usbcon, PHYTEST, rd(usbcon, PHYTEST) | TEST_POWERDOWN_HSP | TEST_POWERDOWN_SSP);
	wr(usbcon, LINKSYSTEM, rd(usbcon, LINKSYSTEM) & ~(LS_FORCE_BVALID | LS_FORCE_VBUSVALID));
	wr(pmu, PMU_USBDEV_PHY, rd(pmu, PMU_USBDEV_PHY) & ~1u);
}

static const struct hakios_usb_glue exynos8890_glue = {
	.name = "exynos8890",
	.dwc3_pa = DWC3_PA,
	.dwc3_size = DWC3_SIZE,
	.look = exynos8890_look,
	.phy_on = exynos8890_phy_on,
	.phy_state = exynos8890_phy_state,
	.phy_off = exynos8890_phy_off,
};

const struct hakios_usb_glue *
hakios_usb_glue_exynos8890(void)
{
	return &exynos8890_glue;
}
