/*
 * The Exynos 9810's side of the PCIe driver (drivers/wifi/hakios.exynos_pcie.c, which has the steps and the DesignWare
 * core; hakios.exynos_pcie_glue.h): the blocks, the pins, the clocks, the PHY, the radio's way to memory.
 *
 * The Galaxy S9's Wi-Fi, stage 1 (starlte, Exynos 9810): the PCIe root complex of channel 0, a DesignWare core with
 * Samsung's PHY, as far as a link to the Broadcom BCM4361 and that chip's identity read from its configuration space.
 * No firmware is loaded and no driver of the radio runs: that is stage 2 (devices/starlte/WIFI.md). Only with boot-arg
 * wifi_pcie=N on this phone (the SoC's registers at 0x10000000); nothing is touched without it.
 *
 * What is where (Samsung's device tree, pcie0@116A0000, and the pins it and the wlan node name):
 *   ELBI      0x116A0000  Samsung's glue: LTSSM enable, link state, resets of the PCS, PMA and MAC, Q-channel
 *   PHY       0x116D0000  the PMA (the common block, then the transceiver's from 0xC0)
 *   PCS       0x116C0000  the PHY's PCS
 *   SYSREG    0x11411044  SYSREG_FSYS1's PCIe words (+0 and +0xC: the PCS's speed, lane 0)
 *   DBI       0x11700000  the DesignWare core's own configuration space and port logic (the iATU's viewport at 0x900)
 *   config    0x127FE000  8 KB: the iATU's window for type 0 configuration requests (the first 4 KB)
 *   memory    0x11800000  16 MB less 64 KB: the window the endpoint's BARs are placed in (identity, bus = CPU address)
 *   PMU       0x14060000  +0x71C bit 0: the PHY out of isolation (PCIE_PHY_CONTROL of channel 0)
 *   SPI 253   the RC's interrupt (INTx, level high): the radio's driver's (hakios_exynos_pcie_intx(), brcmfmac=5)
 *   WLAN_REG_ON   gpg0-5 of PERIC0's GPIO (0x10430080): the radio's power, high = on (Samsung's dhd_custom_exynos.c)
 *   PERST#        gpf2-2 of FSYS1's GPIO (0x11430020): the endpoint's reset, high = out of reset
 *   CLKREQ#       gpf2-0 (function 2, pulled up)
 *   WLAN_HOST_WAKE gpa0-7 of ALIVE's GPIO (0x14050020), PCIE_WAKE gpa3-2 (0x14050080): read only
 * The clocks: the device tree gives pcie-clk-num = 0 and phy-clk-num = 0, so Android's kernel switches none of them; the
 * gates of CMU_FSYS1 (0x11400000) for the Gen2 controller are read and must be open (hardware-controlled or forced
 * on) before the block is touched, else a read of it could stop the bus. wifi_pcie_ungate=1 forces closed ones open.
 *
 *   wifi_pcie=1   a look, reading only: the PMU's isolation, CMU_FSYS1's gates and Q-channels, the five pins
 *   wifi_pcie=2   ... the pins as Android sets them: PERST# low, CLKREQ# to the controller, WLAN_REG_ON high, 200 ms
 *   wifi_pcie=3   ... the PHY: out of isolation and power-down, the core's soft reset, PERST# high (20 ms), the PHY's
 *                 tables (26 MHz reference, Gen2), the resets of the PCS, PMA and MAC, the CDR's; then the core's own
 *                 identity through DBI and the PLL's lock
 *   wifi_pcie=4   ... the root complex set up (one lane, Gen2) and the link trained: LTSSM enabled, the link state polled
 *                 for 20 ms for L0. Up to wifi_pcie_tries times (default 3) from step 3 again, as Android does (10)
 *   wifi_pcie=5   ... the endpoint through the iATU: its vendor and device (Broadcom 0x14e4, BCM4361 0x441f, or 0x4361 /
 *                 0x4420 / 0x4421), class, capabilities, the size of every BAR, which are then placed in the memory
 *                 window. Memory decoding and bus mastering stay off. This is where stage 1 ends
 *   wifi_pcie=6   ... memory decoding on (the command register's bit 1), and the endpoint handed to the radio's driver
 *                 (sys/hakios.exynos_pcie.h; hakios.brcmfmac.c, brcmfmac=N), which sets bus mastering itself
 * Each step says what it is about to do before it does it. A link that does not come up is put back: PERST# low, the
 * LTSSM off, WLAN_REG_ON low. A link that does stays up until the run ends, for stage 2; on the way back to the loader
 * (hakios_exynos_pcie_quiesce(): loader_return(), and the radio driver's hakios_wifi_stop() after it has taken the driver
 * down) the root complex's INTx enables are cleared and the chip is held in reset and unpowered. The endpoint's
 * configuration space is not touched then: after brcmfmac's removal (which resets the chip through its watchdog) or a
 * panic the link may be down, and a request that never completes could keep the log from the loader. Nothing here writes a PMIC: the radio's supplies are the battery's and
 * always-on rails that Android does not switch either (the wlan node has no regulator).
 * Step 6 opens the radio's way to memory (dma_path_open(), below): the data path's gates, the PCIe SysMMU (0x11420000)
 * clocked (its registers read and set to pass addresses through only with wifi_pcie_mmu_probe=1), and with wifi_pcie_coherent=1 the I/O coherency bits of SYSREG_FSYS1 (+0x700).
 * Not done here: translation by the SysMMU (Android's 1:1 table), MSI, the IA sequencer that retrains the CDR on an exit
 * from L1.2 (and L1 substates themselves).
 *
 * What was checked: compiled; every register and value against the BSP files below (the PR has the table). It has
 * not run on the phone.
 *
 * Sources (Samsung's GPL kernel for the S9, Scion-Project/android_kernel_samsung_exynos9810 at e5906667068e, read for
 * these facts, not copied): drivers/pci/host/pci-exynos.c (exynos_pcie_poweron, _establish_link, _setup_rc,
 * _assert_phy_reset, _rd/_wr_own_conf, _rd_other_conf, _prog_viewport_cfg0/_mem_outbound, NCLK_OFF_CONTROL),
 * pci-exynos.h (the ELBI, DBI, SYSREG and PMU offsets), pci-exynos9810_cal.c (exynos_pcie_phy_config,
 * _phy_all_pwrdn_clear, _phy_check_rx_elecidle: channel 0's tables), pcie-designware.c (dw_pcie_setup_rc),
 * drivers/pinctrl/samsung/pinctrl-exynos.c (the 9810's banks), drivers/soc/samsung/cal-if/exynos9810/cmucal-sfr.c
 * (CMU_FSYS1's PCIe gates and Q-channels), drivers/net/wireless/broadcom/bcmdhd_101_12/dhd_custom_exynos.c
 * (WLAN_REG_ON, 200 ms, then the RC on) and include/bcmdevs_legacy.h (the BCM4361's IDs); the phone's device tree
 * (Silicium's starlte.dts: pcie0@116A0000, wlan, cfg_wlanen, wlan_host_wake, pcie0_clkreq, pcie0_perst, pcie_wake).
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
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

#define SYSREG_PA       0x11411000UL    /* the page; channel 0's words are +0x44 and +0x50 */
#define CMU_FSYS1_PA    0x11400000UL
#define CMU_TOP_PA      0x1A242000UL    /* the page of CMU_TOP (0x1A240000) with GATE_CLKCMU_FSYS1_PCIE at +0x74 */

