/*
 * The Exynos 8890's side of the PCIe driver (drivers/wifi/hakios.exynos_pcie.c, hakios.exynos_pcie_glue.h): the Note FE's
 * channel 0 to its BCM4359. Stage 1 only so far: nothing here for the radio's way to memory.
 *
 * What is where (the phone's device tree, pcie0@157A0000, wlan and the pins they name):
 *   ELBI 0x15660000, PHY 0x15630000, PCS 0x156F0000, DBI 0x157A0000, SYSREG 0x15601044 (+0 and +0xC)
 *   config 0x1C000000 (4 KB), memory 0x1C011000 to 0x1DFFFFFF, SPI 204 (INTx)
 *   PMU 0x105C0000 +0x71C bit 0: the PHY out of isolation; +0x41C4 [3:0]: FSYS1's power, 0xF on
 *   WLAN_REG_ON gpj1-3, PERST# gpj1-2, CLKREQ# gpj1-0 (pinctrl@14CC0000, bank +0x60; pulls 2 bits a pin);
 *   WLAN_HOST_WAKE gpa0-7, PCIE_WAKE gpa3-3 (pinctrl@10580000, banks +0 and +0x60): read only
 * The clocks (pcie-clk-num 1, phy-clk-num 3), all muxes of CMU_FSYS1 (0x156E0000), bit 12 = the user's source:
 *   gate_pciewifi0: MUX_ACLK_FSYS1_200_USER (+0x200), on before the block is used
 *   pcie_wifi0_tx0, _rx0, wifi0_dig_refclk: MUX_PHYCLK_PCIE_WIFI0_*_USER (+0x230, +0x234, +0x240): to the PHY's clocks
 *   after its tables and resets, back to the oscillator before a new try
 * The gates themselves are hardware-run (CG_CTRL_VAL_ACLK_FSYS1_200 +0x800 bits 12-16 and 28, QSTATE_CTRL +0x242C,
 * +0x2434, +0x243C): said, not written. Only registers of the tree's sfr-dump-cmu-fsys1 list are read: +0x854
 * (pwrcal's CG_CTRL_VAL_SCLK_PCIE_LINK_WIFI0) is not in it, and reading it stopped the bus (run 20261110T220731).
 *
 * Sources (LineageOS android_kernel_samsung_universal8890 lineage-18.1, read for these facts, not copied):
 * drivers/pci/host/pci-exynos.c (exynos_pcie_poweron, _establish_link, _assert_phy_reset, the clocks),
 * pci-exynos8890_cal.c (the PHY's tables: 26 MHz, Gen1; no OTP tuning, the phone's defconfig has no CONFIG_EXYNOS_OTP),
 * pcie-designware.c (Gen1, L1 substates 0xD2), drivers/soc/samsung/pwrcal/S5E8890 (-cmu.c, -cmusfr.h, -vclk.c),
 * drivers/pinctrl/samsung/pinctrl-exynos.c (exynos8890_pin_banks0 and 9).
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/systm.h>

#include "hakios.exynos_pcie_glue.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#define PMU_FSYS1_PA            0x105C4000UL
#define FSYS1_STATUS            0x1C4
#define CMU_FSYS1_PA            0x156E0000UL
#define SYSREG_PA               0x15601000UL    /* channel 0's words: +0x44 and +0x50 */
#define MUX_ACLK_FSYS1_200_USER 0x200
#define MUX_USER                (1u << 12)
#define MUX_STAT                0x400           /* a mux's STAT is its CON + 0x400 */
static const uint16_t phy_muxes[] = { 0x230, 0x234, 0x240 };   /* TX0, RX0, DIG_REFCLK */

#define ELBI_L1_BUG_FIX_ENABLE  0x038
#define ELBI_PCS_G_RST          0x288
#define ELBI_PHY_RST            0x28C
#define ELBI_MAC_RST            0x290
#define ELBI_QCH_SEL            0x2C8           /* [1:0] = 3: no clock gating (the tree's pcie-changed) */

