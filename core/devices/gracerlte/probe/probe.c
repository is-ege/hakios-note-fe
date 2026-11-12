// Reads the identification registers of every core and leaves a text report where TWRP's pstore picks it up
// (/sys/fs/pstore/pmsg-ramoops-0), then reboots into TWRP. The question it answers: does the Exynos M1 have
// the 16K translation granule (ID_AA64MMFR0_EL1.TGran16) that iOS's XNU needs?

#include <stdint.h>

#define PMU_SWRESET	0x105C0400ul
#define FB_BASE		0xD8000000ul	/* s3cfb.bootloaderfb */
#define FB_WIDTH	1440
#define FB_HEIGHT	2560

/* TWRP kernel: ramoops attached 0x8000@0x92000000, record 16K, then console/ftrace/pmsg 4K each. Every 4K slot
 * after the dump zone gets the report, so it shows whichever of them TWRP exposes. */
#define PSTORE_SLOTS	0x92004000ul
#define PSTORE_SLOT_SZ	0x1000ul
#define PSTORE_NSLOTS	4
#define PERSISTENT_RAM_SIG 0x43474244u

#define PSCI_VERSION	0x84000000u
#define PSCI_CPU_ON	0xC4000003u	/* from the phone's DT (psci 0.1, smc) */

#define NSLOTS		9		/* 0-3 M1 (cluster 0), 4-7 A53 (cluster 1), 8 = the primary's stack only */

/* PMU power-sequencer registers, from Samsung's kernel (drivers/soc/samsung/pwrcal/S5E8890/S5E8890-pmusfr.h).
 * Per core: M1 ("MNGS") n at 0x2000 + n*0x80, A53 ("APOLLO") n at 0x2200 + n*0x80. Same slot order as cores[]. */
#define PMU_BASE		0x105C0000ul
#define PMU_CPU(slot)		(0x2000u + (slot) * 0x80u)
#define CPU_CONFIGURATION	0x0
#define CPU_STATUS		0x4
#define CPU_OPTION		0x8
#define CPU_DURATION0		0x10
#define PMU_NONCPU(cluster)	(0x2400u + (cluster) * 0x20u)	/* CONFIGURATION, STATUS, OPTION, DURATION0 as above */
#define PMU_CPUSEQ_OPTION(cl)	(0x2488u + (cl) * 0x20u)
#define PMU_L2(cluster)		(0x2600u + (cluster) * 0x20u)	/* CONFIGURATION, STATUS, OPTION */
#define PMU_UP_SCHEDULER	0x0120u

struct core {
	uint64_t midr, mpidr, revidr, el;
	uint64_t pfr0, pfr1, isar0, isar1, mmfr0, mmfr1, mmfr2, dfr0, ctr, clidr, sctlr;
	uint64_t esr, elr, far;
	uint32_t exc;			/* vector index + 1, 0 = none */
	volatile uint32_t done;
};

static struct core cores[8];
extern volatile uint64_t alive[8];	/* start.S: written by a secondary's first instructions */
static int64_t psci_ret[8];
static int64_t psci_version;
static uint64_t dtb;
static uint64_t primary_mpidr = ~0ul;
static volatile uint32_t reporting;

static const uint64_t mpidrs[8] = { 0x0, 0x1, 0x2, 0x3, 0x100, 0x101, 0x102, 0x103 };

#define read_sysreg(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " #r : "=r"(_v)); _v; })

static unsigned slot_of(uint64_t mpidr)
{
	unsigned s = ((mpidr >> 8) & 0xff) * 4 + (mpidr & 0xff);
	return s > 7 ? 7 : s;
}

static void dc_civac_range(uint64_t start, uint64_t size)
{
	for (uint64_t a = start & ~63ul; a < start + size; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static int64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
			   "memory");
	return (int64_t)x0;
}

static uint64_t ticks(void) { return read_sysreg(cntvct_el0); }
static uint64_t tick_hz(void)
{
	uint64_t hz = read_sysreg(cntfrq_el0);
	return hz ? hz : 26000000;	/* unprogrammed CNTFRQ: assume the 8890's 26 MHz oscillator */
}

static void delay_ms(uint64_t ms)
{
	uint64_t end = ticks() + tick_hz() / 1000 * ms;
	while (ticks() < end)
		;
}

static void fb_band(unsigned index, uint32_t color)
{
	volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;
	unsigned y0 = 40 + index * 200;
	for (unsigned y = y0; y < y0 + 150 && y < FB_HEIGHT; y++)
		for (unsigned x = 0; x < FB_WIDTH; x++)
			fb[y * FB_WIDTH + x] = color;
}