#define ELBI_PCS_G_RST          0x288
#define ELBI_PHY_RST            0x28C   /* the PCS and the PMA (CMN) */
#define ELBI_MAC_RST            0x290
#define ELBI_STATE_POWER_S      0x2BC
#define ELBI_STATE_POWER_M      0x2C0
#define ELBI_QCH_SEL            0x2C8   /* [11:0] the clock gating of the PMU, APB and AXI sides: all cleared */
#define ELBI_L12ERR_CTRL        0x2F0   /* bit 2: NCLK off, the 9810's guard against speculative access while unlinked */

/* CMU: a gate is open while hardware runs it (MANUAL, bit 20, clear) or it is forced on (MANUAL and CG_VAL, bit 21) */
#define GATE_MANUAL             (1u << 20)
#define GATE_CG_VAL             (1u << 21)
static const struct { uint16_t off; const char *name; } gates[] = {
	{ 0x2000, "PHY_REFCLK_IN" }, { 0x2038, "DBI_ACLK" }, { 0x203C, "PHY_APB" }, { 0x2040, "MSTR_ACLK" },
	{ 0x2044, "SUB_CTRL_APB" }, { 0x2048, "PCS_APB" }, { 0x204C, "SLV_ACLK" }, { 0x20B0, "XIU_DBI" },
	{ 0x20B4, "XIU_SLV" },
};
static const struct { uint16_t off; const char *name; } qchs[] = {
	{ 0x302C, "APB" }, { 0x3030, "DBI" }, { 0x3034, "MSTR" }, { 0x3038, "PCS" }, { 0x303C, "PHY" }, { 0x3000, "SOCPLL" },
};

