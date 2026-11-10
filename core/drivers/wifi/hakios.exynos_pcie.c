/*
 * An Exynos's PCIe root complex to the phone's Broadcom Wi-Fi chip, stage 1: a DesignWare core with Samsung's ELBI glue
 * and PHY, as far as a link and the chip's identity read from its configuration space; then the endpoint handed to the
 * radio's driver (stage 2, hakios.brcmfmac.c). Only with boot-arg wifi_pcie=N; nothing is touched without it.
 *
 * The SoC's side (where the blocks and pins are, the clocks, the PHY's tables, the radio's way to memory) is a glue's
 * (hakios.exynos_pcie_glue.h): hakios.exynos9810_pcie.c, the Galaxy S9's, whose header has that phone's facts and
 * history, and the Note FE's devices/gracerlte/soc/drivers/core/hakios.exynos8890_pcie.c. The one this kernel has is
 * used, the 9810's first.
 *
 *   wifi_pcie=1   a look, reading only: the PMU's isolation, the glue's clocks, the five pins
 *   wifi_pcie=2   ... the pins as Android sets them: PERST# low, CLKREQ# to the controller, WLAN_REG_ON high, 200 ms
 *   wifi_pcie=3   ... the PHY: out of isolation and power-down, the core's soft reset, PERST# high (20 ms), the PHY's
 *                 tables and resets (the glue's); then the core's own identity through DBI
 *   wifi_pcie=4   ... the root complex set up (one lane) and the link trained: LTSSM enabled, the link state polled
 *                 for 20 ms for L0. Up to wifi_pcie_tries times (default 3) from step 3 again, as Android does (10)
 *   wifi_pcie=5   ... the endpoint through the iATU: its vendor and device, class, capabilities, the size of every BAR,
 *                 which are then placed in the memory window. Memory decoding and bus mastering stay off
 *   wifi_pcie=6   ... memory decoding on (the command register's bit 1), and the endpoint handed to the radio's driver
 *                 (sys/hakios.exynos_pcie.h; hakios.brcmfmac.c, brcmfmac=N), which sets bus mastering itself
 * Each step says what it is about to do before it does it. A link that does not come up is put back: PERST# low, the
 * LTSSM off, WLAN_REG_ON low. A link that does stays up until the run ends; on the way back to the loader
 * (hakios_exynos_pcie_quiesce()) the root complex's INTx enables are cleared and the chip is held in reset and unpowered.
 * wifi_pcie_ungate=1 lets the glue open clocks the boot left closed.
 *
 * Sources: Samsung's GPL kernels (drivers/pci/host/pci-exynos.c, pci-exynos.h, pcie-designware.c), read for these facts,
 * not copied; the glues name theirs.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_pcie.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

#include "hakios.exynos_pcie_glue.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern kern_return_t thread_terminate(thread_t thread);
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

/* ELBI (pci-exynos.h) */
#define ELBI_IRQ_PULSE          0x000   /* the latch: INTA bit 0, INTB 2, INTC 4, INTD 6 (asserts); one clears */
#define ELBI_IRQ_LEVEL          0x004
#define ELBI_IRQ_SPECIAL        0x008
#define ELBI_IRQ_EN_PULSE       0x00C
#define ELBI_IRQ_EN_LEVEL       0x010
#define ELBI_IRQ_EN_SPECIAL     0x014
#define ELBI_IRQ_TOE_EN_PULSE2  0x034   /* these two only where the glue says (irq_toe) */
#define ELBI_IRQ_TOE_EN_LEVEL   0x038
#define ELBI_INTX_ASSERTS       0x55u   /* IRQ_INTA_ASSERT | _INTB_ | _INTC_ | _INTD_ (pci-exynos.h) */
#define ELBI_SW_WAKE            0x018   /* bit 1 clear: the bus number enabled */
#define ELBI_APP_LTSSM_ENABLE   0x02C
#define ELBI_RDLH_LINKUP        0x074   /* [4:0] the LTSSM's state: 0x0D-0x14 is a link (0x11 L0) */
#define ELBI_PM_DSTATE          0x088
#define ELBI_HISTORY(i)         (0x138 + 4 * (i))
#define ELBI_LINKDOWN_RST_SEL   0x1B8
#define ELBI_SOFT_CORE_RESET    0x1D0
#define ELBI_HISTORY_CHECK      0x274
#define ELBI_APP_REQ_EXIT_L1_MODE 0x0F4 /* bit 0: hardware mode; bit 4: L1 request NAK when the master is busy */