static vm_offset_t pmu_fsys1, cmu, sysreg;

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static bool
look(__unused bool ungate)
{
	pmu_fsys1 = ml_io_map(PMU_FSYS1_PA, 0x1000);
	cmu = ml_io_map(CMU_FSYS1_PA, 0x3000);
	sysreg = ml_io_map(SYSREG_PA, 0x1000);
	if (!pmu_fsys1 || !cmu || !sysreg) {
		return false;
	}
	uint32_t status = rd(pmu_fsys1, FSYS1_STATUS);

	say("exynos_pcie: 1: PMU FSYS1_STATUS %08x (0xF: powered)\n", status);
	if ((status & 0xf) != 0xf) {
		say("exynos_pcie: 1: FSYS1 is not powered: CMU_FSYS1 and the controller are not read\n");
		return false;
	}
	say("exynos_pcie: 1: CMU_FSYS1: MUX_ACLK_FSYS1_200_USER %08x (STAT %08x); CG_CTRL ACLK_FSYS1_200 VAL %08x MAN %08x; "
	    "PCLK_COMBO_PHY_WIFI %08x\n", rd(cmu, MUX_ACLK_FSYS1_200_USER),
	    rd(cmu, MUX_ACLK_FSYS1_200_USER + MUX_STAT), rd(cmu, 0x800), rd(cmu, 0x1800), rd(cmu, 0x824));
	say("exynos_pcie: 1: the PHY's clock muxes TX0 %08x RX0 %08x DIG_REFCLK %08x; QSTATE_CTRL RC_LINK %08x PCS %08x PHY %08x\n",
	    rd(cmu, phy_muxes[0]), rd(cmu, phy_muxes[1]), rd(cmu, phy_muxes[2]), rd(cmu, 0x242C), rd(cmu, 0x2434),
	    rd(cmu, 0x243C));
	return true;
}

/* exynos_pcie_clock_enable(), then exynos_pcie_poweron()'s "phy all power down clear" */
static void
power_up(const struct hakios_pcie_regs *r)
{
	uint32_t before = rd(cmu, MUX_ACLK_FSYS1_200_USER);

	wr(cmu, MUX_ACLK_FSYS1_200_USER, before | MUX_USER);
	delay(100);
	say("exynos_pcie: 3: gate_pciewifi0: MUX_ACLK_FSYS1_200_USER %08x -> %08x (STAT %08x)\n", before,
	    rd(cmu, MUX_ACLK_FSYS1_200_USER), rd(cmu, MUX_ACLK_FSYS1_200_USER + MUX_STAT));
	wr(r->phy, 0x15 * 4, rd(r->phy, 0x15 * 4) & ~(0xfu << 3));
	wr(r->phy, 0x4E * 4, 0);
	wr(r->phy, 0x4F * 4, 0);
}

static void
pulse(vm_offset_t elbi, uint32_t off)
{
	wr(elbi, off, 1);
	delay(10);
	wr(elbi, off, 0);
	delay(10);
	wr(elbi, off, 1);
	delay(10);
}

/* the common block and the transceiver (from word 0x30), a word a byte */
static const uint8_t phy_cmn[26] = {
	0x01, 0x0F, 0xA6, 0x31, 0x90, 0x62, 0x20, 0x00, 0x00, 0xA7, 0x0A, 0x37, 0x20, 0x08, 0xEF, 0xFC, 0x96, 0x14, 0x00, 0x10,
	0x60, 0x01, 0x00, 0x00, 0x04, 0x10,
};
static const uint8_t phy_trsv[41] = {
	0x31, 0xF4, 0xF4, 0x80, 0x25, 0x40, 0xD8, 0x03, 0x35, 0x55, 0x4C, 0xC3, 0x10, 0x54, 0x70, 0xC5, 0x00, 0x2F, 0x38, 0xA4,
	0x00, 0x3B, 0x30, 0x9A, 0x64, 0x00, 0x1F, 0x83, 0x1B, 0x01, 0xE0, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1D,
	0x00,
};