static vm_offset_t sysreg, cmu, cmu_top;

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

static inline void
set_bits(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t v)
{
	wr(base, off, (rd(base, off) & ~mask) | (v & mask));
}

/* ---- the PHY (pci-exynos9810_cal.c, channel 0) ------------------------------------------------------------------ */

/* the common block (26 MHz reference, Gen2), written from the PHY's offset 0 a word a byte */
static const uint8_t phy_cmn[48] = {
	0x01, 0xE1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x0C, 0x61, 0x45, 0x65, 0x24, 0x33, 0x18, 0xE3, 0xFC,
	0xD8, 0x05, 0xE6, 0x80, 0x00, 0x00, 0x00, 0x00, 0x60, 0x11, 0x00, 0xA0, 0x05, 0x04, 0x18, 0x88,
	0xC0, 0xFF, 0x9B, 0x52, 0x22, 0x30, 0x4F, 0xDC, 0x40, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x80,
};

/* the transceiver, from word 0x30 (offset 0xC0) */
static const uint8_t phy_trsv[80] = {
	0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xC0, 0xFF, 0xFF, 0x3F, 0x8C, 0xC8, 0x02, 0x01, 0x88, 0x80,
	0x06, 0x90, 0x6C, 0x66, 0x09, 0x61, 0x42, 0x44, 0xC6, 0x50, 0x0A, 0x33, 0x58, 0xE7, 0x20, 0x22,
	0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x55, 0x15, 0xAC, 0xAA, 0x3E, 0x00,
	0x00, 0x00, 0x20, 0x3F, 0x00, 0x03, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x85, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* exynos_phy_all_pwrdn_clear() */
static void
power_up(const struct hakios_pcie_regs *r)
{
	set_bits(r->pcs, 0x100, 1u << 6, 0);
	set_bits(r->pcs, 0x104, 1u << 7, 0);
	wr(r->phy, 0x20 * 4, 0xC0);
	wr(r->phy, 0x57 * 4, 0x7E);
	wr(r->elbi, ELBI_STATE_POWER_S, 0x200000);
	wr(r->elbi, ELBI_STATE_POWER_M, 0xffffffffu);
}

static void
pulse(vm_offset_t elbi, uint32_t off, unsigned last_us)
{
	wr(elbi, off, 1);
	delay(10);
	wr(elbi, off, 0);
	delay(10);
	wr(elbi, off, 1);
	delay(last_us);
}

/* exynos_pcie_phy_config(), channel 0 (no OTP tuning: the BSP's defconfig has no CONFIG_EXYNOS_OTP) */
static void
phy_config(const struct hakios_pcie_regs *r)
{
	vm_offset_t phy = r->phy, pcs = r->pcs;
	uint32_t v;

	set_bits(r->elbi, ELBI_QCH_SEL, 0xfff, 0);
	/* SYSREG_FSYS1: the PCS's high speed, and lane 0 */
	set_bits(sysreg, 0x44, 1u << 1, 1u << 1);
	v = rd(sysreg, 0x50);
	v = (v & ~(0xfu << 4) & ~(0xfu << 2)) | (0x3u << 2);
	v &= ~(1u << 1);
	wr(sysreg, 0x50, v);
	set_bits(sysreg, 0x50, 1u << 12, 1u << 12);

	pulse(r->elbi, ELBI_PCS_G_RST, 10);
	for (unsigned i = 0; i < sizeof(phy_cmn); i++) {
		wr(phy, 4 * i, phy_cmn[i]);
	}
	for (unsigned i = 0; i < sizeof(phy_trsv); i++) {
		wr(phy, 4 * (0x30 + i), phy_trsv[i]);
	}
	wr(pcs, 0xF8, 0x70);                    /* tx latency */
	wr(pcs, 0x100, 0x87);                   /* the PCS's reference clock out */
	wr(pcs, 0x104, 0x50);
	v = rd(pcs, 0x0C);                      /* PRGM_TIMEOUT_L1SS_VAL */
	v = (v & ~(1u << 1)) | (1u << 4);
	wr(pcs, 0x0C, v);
	pulse(r->elbi, ELBI_MAC_RST, 10);
	pulse(r->elbi, ELBI_PHY_RST, 100);
	/* the CDR's reset: [7:6] = 3, 20 us, = 2, = 0 */
	v = rd(pcs, 0xD0) | (0x3u << 6);
	wr(pcs, 0xD0, v);
	delay(20);
	v = (rd(pcs, 0xD0) & ~(0x3u << 6)) | (0x2u << 6);
	wr(pcs, 0xD0, v);
	v &= ~(0x3u << 6);
	wr(pcs, 0xD0, v);
	say("exynos_pcie: 3: PHY PLL %x (bits 1:0 set: locked)\n", rd(phy, 0xBC) & 3);
}

/* exynos_pcie_rd/_wr_own_conf: DBI guarded while there is no link (PCS +0xEC bit 3: the receiver's electrical idle ignored) */
static void
dbi_guard(const struct hakios_pcie_regs *r, bool open)
{
	if (open) {
		wr(r->elbi, ELBI_L12ERR_CTRL, 0);
		set_bits(r->pcs, 0xEC, 1u << 3, 1u << 3);
	} else {
		set_bits(r->pcs, 0xEC, 1u << 3, 0);
		wr(r->elbi, ELBI_L12ERR_CTRL, 1u << 2);
	}
}

static void
link_up(const struct hakios_pcie_regs *r)
{
	wr(r->elbi, ELBI_L12ERR_CTRL, 0);
}

/* step 1: the gates; true if every one of the Gen2 controller's is open */
static bool
look(bool ungate)
{
	bool open = true;

	sysreg = ml_io_map(SYSREG_PA, 0x1000);
	cmu = ml_io_map(CMU_FSYS1_PA, 0x4000);
	cmu_top = ml_io_map(CMU_TOP_PA, 0x1000);
	if (!sysreg || !cmu || !cmu_top) {
		return false;
	}

	say("exynos_pcie: 1: CMU_TOP GATE_CLKCMU_FSYS1_PCIE %08x; CMU_FSYS1 MUX_CLKCMU_FSYS1_PCIE_USER %08x\n",
	    rd(cmu_top, 0x074), rd(cmu, 0x180));
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		uint32_t v = rd(cmu, gates[i].off);
		bool closed = (v & GATE_MANUAL) && !(v & GATE_CG_VAL);

		say("exynos_pcie: 1: gate %s (+%#x) %08x%s\n", gates[i].name, gates[i].off, v, closed ? ": CLOSED" : "");
		if (closed && ungate) {
			say("exynos_pcie: forcing gate %s open (wifi_pcie_ungate=1)\n", gates[i].name);
			wr(cmu, gates[i].off, v | GATE_CG_VAL);
			closed = !(rd(cmu, gates[i].off) & GATE_CG_VAL);
		}
		open = open && !closed;
	}
	for (unsigned i = 0; i < sizeof(qchs) / sizeof(qchs[0]); i++) {
		say("exynos_pcie: 1: Q-channel %s (+%#x) %08x\n", qchs[i].name, qchs[i].off, rd(cmu, qchs[i].off));
	}
	return open;
}