static void read_self(struct core *c)
{
	c->midr = read_sysreg(midr_el1);
	c->mpidr = read_sysreg(mpidr_el1);
	c->revidr = read_sysreg(revidr_el1);
	c->el = read_sysreg(CurrentEL) >> 2;
	c->pfr0 = read_sysreg(id_aa64pfr0_el1);
	c->pfr1 = read_sysreg(id_aa64pfr1_el1);
	c->isar0 = read_sysreg(id_aa64isar0_el1);
	c->isar1 = read_sysreg(id_aa64isar1_el1);
	c->mmfr0 = read_sysreg(id_aa64mmfr0_el1);
	c->mmfr1 = read_sysreg(id_aa64mmfr1_el1);
	c->mmfr2 = read_sysreg(s3_0_c0_c7_2);	/* ID_AA64MMFR2_EL1; RAZ on ARMv8.0 */
	c->dfr0 = read_sysreg(id_aa64dfr0_el1);
	c->ctr = read_sysreg(ctr_el0);
	c->clidr = read_sysreg(clidr_el1);
	c->sctlr = c->el == 2 ? read_sysreg(sctlr_el2) : read_sysreg(sctlr_el1);
}

static uint32_t pmu_rd(uint32_t off) { return *(volatile uint32_t *)(PMU_BASE + off); }
static void pmu_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(PMU_BASE + off) = v; }

static void pmu_setf(uint32_t off, unsigned pos, uint32_t mask, uint32_t value)
{
	pmu_wr(off, (pmu_rd(off) & ~(mask << pos)) | (value << pos));
}

/* The CPU part of Samsung's pwrcal_syspwr_init (S5E8890-syspwr.c), which its kernel runs before starting any
 * secondary: init_pmu_l2_option, init_pmu_cpu_option, init_pmu_up_scheduler. Only power-sequencer options;
 * no voltage, clock or reset register is touched. */
static void pmu_cpu_init(void)
{
	for (unsigned cl = 0; cl < 2; cl++) {
		uint32_t opt = PMU_NONCPU(cl) + CPU_OPTION;
		pmu_wr(opt, (pmu_rd(opt) & ~(1u << 1)) | (1u << 0));		/* SC_COUNTER, not SC_FEEDBACK */
		pmu_setf(PMU_NONCPU(cl) + CPU_DURATION0, 8, 0xf, 3);
		pmu_setf(PMU_NONCPU(cl) + CPU_DURATION0, 4, 0xf, 3);
		uint32_t l2 = PMU_L2(cl) + CPU_OPTION;
		pmu_wr(l2, (pmu_rd(l2) & ~((1u << 17) | (1u << 4))) | (1u << 16));	/* no auto L2 flush/retention; STANDBYWFIL2 */
	}
	for (unsigned s = 0; s < 8; s++) {
		uint32_t opt = PMU_CPU(s) + CPU_OPTION;
		uint32_t v = pmu_rd(opt);
		v &= ~((1u << 31) | (1u << 30) | (1u << 27) | (1u << 26) | (1u << 25) | (1u << 24) | (1u << 20) |
		       (1u << 17) | (1u << 15) | (1u << 12) | (1u << 9) | (1u << 5) | (1u << 4) | (1u << 2) | (1u << 1));
		v |= (1u << 28) | (1u << 16) | (1u << 8) | (1u << 3) | (1u << 0);
		/* = USE_SMPEN, USE_STANDBYWFI, ENABLE_AUTO_WAKEUP_INT_REQ, USE_MEMPWR_FEEDBACK, USE_SC_COUNTER */
		pmu_wr(opt, v);
		pmu_setf(PMU_CPU(s) + CPU_DURATION0, 8, 0xf, 3);
		pmu_setf(PMU_CPU(s) + CPU_DURATION0, 4, 0xf, 3);
	}
	pmu_wr(PMU_UP_SCHEDULER, pmu_rd(PMU_UP_SCHEDULER) | 0x3);	/* limit in-rush current, both clusters */
	__asm__ volatile("dsb sy" ::: "memory");
}

struct pmu_snapshot {
	uint32_t cpu[8][4];	/* CONFIGURATION, STATUS, OPTION, DURATION0 */
	uint32_t noncpu[2][4];
	uint32_t l2[2][3];
	uint32_t cpuseq[2];
	uint32_t up_scheduler;
};

static struct pmu_snapshot pmu_after;
static uint32_t cpu_status_after_on[8], cpu_status_after_pwrup[8];
static uint64_t alive_before_pwrup[8];