/* exynos_pcie_establish_link()'s Q-channel choice, exynos_pcie_phy_config(), exynos_pcie_phy_clock_enable() */
static void
phy_config(const struct hakios_pcie_regs *r)
{
	uint32_t v;

	for (unsigned i = 0; i < 3; i++) {              /* a new try: the PHY's clocks back to the oscillator first */
		wr(cmu, phy_muxes[i], rd(cmu, phy_muxes[i]) & ~MUX_USER);
	}
	wr(r->elbi, ELBI_QCH_SEL, (rd(r->elbi, ELBI_QCH_SEL) & ~3u) | 3);

	wr(sysreg, 0x44, rd(sysreg, 0x44) & ~(1u << 1));
	v = rd(sysreg, 0x50);
	v = ((v & ~(0xfu << 4) & ~(0xfu << 2)) | (0x3u << 2)) & ~(1u << 1);
	wr(sysreg, 0x50, v);

	pulse(r->elbi, ELBI_PCS_G_RST);
	for (unsigned i = 0; i < sizeof(phy_cmn); i++) {
		wr(r->phy, 4 * i, phy_cmn[i]);
	}
	for (unsigned i = 0; i < sizeof(phy_trsv); i++) {
		wr(r->phy, 4 * (0x30 + i), phy_trsv[i]);
	}
	wr(r->phy, 0x5C * 4, 0x14);                     /* tx amplitude */
	wr(r->pcs, 0xF8, 0x70);                         /* tx latency */
	wr(r->pcs, 0x0C, rd(r->pcs, 0x0C) | (1u << 4)); /* PRGM_TIMEOUT_L1SS_VAL */
	pulse(r->elbi, ELBI_MAC_RST);
	pulse(r->elbi, ELBI_PHY_RST);

	for (unsigned i = 0; i < 3; i++) {
		wr(cmu, phy_muxes[i], rd(cmu, phy_muxes[i]) | MUX_USER);
	}
	delay(100);
	say("exynos_pcie: 3: the PHY's clocks: TX0 %08x (STAT %08x) RX0 %08x (%08x) DIG_REFCLK %08x (%08x)\n",
	    rd(cmu, phy_muxes[0]), rd(cmu, phy_muxes[0] + MUX_STAT), rd(cmu, phy_muxes[1]), rd(cmu, phy_muxes[1] + MUX_STAT),
	    rd(cmu, phy_muxes[2]), rd(cmu, phy_muxes[2] + MUX_STAT));
	wr(r->elbi, ELBI_L1_BUG_FIX_ENABLE, 1);
}

static const struct hakios_pcie_glue glue = {
	.name = "exynos8890",
	.radio = "BCM4359",
	.elbi_pa = 0x15660000, .phy_pa = 0x15630000, .pcs_pa = 0x156F0000, .dbi_pa = 0x157A0000,
	.cfg_pa = 0x1C000000,
	.mem_pa = 0x1C011000, .mem_size = 0x01FEF000,
	.pmu_pa = 0x105C0000, .pmu_phy_control = 0x71C,
	.intx_spi = 204,
	.max_speed = 1,
	.l1_substates = 0xD2,
	.pud_bits = 2,
	.pins = {
		[HAKIOS_PCIE_WLAN_EN] = { "gpj1-3", 0x14CC0000, 0x060, 3 },
		[HAKIOS_PCIE_PERST] = { "gpj1-2", 0x14CC0000, 0x060, 2 },
		[HAKIOS_PCIE_CLKREQ] = { "gpj1-0", 0x14CC0000, 0x060, 0 },
		[HAKIOS_PCIE_HOST_WAKE] = { "gpa0-7", 0x10580000, 0x000, 7 },
		[HAKIOS_PCIE_WAKE] = { "gpa3-3", 0x10580000, 0x060, 3 },
	},
	.look = look,
	.power_up = power_up,
	.phy_config = phy_config,
};

const struct hakios_pcie_glue *
hakios_pcie_glue_exynos8890(void)
{
	return &glue;
}