/*
 * What stands between the radio and memory (sys/hakios.exynos_pcie.h). The radio's reads and writes of our memory leave
 * the controller's master port (MSTR_ACLK) and cross FSYS1's data path to the bus: XIU_D_FSYS1, the PCIe SysMMU
 * (0x11420000), PPMU_FSYS1, BTM_FSYS1 and LHS_ACEL_D_FSYS1. Their gates in CMU_FSYS1 are said at step 6 and, closed,
 * forced open (MANUAL and CG_VAL): Android's exynos-pcie-iommu.c switches the SysMMU's clock on (clk_enable of "aclk")
 * whatever it then does with it. Left off (wifi-a4: a read of the SysMMU's CTRL stopped the bus, "Kernel data abort",
 * ESR 0x96000610, a synchronous external abort), nothing that crosses it would get through either.
 *
 * The SysMMU's own registers are not touched by default: wifi-a5 (its gate forced on, 00300000) read them at step 6
 * (CTRL 4, set to 0) but a later read from the interrupt poller (hakios_exynos_pcie_say_dma()) stopped the bus again,
 * a synchronous external abort. Its gate is still forced on, harmless, as the radio's DMA may cross the block in
 * bypass. wifi_pcie_mmu_probe=1 reads them (step 6 and say_dma): CTRL 0 is the bypass (Android's disable), 5
 * translating, 7 and 3 blocking; anything but 0 is then set to 0 (wifi_pcie_mmu_bypass=0 leaves it). The fault
 * registers are the ones of Samsung's exynos-pcie-iommu.h (AR 0x70/0x78, AW 0x80/0x88), nothing between.
 *
 * SYSREG_FSYS1 +0x700 (PCIE_SYSREG_SHARABILITY_CTRL, bits 9:8 for channel 0): 3 makes the radio's reads and writes
 * shareable, so that they look in the processors' caches (Android's use-cache-coherency = "true",
 * enable_cache_cohernecy()). wifi_pcie_coherent=1 sets it; the radio's memory is then the kernel's own cached memory
 * (hakios_wifi_radio_coherent(), hakios.lx.c), not a second, uncached view of it. Without it (the default) the bits are
 * left as they are and the radio's memory is uncached, which a radio whose writes do not look in the caches needs.
 *
 * Also said: the root port's and the endpoint's command/status (bit 29 of status: a master abort received, bit 28 a
 * target abort received, bit 27 one signalled, bit 30 SERR, bit 31 a parity error) and the PCIe device status of both
 * (bits 0-3: correctable, non-fatal, fatal, unsupported request).
 */