/* DBI: the core's configuration space and port logic */
#define DBI_ID                  0x000
#define DBI_COMMAND             0x004
#define DBI_CLASS               0x008
#define DBI_BAR0                0x010
#define DBI_BAR1                0x014
#define DBI_BUS                 0x018
#define DBI_MEM_WINDOW          0x020
#define DBI_INTERRUPT           0x03C
#define DBI_PM_CTRL             0x044   /* the PM capability (0x40) + 4 */
#define DBI_LNKCAP              0x07C   /* the PCIe capability (0x70) + 0x0C */
#define DBI_LNKCTL              0x080   /* + 0x10; the status in [31:16] */
#define DBI_LNKCTL2             0x0A0   /* + 0x30 */
#define DBI_PORT_LINK_CONTROL   0x710
#define DBI_LINK_WIDTH_SPEED    0x80C
#define DBI_MISC_CONTROL        0x8BC   /* bit 0: read-only registers writable */
#define DBI_AUX_CLK_FREQ        0xB40
#define DBI_L1_SUBSTATES        0xB44
#define DBI_ATU_VIEWPORT        0x900
#define DBI_ATU_CR1             0x904
#define DBI_ATU_CR2             0x908
#define DBI_ATU_LOWER_BASE      0x90C
#define DBI_ATU_UPPER_BASE      0x910
#define DBI_ATU_LIMIT           0x914
#define DBI_ATU_LOWER_TARGET    0x918
#define DBI_ATU_UPPER_TARGET    0x91C
#define ATU_ENABLE              (1u << 31)
#define ATU_TYPE_MEM            0
#define ATU_TYPE_CFG0           4

#define BCM_VENDOR              0x14e4
#define WIFI_TURNON_MS          200     /* dhd_custom_exynos.c, WIFI_TURNON_DELAY */
#define LINK_POLLS              2000    /* of 10 us: pci-exynos.h MAX_TIMEOUT */
#define CFG_SIZE                0x1000UL

static const struct hakios_pcie_glue *glue;
static struct hakios_pcie_regs regs;
static vm_offset_t cfg, pmu, gpio[HAKIOS_PCIE_NPINS];
static bool coherent;          /* the glue's dma_open() said so (step 6) */
static bool reg_on;            /* WLAN_REG_ON driven high by this run (step 2), until put back */

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

/* ---- pins ----------------------------------------------------------------------------------------------------- */

static uint32_t
pin_con(unsigned i)
{
	return (rd(gpio[i], glue->pins[i].bank) >> (4 * glue->pins[i].pin)) & 0xf;
}

static uint32_t
pin_dat(unsigned i)
{
	return (rd(gpio[i], glue->pins[i].bank + 4) >> glue->pins[i].pin) & 1;
}

static uint32_t
pin_pud(unsigned i)
{
	return (rd(gpio[i], glue->pins[i].bank + 8) >> (glue->pud_bits * glue->pins[i].pin)) & ((1u << glue->pud_bits) - 1);
}

/* an output, driven: the level first, then the function, as gpio_direction_output() does */
static void
pin_out(unsigned i, unsigned level)
{
	const struct hakios_pcie_pin *p = &glue->pins[i];

	set_bits(gpio[i], p->bank + 4, 1u << p->pin, (uint32_t)level << p->pin);
	set_bits(gpio[i], p->bank, 0xfu << (4 * p->pin), 1u << (4 * p->pin));
}

