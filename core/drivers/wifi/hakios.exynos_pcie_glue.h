/*
 * The SoC's side of the Exynos PCIe root complex (drivers/wifi/hakios.exynos_pcie.c): where the blocks and the pins are,
 * the clocks, the PHY. The shared driver keeps the DesignWare core, the steps and what the radio's driver sees.
 * The glues: hakios.exynos9810_pcie.c (the Galaxy S9), devices/gracerlte/soc/drivers/core/hakios.exynos8890_pcie.c.
 */
#ifndef _HAKIOS_EXYNOS_PCIE_GLUE_H_
#define _HAKIOS_EXYNOS_PCIE_GLUE_H_

#include <stdbool.h>
#include <stdint.h>
#include <mach/mach_types.h>

/* a pin: its GPIO block, the bank's offset in it (CON +0, 4 bits a pin; DAT +4; PUD +8) */
struct hakios_pcie_pin {
	const char *name;               /* "gpj1-3" */
	uint64_t gpio_pa;
	uint16_t bank;
	uint8_t pin;
};

/* WLAN_REG_ON (high: the radio powered), PERST# (high: out of reset), CLKREQ#, and two that are only read */
enum { HAKIOS_PCIE_WLAN_EN, HAKIOS_PCIE_PERST, HAKIOS_PCIE_CLKREQ, HAKIOS_PCIE_HOST_WAKE, HAKIOS_PCIE_WAKE, HAKIOS_PCIE_NPINS };

/* what the shared driver has mapped, for the glue's steps */
struct hakios_pcie_regs {
	vm_offset_t elbi, phy, pcs, dbi;
	bool linked;
};

struct hakios_pcie_glue {
	const char *name;               /* "exynos9810" */
	const char *radio;              /* "BCM4361" */
	uint64_t soc_base;              /* pe_arm_get_soc_base_phys() of this SoC's phones; 0: not checked */
	uint64_t elbi_pa, phy_pa, pcs_pa, dbi_pa;
	uint64_t cfg_pa;                /* 4 KB: type 0 configuration requests */
	uint64_t mem_pa, mem_size;      /* the endpoint's BARs: bus address = CPU address */
	uint64_t pmu_pa;                /* the PMU's page with PCIE_PHY_CONTROL at +pmu_phy_control (bit 0: out of isolation) */
	uint32_t pmu_phy_control;
	uint32_t intx_spi;
	uint8_t max_speed;              /* 1: 2.5 GT/s, 2: 5 GT/s */
	uint8_t l1_substates;           /* the port logic's L1 substates word (0xB44) */
	uint8_t pud_bits;               /* the width of a pin's pull field */
	bool irq_toe;                   /* ELBI has the TOE interrupt enables (+0x34, +0x38), cleared with INTx on */
	struct hakios_pcie_pin pins[HAKIOS_PCIE_NPINS];

	/* step 1: the glue's own blocks mapped, the clocks said (and opened, with ungate); true: the block may be touched */
	bool (*look)(bool ungate);
	/* step 3, once: after the PHY is out of isolation */
	void (*power_up)(const struct hakios_pcie_regs *r);
	/* step 3, every try: ELBI's clock gating choice, then the PHY's tables, resets and clocks */
	void (*phy_config)(const struct hakios_pcie_regs *r);
	/* around every DBI access while there is no link; NULL: nothing to guard */
	void (*dbi_guard)(const struct hakios_pcie_regs *r, bool open);
	/* the link has come up; NULL: nothing */
	void (*link_up)(const struct hakios_pcie_regs *r);
	/* step 6: the radio's way to memory; true: its reads and writes look in the processors' caches. NULL: nothing */
	bool (*dma_open)(void);
	/* the SoC's part of hakios_exynos_pcie_say_dma(); NULL: nothing */
	void (*say_dma)(const char *when);
};

const struct hakios_pcie_glue *hakios_pcie_glue_exynos9810(void);
const struct hakios_pcie_glue *hakios_pcie_glue_exynos8890(void);

#endif