#define SYSMMU_PA       0x11420000UL
#define SYSREG_FSYS1_PA 0x11410000UL
#define SHARABILITY     0x700
#define SHARABLE_CH0    (3u << 8)
#define GATE_SYSMMU     0x2098
static const struct { uint16_t gate, qch; const char *name; } dma_path[] = {
	{ 0x2040, 0x3034, "PCIE_GEN2 MSTR_ACLK" }, { 0x20ac, 0, "XIU_D_FSYS1" }, { GATE_SYSMMU, 0x306c, "SYSMMU_FSYS1" },
	{ 0x2078, 0x3060, "PPMU_FSYS1" }, { 0x2018, 0x3014, "BTM_FSYS1" }, { 0x202c, 0x3024, "LHS_ACEL_D_FSYS1" },
	{ 0x209c, 0x3070, "SYSREG_FSYS1" },
};
static vm_offset_t mmu, fsys1_sysreg;
static bool mmu_clocked, mmu_probe, coherent;

static void
say_kept(const char *format, ...) __printflike(1, 2);

static void
say_kept(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	hakios_wifi_report_add(line);
	say("%s", line);
}

/* step 6, before the hand-over: the data path's gates, the SysMMU passing addresses through, the shareability */
static bool
dma_open(void)
{
	uint32_t bypass = 1, want_coherent = 0, probe = 0, v;

	PE_parse_boot_argn("wifi_pcie_mmu_bypass", &bypass, sizeof(bypass));
	PE_parse_boot_argn("wifi_pcie_coherent", &want_coherent, sizeof(want_coherent));
	PE_parse_boot_argn("wifi_pcie_mmu_probe", &probe, sizeof(probe));
	mmu_probe = probe != 0;
	if (fsys1_sysreg == 0) {
		fsys1_sysreg = ml_io_map(SYSREG_FSYS1_PA, 0x1000);
	}
	if (mmu_probe && mmu == 0) {
		mmu = ml_io_map(SYSMMU_PA, 0x1000);
	}
	for (unsigned i = 0; i < sizeof(dma_path) / sizeof(dma_path[0]); i++) {
		bool closed, force;

		v = rd(cmu, dma_path[i].gate);
		closed = (v & GATE_MANUAL) && !(v & GATE_CG_VAL);
		/* the SysMMU's clock is switched on as Android does, closed or hardware-run (its registers: mmu_probe only) */
		force = closed || dma_path[i].gate == GATE_SYSMMU;
		say("exynos_pcie: 6: the data path: gate %s (+%#x) %08x%s, Q-channel %08x\n", dma_path[i].name,
		    dma_path[i].gate, v, closed ? ": CLOSED" : "", dma_path[i].qch ? rd(cmu, dma_path[i].qch) : 0);
		if (force) {
			wr(cmu, dma_path[i].gate, v | GATE_MANUAL | GATE_CG_VAL);
			v = rd(cmu, dma_path[i].gate);
			say("exynos_pcie: 6: gate %s forced on: %08x\n", dma_path[i].name, v);
		}
		if (dma_path[i].gate == GATE_SYSMMU) {
			mmu_clocked = (v & GATE_MANUAL) && (v & GATE_CG_VAL);
		}
	}
	if (!mmu_probe) {
		say("exynos_pcie: 6: the PCIe SysMMU's registers are left alone (wifi_pcie_mmu_probe=1 reads them)\n");
	} else if (mmu != 0 && mmu_clocked) {
		v = rd(mmu, 0x000);
		say("exynos_pcie: 6: the PCIe SysMMU: CTRL %08x CFG %08x STATUS %08x VERSION %08x (CTRL 0: addresses passed through)\n",
		    v, rd(mmu, 0x004), rd(mmu, 0x008), rd(mmu, 0x034));
		if (v != 0 && bypass != 0) {
			wr(mmu, 0x000, 0);
			say("exynos_pcie: 6: the SysMMU set to pass addresses through: CTRL now %08x\n", rd(mmu, 0x000));
		}
	} else {
		say("exynos_pcie: 6: the PCIe SysMMU's clock is not on: its registers are not read\n");
	}
	if (fsys1_sysreg != 0) {
		v = rd(fsys1_sysreg, SHARABILITY);
		say("exynos_pcie: 6: SYSREG_FSYS1 +0x700 (shareability) %08x: channel 0's %u (3: the radio's reads and writes "
		    "look in the caches)\n", v, (v >> 8) & 3);
		if (want_coherent != 0) {
			wr(fsys1_sysreg, SHARABILITY, v | SHARABLE_CH0);
			v = rd(fsys1_sysreg, SHARABILITY);
			coherent = (v & SHARABLE_CH0) == SHARABLE_CH0;
			say("exynos_pcie: 6: wifi_pcie_coherent=1: shareability now %08x: %s\n", v,
			    coherent ? "coherent, the radio's memory is the kernel's cached memory" : "it did not take, uncached memory");
		}
	}
	return coherent;
}