static void
say_pins(const char *when)
{
	say("exynos_pcie: %s: WLAN_REG_ON %s CON %x DAT %u; PERST# %s CON %x DAT %u; CLKREQ# %s CON %x PUD %x; "
	    "HOST_WAKE %s CON %x DAT %u; PCIE_WAKE %s CON %x DAT %u\n", when,
	    glue->pins[HAKIOS_PCIE_WLAN_EN].name, pin_con(HAKIOS_PCIE_WLAN_EN), pin_dat(HAKIOS_PCIE_WLAN_EN),
	    glue->pins[HAKIOS_PCIE_PERST].name, pin_con(HAKIOS_PCIE_PERST), pin_dat(HAKIOS_PCIE_PERST),
	    glue->pins[HAKIOS_PCIE_CLKREQ].name, pin_con(HAKIOS_PCIE_CLKREQ), pin_pud(HAKIOS_PCIE_CLKREQ),
	    glue->pins[HAKIOS_PCIE_HOST_WAKE].name, pin_con(HAKIOS_PCIE_HOST_WAKE), pin_dat(HAKIOS_PCIE_HOST_WAKE),
	    glue->pins[HAKIOS_PCIE_WAKE].name, pin_con(HAKIOS_PCIE_WAKE), pin_dat(HAKIOS_PCIE_WAKE));
}

/* exynos_phy_check_rx_elecidle(): PCS +0xEC bit 3, 1 = ignore the receiver's electrical idle while DBI is used */
static void
rx_elecidle_ignore(bool ignore)
{
	set_bits(regs.pcs, 0xEC, 1u << 3, ignore ? 1u << 3 : 0);
}

/* ---- DBI: the core's own registers (exynos_pcie_rd/_wr_own_conf: guarded while there is no link) --------------- */

static void
dbi_open(void)
{
	if (!regs.linked && glue->dbi_guard != NULL) {
		glue->dbi_guard(&regs, true);
	}
}

static void
dbi_close(void)
{
	if (!regs.linked && glue->dbi_guard != NULL) {
		glue->dbi_guard(&regs, false);
	}
}

static uint32_t
dbi_rd(uint32_t off)
{
	uint32_t v;

	dbi_open();
	v = rd(regs.dbi, off);
	dbi_close();
	return v;
}

static void
dbi_wr(uint32_t off, uint32_t v)
{
	dbi_open();
	wr(regs.dbi, off, v);
	dbi_close();
}

static void
dbi_set(uint32_t off, uint32_t mask, uint32_t v)
{
	dbi_wr(off, (dbi_rd(off) & ~mask) | (v & mask));
}

/* one outbound region of the iATU, through its viewport (exynos_pcie_prog_viewport_*) */
static void
atu_outbound(uint32_t index, uint32_t type, uint64_t cpu, uint64_t size, uint64_t target)
{
	dbi_wr(DBI_ATU_VIEWPORT, index);        /* bit 31 clear: outbound */
	dbi_wr(DBI_ATU_CR1, type);
	dbi_wr(DBI_ATU_LOWER_BASE, (uint32_t)cpu);
	dbi_wr(DBI_ATU_UPPER_BASE, (uint32_t)(cpu >> 32));
	dbi_wr(DBI_ATU_LIMIT, (uint32_t)(cpu + size - 1));
	dbi_wr(DBI_ATU_LOWER_TARGET, (uint32_t)target);
	dbi_wr(DBI_ATU_UPPER_TARGET, (uint32_t)(target >> 32));
	dbi_wr(DBI_ATU_CR2, ATU_ENABLE);
}

/* ---- the steps ------------------------------------------------------------------------------------------------ */

static bool
map_all(void)
{
	bool mapped = true;

	regs.elbi = ml_io_map(glue->elbi_pa, 0x1000);
	regs.phy = ml_io_map(glue->phy_pa, 0x1000);
	regs.pcs = ml_io_map(glue->pcs_pa, 0x1000);
	regs.dbi = ml_io_map(glue->dbi_pa, 0x1000);
	cfg = ml_io_map(glue->cfg_pa, CFG_SIZE);
	pmu = ml_io_map(glue->pmu_pa, 0x1000);
	for (unsigned i = 0; i < HAKIOS_PCIE_NPINS; i++) {
		gpio[i] = ml_io_map(glue->pins[i].gpio_pa, 0x1000);
		mapped = mapped && gpio[i] != 0;
	}
	return mapped && regs.elbi && regs.phy && regs.pcs && regs.dbi && cfg && pmu;
}