static void pmu_snap(struct pmu_snapshot *p)
{
	static const uint32_t regs[4] = { CPU_CONFIGURATION, CPU_STATUS, CPU_OPTION, CPU_DURATION0 };
	for (unsigned s = 0; s < 8; s++)
		for (unsigned r = 0; r < 4; r++)
			p->cpu[s][r] = pmu_rd(PMU_CPU(s) + regs[r]);
	for (unsigned cl = 0; cl < 2; cl++) {
		for (unsigned r = 0; r < 4; r++)
			p->noncpu[cl][r] = pmu_rd(PMU_NONCPU(cl) + regs[r]);
		for (unsigned r = 0; r < 3; r++)
			p->l2[cl][r] = pmu_rd(PMU_L2(cl) + regs[r]);
		p->cpuseq[cl] = pmu_rd(PMU_CPUSEQ_OPTION(cl));
	}
	p->up_scheduler = pmu_rd(PMU_UP_SCHEDULER);
}

void secondary_main(void)
{
	struct core *c = &cores[slot_of(read_sysreg(mpidr_el1))];
	read_self(c);
	__asm__ volatile("dsb sy" ::: "memory");
	c->done = 1;
	__asm__ volatile("dsb sy; sev" ::: "memory");
}

/* --- the report --- */

static char report[4096 - 12];
static unsigned report_len;

static void put(const char *s)
{
	while (*s && report_len < sizeof(report) - 1)
		report[report_len++] = *s++;
}

static void put_hex(uint64_t v)
{
	char b[19] = "0x";
	int n = 0;
	for (int shift = 60; shift >= 0; shift -= 4) {
		unsigned d = (v >> shift) & 0xf;
		if (d || n || shift == 0)
			b[2 + n++] = "0123456789abcdef"[d];
	}
	b[2 + n] = 0;
	put(b);
}

static void put_kv(const char *k, uint64_t v)
{
	put(" ");
	put(k);
	put("=");
	put_hex(v);
}

static void report_core(unsigned slot)
{
	struct core *c = &cores[slot];
	put("cpu");
	put_hex(slot);
	put_kv("target", mpidrs[slot]);
	put_kv("psci", (uint64_t)psci_ret[slot]);
	put_kv("done", c->done);
	put_kv("alive", alive[slot]);
	put_kv("stat_on", cpu_status_after_on[slot]);
	put_kv("alive_on", alive_before_pwrup[slot]);
	put_kv("stat_pwrup", cpu_status_after_pwrup[slot]);
	if (c->done || c->midr) {
		/* TGran16 [23:20]: 1 = supported; TGran4 [31:28] and TGran64 [27:24]: 0 = supported */
		put_kv("TGran16", (c->mmfr0 >> 20) & 0xf);
		put_kv("TGran4", (c->mmfr0 >> 28) & 0xf);
		put_kv("TGran64", (c->mmfr0 >> 24) & 0xf);
		put_kv("PARange", c->mmfr0 & 0xf);
		put("\n ");
		put_kv("midr", c->midr);
		put_kv("mpidr", c->mpidr);
		put_kv("revidr", c->revidr);
		put_kv("el", c->el);
		put_kv("sctlr", c->sctlr);
		put("\n ");
		put_kv("mmfr0", c->mmfr0);
		put_kv("mmfr1", c->mmfr1);
		put_kv("mmfr2", c->mmfr2);
		put("\n ");
		put_kv("pfr0", c->pfr0);
		put_kv("pfr1", c->pfr1);
		put_kv("isar0", c->isar0);
		put_kv("isar1", c->isar1);
		put("\n ");
		put_kv("dfr0", c->dfr0);
		put_kv("ctr", c->ctr);
		put_kv("clidr", c->clidr);
	}
	if (c->exc) {
		put("\n  EXCEPTION");
		put_kv("vec", c->exc - 1);
		put_kv("esr", c->esr);
		put_kv("elr", c->elr);
		put_kv("far", c->far);
	}
	put("\n");
}

/* Compact: one line per core (cfg/stat/opt/dur0) and per cluster, before -> after pmu_cpu_init. */
static void report_pmu(const char *label, const struct pmu_snapshot *p)
{
	put("pmu ");
	put(label);
	put(" upsch=");
	put_hex(p->up_scheduler);
	for (unsigned s = 0; s < 8; s++) {
		put("\n c");
		put_hex(s);
		for (unsigned r = 0; r < 4; r++) {
			put(" ");
			put_hex(p->cpu[s][r]);
		}
	}
	for (unsigned cl = 0; cl < 2; cl++) {
		put("\n nc");
		put_hex(cl);
		for (unsigned r = 0; r < 4; r++) {
			put(" ");
			put_hex(p->noncpu[cl][r]);
		}
		put(" l2");
		for (unsigned r = 0; r < 3; r++) {
			put(" ");
			put_hex(p->l2[cl][r]);
		}
		put_kv("seq", p->cpuseq[cl]);
	}
	put("\n");
}