static void
say_dma(const char *when)
{
	if (cmu != 0) {
		say_kept("exynos_pcie: %s: SysMMU gate %08x Q-channel %08x\n", when, rd(cmu, GATE_SYSMMU), rd(cmu, 0x306c));
	}
	if (mmu_probe && mmu != 0 && mmu_clocked) {
		say_kept("exynos_pcie: %s: SysMMU CTRL %08x CFG %08x STATUS %08x VERSION %08x INT_STATUS %08x; "
		    "fault AR %08x (info %08x), AW %08x (info %08x)\n", when,
		    rd(mmu, 0x000), rd(mmu, 0x004), rd(mmu, 0x008), rd(mmu, 0x034), rd(mmu, 0x060),
		    rd(mmu, 0x070), rd(mmu, 0x078), rd(mmu, 0x080), rd(mmu, 0x088));
	}
	if (fsys1_sysreg != 0) {
		say_kept("exynos_pcie: %s: SYSREG_FSYS1 +0x700 (shareability) %08x\n", when, rd(fsys1_sysreg, SHARABILITY));
	}
}

static const struct hakios_pcie_glue glue = {
	.name = "exynos9810",
	.radio = "BCM4361",
	.soc_base = 0x10000000,
	.elbi_pa = 0x116A0000, .phy_pa = 0x116D0000, .pcs_pa = 0x116C0000, .dbi_pa = 0x11700000,
	.cfg_pa = 0x127FE000,                   /* the first half of "config": type 0 requests (cfg0) */
	.mem_pa = 0x11800000, .mem_size = 0x00FF0000,   /* "ranges": 32-bit memory */
	.pmu_pa = 0x14060000, .pmu_phy_control = 0x71C,
	.intx_spi = 253,
	.max_speed = 2,                         /* the device tree's max-link-speed */
	.l1_substates = 0xEA,
	.pud_bits = 4,                          /* bank_type_6/7 */
	.irq_toe = true,
	.pins = {
		[HAKIOS_PCIE_WLAN_EN] = { "gpg0-5", 0x10430000, 0x080, 5 },     /* PERIC0 */
		[HAKIOS_PCIE_PERST] = { "gpf2-2", 0x11430000, 0x020, 2 },       /* FSYS1 */
		[HAKIOS_PCIE_CLKREQ] = { "gpf2-0", 0x11430000, 0x020, 0 },
		[HAKIOS_PCIE_HOST_WAKE] = { "gpa0-7", 0x14050000, 0x020, 7 },   /* ALIVE */
		[HAKIOS_PCIE_WAKE] = { "gpa3-2", 0x14050000, 0x080, 2 },
	},
	.look = look,
	.power_up = power_up,
	.phy_config = phy_config,
	.dbi_guard = dbi_guard,
	.link_up = link_up,
	.dma_open = dma_open,
	.say_dma = say_dma,
};

const struct hakios_pcie_glue *
hakios_pcie_glue_exynos9810(void)
{
	return &glue;
}