/* steps 3 and the first half of 4's tries: the PHY and the core up to the moment the LTSSM is let go */
static void
phy_up(void)
{
	say("exynos_pcie: 3: the core's soft reset, PERST# high (20 ms), L1 exit in hardware, the PHY's tables\n");
	rx_elecidle_ignore(true);
	wr(regs.elbi, ELBI_SOFT_CORE_RESET, 0);
	delay(20);
	wr(regs.elbi, ELBI_SOFT_CORE_RESET, 1);
	pin_out(HAKIOS_PCIE_PERST, 1);
	delay_for_interval(20, NSEC_PER_MSEC);
	set_bits(regs.elbi, ELBI_APP_REQ_EXIT_L1_MODE, 0x11, 0x11);
	wr(regs.elbi, ELBI_LINKDOWN_RST_SEL, 1u << 1);  /* the link-down reset by hand */
	glue->phy_config(&regs);
	set_bits(regs.elbi, ELBI_SW_WAKE, 1u << 1, 0);  /* the bus number enabled */
	say("exynos_pcie: 3: the core's own ID %08x, class %08x\n", dbi_rd(DBI_ID), dbi_rd(DBI_CLASS));
}

/* dw_pcie_setup_rc() then exynos_pcie_setup_rc(), for one lane */
static void
setup_rc(void)
{
	dbi_set(DBI_PORT_LINK_CONTROL, 0x3fu << 16, 1u << 16);
	dbi_set(DBI_LINK_WIDTH_SPEED, 0x1fu << 8, 1u << 8);
	dbi_wr(DBI_BAR0, 0x4);
	dbi_wr(DBI_BAR1, 0);
	dbi_set(DBI_INTERRUPT, 0xff00, 0x0100);         /* INTA */
	dbi_set(DBI_BUS, 0x00ffffff, 0x00ff0100);       /* primary 0, secondary 1, subordinate 0xff */
	dbi_set(DBI_COMMAND, 0xffff, 0x0107);           /* I/O, memory, bus master, SERR */
	dbi_wr(DBI_BAR0, 0);
	dbi_set(DBI_CLASS, 0xffff0000u, 0x0604u << 16); /* a PCI-to-PCI bridge */
	dbi_set(DBI_LINK_WIDTH_SPEED, 1u << 17, 1u << 17);      /* speed change */

	dbi_wr(DBI_MISC_CONTROL, 1);                    /* read-only registers writable */
	dbi_set(DBI_LNKCAP, 0x38000 | 0x3f0 | 0xf, (0x7u << 15) | (1u << 4) | 2);  /* L1 exit 64 us, x1, 5 GT/s */
	dbi_wr(DBI_AUX_CLK_FREQ, 0x1A);                 /* 26 MHz */
	dbi_wr(DBI_L1_SUBSTATES, glue->l1_substates);
	dbi_wr(DBI_PM_CTRL, 0);
	dbi_set(DBI_LNKCTL, 1u << 5, 1u << 5);          /* retrain */
	dbi_set(DBI_LNKCTL2, 0xf, glue->max_speed);
}

static bool
train(void)
{
	uint32_t v = 0;

	say("exynos_pcie: 4: the root complex set up (x1, Gen%u), then the LTSSM enabled\n", glue->max_speed);
	setup_rc();
	rx_elecidle_ignore(false);
	say("exynos_pcie: 4: D state %x, link state %x before\n", rd(regs.elbi, ELBI_PM_DSTATE) & 7, rd(regs.elbi, ELBI_RDLH_LINKUP));
	wr(regs.elbi, ELBI_APP_LTSSM_ENABLE, 1);
	for (unsigned i = 0; i < LINK_POLLS; i++) {
		v = rd(regs.elbi, ELBI_RDLH_LINKUP) & 0x1f;
		if (v >= 0x0d && v <= 0x14) {
			regs.linked = true;
			if (glue->link_up != NULL) {
				glue->link_up(&regs);
			}
			v = rd(regs.dbi, DBI_LNKCTL) >> 16;
			say("exynos_pcie: 4: LINK UP after %u us (state %x): Gen%u x%u (at most Gen%u)\n", 10 * i,
			    rd(regs.elbi, ELBI_RDLH_LINKUP) & 0x1f, v & 0xf, (v >> 4) & 0x3f, glue->max_speed);
			wr(regs.elbi, ELBI_IRQ_PULSE, rd(regs.elbi, ELBI_IRQ_PULSE));    /* what is pending, cleared */
			return true;
		}
		delay(10);
	}
	say("exynos_pcie: 4: no link in %u us: state %x; the LTSSM's last states, oldest first:\n", 10 * LINK_POLLS,
	    rd(regs.elbi, ELBI_RDLH_LINKUP));
	for (int i = 31; i >= 0; i -= 8) {
		say("exynos_pcie:    ");
		for (int j = i; j > i - 8; j--) {
			uint32_t h = rd(regs.elbi, ELBI_HISTORY(j));

			say(" %02x/%x/%x", (h >> 16) & 0x3f, h & 7, (h >> 8) & 7);   /* LTSSM / L1 substate / D state */
		}
		say("\n");
	}
	pin_out(HAKIOS_PCIE_PERST, 0);
	wr(regs.elbi, ELBI_APP_LTSSM_ENABLE, 0);
	return false;
}