static void write_report_and_reset(uint64_t self_slot)
{
	reporting = 1;
	report_len = 0;
	put("HAKIOS-PROBE v1\n");
	put_kv("boot_slot", self_slot);
	put_kv("dtb", dtb);
	put_kv("psci_version", (uint64_t)psci_version);
	put_kv("cntfrq", tick_hz());
	put("\n");
	for (unsigned s = 0; s < 8; s++)
		report_core(s);
	report_pmu("after-init", &pmu_after);
	put("HAKIOS-PROBE end\n");

	for (unsigned i = 0; i < PSTORE_NSLOTS; i++) {
		uint64_t base = PSTORE_SLOTS + i * PSTORE_SLOT_SZ;
		volatile uint32_t *hdr = (volatile uint32_t *)base;
		volatile uint8_t *data = (volatile uint8_t *)(base + 12);
		dc_civac_range(base, PSTORE_SLOT_SZ);
		for (unsigned k = 0; k < report_len; k++)
			data[k] = (uint8_t)report[k];
		hdr[1] = report_len;	/* start */
		hdr[2] = report_len;	/* size */
		hdr[0] = PERSISTENT_RAM_SIG;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	fb_band(11, 0xFFFFFFFF);
	delay_ms(3000);
	*(volatile uint32_t *)PMU_SWRESET = 1;	/* INFORM3 was set to recovery at entry */
	for (;;)
		__asm__ volatile("wfi");
}

void exception_main(uint64_t vec, uint64_t esr, uint64_t elr, uint64_t far)
{
	uint64_t mpidr = read_sysreg(mpidr_el1);
	struct core *c = &cores[slot_of(mpidr)];
	c->exc = (uint32_t)vec + 1;
	c->esr = esr;
	c->elr = elr;
	c->far = far;
	__asm__ volatile("dsb sy" ::: "memory");
	/* A secondary that faults just stops; the primary times out on it and reports the exception. */
	if (mpidr != primary_mpidr)
		return;
	if (reporting)	/* faulted while reporting: reset without a report */
		*(volatile uint32_t *)PMU_SWRESET = 1;
	write_report_and_reset(slot_of(mpidr));
}

void probe_main(uint64_t dtb_pa)
{
	dtb = dtb_pa;
	primary_mpidr = read_sysreg(mpidr_el1);
	unsigned self = slot_of(primary_mpidr);
	fb_band(0, 0xFFFFFFFF);

	read_self(&cores[self]);
	cores[self].done = 1;
	fb_band(1 + self, 0xFFFFFFFF);

	psci_version = smc(PSCI_VERSION, 0, 0, 0);

	pmu_cpu_init();
	pmu_snap(&pmu_after);

	extern char secondary_entry[];
	for (unsigned s = 0; s < 8; s++) {
		if (s == self)
			continue;
		psci_ret[s] = smc(PSCI_CPU_ON, mpidrs[s], (uint64_t)secondary_entry, 0);
		delay_ms(50);
		cpu_status_after_on[s] = pmu_rd(PMU_CPU(s) + CPU_STATUS);
		alive_before_pwrup[s] = alive[s];
		/* Samsung's EL3 answers CPU_ON with 0 but leaves the core off (probe v3): power it the way
		 * exynos_cpu_up() does, CONFIGURATION[3:0] = 0xF, and see whether it then arrives at our entry. */
		if (!alive[s])
			pmu_wr(PMU_CPU(s) + CPU_CONFIGURATION, pmu_rd(PMU_CPU(s) + CPU_CONFIGURATION) | 0xF);
		uint64_t end = ticks() + tick_hz() * 2;
		while (!cores[s].done && !cores[s].exc && ticks() < end)
			;
		cpu_status_after_pwrup[s] = pmu_rd(PMU_CPU(s) + CPU_STATUS);
		fb_band(1 + s, cores[s].done ? 0xFFFFFFFF : 0xFF404040);
	}
	delay_ms(2000);	/* late arrivals */
	for (unsigned s = 0; s < 8; s++)
		fb_band(1 + s, cores[s].done ? 0xFFFFFFFF : 0xFF404040);
	write_report_and_reset(self);
}