static uint32_t
cfg_rd(uint32_t off)
{
	return rd(cfg, off);
}

static void
cfg_wr(uint32_t off, uint32_t v)
{
	wr(cfg, off, v);
}

/* where step 5 placed the endpoint's BARs (address 0: not placed), and whether step 6 has handed it over */
static uint64_t bar_address[6], bar_size[6];
static volatile bool handed_over;

/* step 5: bus 1, device 0, function 0; true if it is an endpoint with a BAR placed */
static bool
enumerate(void)
{
	bool placed = false;

	uint32_t id, class, hdr, sub, next = (uint32_t)glue->mem_pa;

	say("exynos_pcie: 5: the iATU: region 1 to type 0 configuration of 1:00.0 at %#llx, region 0 to memory at %#llx\n",
	    glue->cfg_pa, glue->mem_pa);
	atu_outbound(1, ATU_TYPE_CFG0, glue->cfg_pa, CFG_SIZE, 1u << 24);
	atu_outbound(0, ATU_TYPE_MEM, glue->mem_pa, glue->mem_size, glue->mem_pa);
	/* the bridge's memory window: whole megabytes inside the window (the S9's: 0x11800000 to 0x126FFFFF) */
	dbi_wr(DBI_MEM_WINDOW, (((((uint32_t)((glue->mem_pa + glue->mem_size) >> 20) - 1) << 4) | 0xfu) << 16) |
	    ((uint32_t)(glue->mem_pa >> 20) << 4));

	id = cfg_rd(0x00);
	if ((id & 0xffff) == 0xffff || id == 0) {
		say("exynos_pcie: 5: nothing answers at 1:00.0 (%08x)\n", id);
		return false;
	}
	class = cfg_rd(0x08);
	hdr = cfg_rd(0x0C);
	sub = cfg_rd(0x2C);
	say("exynos_pcie: 5: 1:00.0 is %04x:%04x%s, class %06x rev %02x, header %02x, subsystem %04x:%04x\n", id & 0xffff,
	    id >> 16, (id & 0xffff) == BCM_VENDOR ? " (Broadcom)" : " (NOT Broadcom)", class >> 8, class & 0xff,
	    (hdr >> 16) & 0x7f, sub & 0xffff, sub >> 16);
	if (cfg_rd(0x04) & (0x10u << 16)) {
		uint32_t at = cfg_rd(0x34) & 0xfc;

		say("exynos_pcie: 5: capabilities:");
		for (unsigned n = 0; at >= 0x40 && n < 24; n++) {
			uint32_t cap = cfg_rd(at);

			say(" %02x@%02x", cap & 0xff, at);
			if ((cap & 0xff) == 0x10) {
				say("(link %x)", cfg_rd(at + 0x10) >> 16);
			}
			at = (cap >> 8) & 0xfc;
		}
		say("\n");
	}
	if (((hdr >> 16) & 0x7f) != 0) {
		say("exynos_pcie: 5: not an endpoint's header: BARs left alone\n");
		return false;
	}
	/* the BARs: sized with decoding off (it is, after reset), then placed in the window, aligned to their size */
	for (unsigned b = 0; b < 6; b++) {
		uint32_t off = 0x10 + 4 * b, was = cfg_rd(off), lo, hi = 0xffffffffu;
		bool is64 = (was & 0x7) == 0x4;
		uint64_t mask, size;

		cfg_wr(off, 0xffffffffu);
		lo = cfg_rd(off);
		cfg_wr(off, was);
		if (is64 && b < 5) {
			uint32_t was_hi = cfg_rd(off + 4);

			cfg_wr(off + 4, 0xffffffffu);
			hi = cfg_rd(off + 4);
			cfg_wr(off + 4, was_hi);
		}
		if (lo == 0) {
			continue;
		}
		if (was & 1) {
			say("exynos_pcie: 5: BAR%u: I/O (%08x), left alone\n", b, lo);
			continue;
		}
		mask = ((uint64_t)hi << 32) | (lo & ~0xfu);
		size = ~mask + 1;
		if (!is64) {
			size &= 0xffffffffu;
		}
		next = (uint32_t)((next + size - 1) & ~(size - 1));
		if (size == 0 || next + size > glue->mem_pa + glue->mem_size) {
			say("exynos_pcie: 5: BAR%u: %s, %#llx bytes: does not fit the window, left unplaced\n", b,
			    is64 ? "64-bit" : "32-bit", size);
		} else {
			cfg_wr(off, next | (was & 0xf));
			if (is64) {
				cfg_wr(off + 4, 0);
			}
			say("exynos_pcie: 5: BAR%u: %s%s memory, %#llx bytes (%llu KB), placed at %#x (read back %08x)\n", b,
			    is64 ? "64-bit" : "32-bit", (was & 8) ? " prefetchable" : "", size, size >> 10, next, cfg_rd(off));
			bar_address[b] = next;
			bar_size[b] = size;
			placed = true;
			next += (uint32_t)size;
		}
		if (is64) {
			b++;
		}
	}
	say("exynos_pcie: 5: stage 1 done: the link is up and the endpoint known. Decoding and bus mastering stay off; "
	    "no firmware (stage 2)\n");
	return placed;
}

/* ---- for the radio's driver (sys/hakios.exynos_pcie.h) ---------------------------------------------------------- */

bool
hakios_exynos_pcie_ready(void)
{
	return handed_over;
}

uint32_t
hakios_exynos_pcie_config_read(uint32_t offset)
{
	return handed_over && offset < CFG_SIZE ? cfg_rd(offset & ~3u) : 0xffffffffu;
}

void
hakios_exynos_pcie_config_write(uint32_t offset, uint32_t value)
{
	if (handed_over && offset < CFG_SIZE) {
		cfg_wr(offset & ~3u, value);
	}
}

bool
hakios_exynos_pcie_bar(unsigned bar, uint64_t *address, uint64_t *size)
{
	if (!handed_over || bar >= 6 || bar_address[bar] == 0) {
		return false;
	}
	*address = bar_address[bar];
	*size = bar_size[bar];
	return true;
}

/*
 * INTx (sys/hakios.exynos_pcie.h): what Android's exynos_pcie_enable_interrupts() writes, INTA to INTD enabled in
 * IRQ_EN_PULSE and the TOE, LEVEL and SPECIAL sources off; the latch cleared first, so that nothing old fires.
 */
void
hakios_exynos_pcie_intx(bool on)
{
	if (!handed_over) {
		return;
	}
	wr(regs.elbi, ELBI_IRQ_EN_PULSE, 0);
	wr(regs.elbi, ELBI_IRQ_PULSE, rd(regs.elbi, ELBI_IRQ_PULSE));
	if (on) {
		if (glue->irq_toe) {
			wr(regs.elbi, ELBI_IRQ_TOE_EN_PULSE2, 0);
			wr(regs.elbi, ELBI_IRQ_TOE_EN_LEVEL, 0);
		}
		wr(regs.elbi, ELBI_IRQ_EN_LEVEL, 0);
		wr(regs.elbi, ELBI_IRQ_EN_SPECIAL, 0);
		wr(regs.elbi, ELBI_IRQ_EN_PULSE, ELBI_INTX_ASSERTS);
	}
}

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

/* the glue's dma_open() made the radio's reads and writes coherent: its memory need not be uncached (hakios.lx.c) */
bool
hakios_wifi_radio_coherent(void)
{
	return coherent;
}

void
hakios_exynos_pcie_say_dma(const char *when)
{
	uint32_t ep_cap = 0, at;

	if (!handed_over) {
		return;
	}
	if (glue->say_dma != NULL) {
		glue->say_dma(when);
	}
	at = cfg_rd(0x34) & 0xfc;
	for (unsigned n = 0; at >= 0x40 && n < 24; n++) {
		uint32_t cap = cfg_rd(at);

		if ((cap & 0xff) == 0x10) {
			ep_cap = at;
			break;
		}
		at = (cap >> 8) & 0xfc;
	}
	say_kept("exynos_pcie: %s: root port command/status %08x (device status %04x), endpoint %08x (device status %04x, "
	    "control %04x), ELBI IRQ latch %08x\n", when, dbi_rd(DBI_COMMAND), dbi_rd(0x78) >> 16, cfg_rd(0x04),
	    ep_cap ? cfg_rd(ep_cap + 8) >> 16 : 0xffff, ep_cap ? cfg_rd(ep_cap + 8) & 0xffff : 0xffff, rd(regs.elbi, ELBI_IRQ_PULSE));
}

/* exynos_pcie_irq_handler()'s first two lines: what is latched, written back to clear it */
uint32_t
hakios_exynos_pcie_intx_ack(void)
{
	uint32_t latched;

	if (!handed_over) {
		return 0;
	}
	latched = rd(regs.elbi, ELBI_IRQ_PULSE);
	wr(regs.elbi, ELBI_IRQ_PULSE, latched);
	return latched;
}

/* step 6: the endpoint's memory decoding on, and the endpoint to the radio's driver */
static void
hand_over(void)
{
	say("exynos_pcie: 6: 1:00.0's memory decoding on (command bit 1; bus mastering is the radio driver's to set)\n");
	cfg_wr(0x04, (cfg_rd(0x04) & 0xffffu) | 0x2u);
	if (glue->dma_open != NULL) {
		coherent = glue->dma_open();
	}
	say("exynos_pcie: 6: command/status now %08x; the endpoint handed to the radio's driver (brcmfmac=N)\n", cfg_rd(0x04));
	handed_over = true;
	hakios_brcmfmac_start();
}

static void
wifi_pcie_thread(void *arg, __unused wait_result_t wr_)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, tries = 3, ungate = 0;
	bool open;

	PE_parse_boot_argn("wifi_pcie_tries", &tries, sizeof(tries));
	PE_parse_boot_argn("wifi_pcie_ungate", &ungate, sizeof(ungate));
	tries = tries == 0 ? 1 : tries > 10 ? 10 : tries;
	say("exynos_pcie: wifi_pcie=%u: %s PCIe to the %s, step by step (5: stage 1; 6: to the driver)\n", steps, glue->name,
	    glue->radio);
	if (!map_all()) {
		say("exynos_pcie: the registers cannot be mapped\n");
		goto out;
	}
	say("exynos_pcie: 1: PMU PCIE_PHY_CONTROL %08x (bit 0: out of isolation)\n", rd(pmu, glue->pmu_phy_control));
	open = glue->look(steps >= 3 && ungate != 0);
	say_pins("1");
	if (steps < 2) {
		goto out;
	}

	say("exynos_pcie: 2: PERST# low, CLKREQ# to the controller (function 2, pull-up), WLAN_REG_ON high, %u ms\n",
	    WIFI_TURNON_MS);
	pin_out(HAKIOS_PCIE_PERST, 0);
	{
		const struct hakios_pcie_pin *p = &glue->pins[HAKIOS_PCIE_CLKREQ];
		uint32_t pud = glue->pud_bits * p->pin;

		set_bits(gpio[HAKIOS_PCIE_CLKREQ], p->bank + 8, ((1u << glue->pud_bits) - 1) << pud, 0x3u << pud);
		set_bits(gpio[HAKIOS_PCIE_CLKREQ], p->bank, 0xfu << (4 * p->pin), 0x2u << (4 * p->pin));
	}
	pin_out(HAKIOS_PCIE_WLAN_EN, 1);
	reg_on = true;
	delay_for_interval(WIFI_TURNON_MS, NSEC_PER_MSEC);
	say_pins("2");
	if (pin_dat(HAKIOS_PCIE_WLAN_EN) != 1) {
		say("exynos_pcie: 2: WLAN_REG_ON does not read back high: stopping\n");
		goto out;
	}
	if (steps < 3) {
		goto out;
	}

	if (!open) {
		say("exynos_pcie: 3: the controller's clocks are not all on (above): the block is not touched. "
		    "wifi_pcie_ungate=1 opens them\n");
		goto off;
	}
	say("exynos_pcie: 3: the PHY out of isolation (PMU PCIE_PHY_CONTROL bit 0), out of power-down, the LTSSM's history on\n");
	set_bits(pmu, glue->pmu_phy_control, 1, 1);
	if ((rd(pmu, glue->pmu_phy_control) & 1) == 0) {
		say("exynos_pcie: 3: PCIE_PHY_CONTROL does not read back 1 (%08x): stopping\n", rd(pmu, glue->pmu_phy_control));
		goto off;
	}
	glue->power_up(&regs);
	wr(regs.elbi, ELBI_HISTORY_CHECK, 0);
	wr(regs.elbi, ELBI_HISTORY_CHECK, 3);
	for (uint32_t t = 1;; t++) {
		phy_up();
		if (steps < 4) {
			say("exynos_pcie: 3: done; the PHY is left as it is, PERST# high, until the run ends\n");
			goto out;
		}
		if (train()) {
			break;
		}
		if (t >= tries) {
			say("exynos_pcie: 4: no link in %u tries\n", tries);
			goto off;
		}
		say("exynos_pcie: 4: try %u of %u\n", t + 1, tries);
	}
	if (steps < 5) {
		say("exynos_pcie: 4: done; the link stays up until the run ends\n");
		goto out;
	}
	if (!enumerate() || steps < 6) {
		goto out;
	}
	hand_over();
	goto out;
off:
	say("exynos_pcie: put back: PERST# low, the LTSSM off, WLAN_REG_ON low\n");
	pin_out(HAKIOS_PCIE_PERST, 0);
	if (open) {
		wr(regs.elbi, ELBI_APP_LTSSM_ENABLE, 0);
	}
	pin_out(HAKIOS_PCIE_WLAN_EN, 0);
	reg_on = false;
out:
	thread_terminate(current_thread());
}

/*
 * On the way back to the loader (loader_return(), a panic's way out too: one processor left, no lock taken): the chip held
 * in reset and its power off, PERST# low then WLAN_REG_ON low, as the stopping path above does -- if this run powered it.
 * From step 2 on the steps leave it powered "until the run ends", and with its firmware up (brcmfmac=3 and on) it is a bus
 * master with rings in the kernel's memory, which the loader reuses for the log while the BCM4361 would still write there.
 * The radio's driver calls this too, from hakios_wifi_stop(), once it has taken brcmfmac down in order; the second call
 * does nothing.
 */
void
hakios_exynos_pcie_quiesce(void)
{
	if (!reg_on) {
		return;
	}
	if (handed_over) {
		/* INTx off (brcmfmac=5): nothing latched, nothing to raise SPI 253 */
		wr(regs.elbi, ELBI_IRQ_EN_PULSE, 0);
		wr(regs.elbi, ELBI_IRQ_PULSE, rd(regs.elbi, ELBI_IRQ_PULSE));
	}
	pin_out(HAKIOS_PCIE_PERST, 0);
	pin_out(HAKIOS_PCIE_WLAN_EN, 0);
	reg_on = false;
	say("exynos_pcie: at the run's end: PERST# low, WLAN_REG_ON low (reads %u)\n", pin_dat(HAKIOS_PCIE_WLAN_EN));
}

void
hakios_exynos_pcie_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("wifi_pcie", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	glue = hakios_pcie_glue_exynos9810();
	if (glue == NULL) {
		glue = hakios_pcie_glue_exynos8890();
	}
	if (glue == NULL || (glue->soc_base != 0 && pe_arm_get_soc_base_phys() != glue->soc_base)) {
		return;                 /* no glue in this kernel, or not its SoC */
	}
	if (kernel_thread_start(wifi_pcie_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
