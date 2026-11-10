/*
 * More than one processor on generic (non-Apple) ARMv8 boards.
 *
 * On Apple hardware the other cores are powered up by closed drivers. Here
 * the firmware does it, through the standard power interface of ARM systems
 * (PSCI): CPU_ON starts a core at an address of the caller's choice, CPU_OFF
 * stops the calling core for good. The address we give is the kernel's own
 * reset vector (LowResetVectorBase in start.s), so that a new core takes the
 * path Apple's cores take: it finds its cpu data by its MPIDR, turns the MMU
 * on and comes out in arm_init_cpu(). The calls are made with SMC or, under
 * a hypervisor that implements them itself, with HVC; the boot loader says
 * which (/chosen/psci-method).
 *
 * The platform expert (iokit/Kernel/GenericARMPE.cpp) registers the cores of
 * the device tree and starts them here, one after the other. Signals between
 * processors are interrupts of the GIC (gic_v3_irq.c).
 *
 * A new core begins with its MMU and caches off, and what it reads then it
 * reads from memory. On real hardware that is not the same as what the
 * running processors see: under Qualcomm's hypervisor (the phone this was
 * written for) a line that is dirty in another core's cache is simply not
 * there yet. QEMU has no cache and shows none of this. So before CPU_ON
 * everything that core will read before its MMU is on is written back to
 * memory (psci_smp_start_cpu()), and what it writes in that state is read
 * only after throwing away the reader's cached copy.
 *
 * The other half is the way out. The kernel ends by returning to the boot
 * loader (loader_return.c), and the loader gets back only what it started:
 * one core, the boot processor's. psci_smp_stop_others() makes every other
 * core switch itself off first, wherever it is.
 */
#include <stdarg.h>
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/misc_protos.h>
#include <libkern/kernel_mach_header.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <arm64/psci_smp.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <vm/pmap.h>

#if defined(GENERIC_PSCI_SMP)

#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON             0xC4000003u
#define PSCI_AFFINITY_INFO      0xC4000004u
#define PSCI_AFFINITY_OFF       1

#define PSCI_SMP_START_SECONDS  5       /* how long a started core is waited for */
#define PSCI_SMP_STOP_SECONDS   3       /* ... and the cores that were told to stop */

extern void loader_return(void);
extern void loader_log_putc(char c);
extern void loader_log_line(const char *line);
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);
extern unsigned long gVirtBase, gPhysBase;
/*
 * HakiOS (patch 1508): asked before every CPU_ON; anything but 0 and that core is not started. The Galaxy S9's big
 * cores need six PMU fields set first (drivers/core/hakios.exynos_m3.c, m3_prep=); elsewhere, and with that driver
 * left out, it is a stub that says go ahead (hakios.stubs.c).
 */
extern int hakios_psci_cpu_on_prepare(int cpu, uint32_t mpidr);
/*
 * HakiOS (patch 1513): told after a CPU_ON the firmware granted. The Exynos 8890's EL3 grants it and leaves the core off:
 * the PMU powers it (devices/gracerlte/soc/drivers/core/hakios.exynos8890_cpu.c); elsewhere a stub that does nothing.
 */
extern void hakios_psci_cpu_on_started(int cpu, uint32_t mpidr);

/* The firmware is called with HVC, not SMC. (start.s reads this too, at its physical address.) */
uint32_t psci_smp_hvc;
/*
 * HakiOS (patch 1515): the CPU_ON entry, when the loader gives one (/chosen/psci-cpu-on-entry): a trampoline of its own
 * below 4 GB that goes on to LowResetVectorBase. The Exynos 8890's EL3 keeps only an entry's low 32 bits.
 */
static uint64_t psci_smp_entry;

/*
 * Left by a core that took an exception between its first instruction and the kernel's vectors
 * (Lsecondary_early_exception in start.s). That core writes it with its cache off; it has a
 * cache line to itself so that looking at memory does not disturb anything else.
 */
struct psci_smp_start_fault {
	uint64_t valid, mpidr, esr, elr, far, lr, sp, spsr;
	uint64_t tpidr, ttbr0, ttbr1, tcr, sctlr, vbar, x21, source;   /* patch 1509 */
} psci_smp_start_fault __attribute__((aligned(64)));
_Static_assert(sizeof(struct psci_smp_start_fault) == 128, "two cache lines, as start.s writes them");

/*
 * HakiOS (patch 1509), boot-arg m3_diag=1: a core being started keeps the physical early vectors until it has its
 * thread (start.s, psci_smp_diag_vectors()), and an exception sleh takes on it before it has joined the scheduler is
 * recorded too (psci_smp_diag_sleh()). Either way the core parks (WFE, interrupts off) and the boot processor says
 * everything about it in the log, then goes on without it. start.s reads this with its MMU off.
 */
uint32_t psci_smp_diag;
extern vm_offset_t vm_kernel_slide;
extern uint64_t cpu_ttep;
extern void ExceptionVectorsBase;
extern uint32_t hakios_pan_off;         /* osfmk/arm/arm_init.c, patch 1509 */

static uint32_t psci_smp_cpu_stage[MAX_CPUS];   /* PSCI_SMP_CPU_*, by cpu number */
uint32_t hakios_m3_park;                        /* start.s: a big core parks after writing its IDs (m3_ids=1) */
static uint32_t psci_smp_started;               /* a bit per core that CPU_ON was granted for */

static int      psci_smp_stop_owner = -1;       /* the processor that began the return to the boot loader */
static bool     psci_smp_boot_wanted;           /* it was not the boot processor: that one is to finish */
static bool     psci_smp_boot_busy;             /* ... and has begun to */
static bool     psci_smp_stopping;              /* every other core switches itself off */

static int64_t
psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = arg3;

	if (psci_smp_hvc) {
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	} else {
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	/* HakiOS (patch 1514): PSCI's answers are 32-bit; Samsung's EL3 leaves x0's top half 0 (-1 reads 0xFFFFFFFF) */
	return (int32_t)x0;
}

static const char *
psci_error(int64_t status)
{
	static const char *const names[] = {
		"success", "not supported", "invalid parameters", "denied", "already on", "on pending",
		"internal failure", "not present", "disabled", "invalid address",
	};

	return (status <= 0 && status > -(int64_t)(sizeof(names) / sizeof(names[0]))) ? names[-status] : "?";
}

/*
 * End the machine through the firmware: SYSTEM_OFF, or SYSTEM_RESET with reset. Under crosvm
 * (AVF) either one ends the VM, and the VM's owner is told how it stopped. Returns only if the
 * firmware refuses.
 */
#define PSCI_SYSTEM_OFF         0x84000008u
#define PSCI_SYSTEM_RESET       0x84000009u

int64_t
psci_system_off(int reset)
{
	DTEntry chosen;
	void const *prop;
	unsigned int size;

	/* (psci_smp_init() runs only when there are more processors to start: ask the device tree here) */
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "psci-method", &prop, &size) != kSuccess || size < 3) {
		return -1;                      /* no PSCI firmware described */
	}
	psci_smp_hvc = strncmp((const char *)prop, "hvc", 3) == 0;
	return psci_call(reset ? PSCI_SYSTEM_RESET : PSCI_SYSTEM_OFF, 0, 0, 0);
}

void
psci_smp_init(void)
{
	DTEntry chosen;
	void const *prop;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "psci-method", &prop, &size) == kSuccess &&
	    size >= 3 && strncmp((const char *)prop, "hvc", 3) == 0) {
		psci_smp_hvc = 1;
	}
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "psci-cpu-on-entry", &prop, &size) == kSuccess && size == sizeof(uint64_t)) {
		memcpy(&psci_smp_entry, prop, sizeof(psci_smp_entry));
	}
}

/* A processor other than the boot processor says how far it has come. */
void
psci_smp_cpu_reached(uint32_t stage)
{
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], stage, release);
}

/* ... all the way: from GenericARMCPU::initCPU(), where the scheduler takes it. */
void
psci_smp_cpu_running(void)
{
	psci_smp_cpu_reached(PSCI_SMP_CPU_RUNNING);
}

static void
psci_smp_clean_segment(const char *name)
{
	unsigned long size = 0;
	vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);

	if (start != 0 && size != 0) {
		CleanPoC_DcacheRegion(start, size);
	}
}

/* ---- patch 1509: m3_diag=1, m3_flush=1 ---- */

static void psci_smp_step(const char *fmt, ...) __printflike(1, 2);
static uint64_t psci_smp_us(uint64_t abs);

static const char *
psci_smp_ec_name(uint32_t ec)
{
	switch (ec) {
	case 0x00: return "unknown instruction: UNDEFINED here (an ARMv8.1+ instruction on an ARMv8.0 core?)";
	case 0x01: return "WFI/WFE trapped";
	case 0x07: return "FP/SIMD access trapped (CPACR)";
	case 0x0e: return "illegal execution state";
	case 0x16: return "HVC";
	case 0x17: return "SMC";
	case 0x18: return "MSR/MRS/system instruction trapped";
	case 0x20: return "instruction abort from EL0";
	case 0x21: return "instruction abort at EL1";
	case 0x22: return "PC alignment";
	case 0x24: return "data abort from EL0";
	case 0x25: return "data abort at EL1";
	case 0x26: return "SP alignment";
	case 0x2f: return "SError";
	case 0x3c: return "BRK";
	default:   return "?";
	}
}

/* An address the faulting core gave: physical (start.s, MMU off or through the V=P map) or a kernel one; as unslid. */
static uint64_t
psci_smp_unslid(uint64_t a)
{
	if (a >= gPhysBase && a < gPhysBase + 0x40000000ULL) {
		a += gVirtBase - gPhysBase;
	}
	return a >= VM_MIN_KERNEL_AND_KEXT_ADDRESS ? a - vm_kernel_slide : 0;
}

/* The boot processor, just before CPU_ON: the flags start.s reads with its MMU off, and m3_flush=1. */
static void
psci_smp_diag_before(int cpu, uint32_t phys_id)
{
	static bool asked;
	uint32_t v = 0;

	if (!asked) {
		asked = true;
		if (PE_parse_boot_argn("m3_diag", &v, sizeof(v)) && v != 0) {
			psci_smp_diag = 1;
		}
		psci_smp_step("psci: m3_diag=%u; PAN %s (boot core ID_AA64MMFR1 0x%llx; pan=0 turns it off on every core)\n",
		    psci_smp_diag, hakios_pan_off ? "OFF" : "on", __builtin_arm_rsr64("S3_0_C0_C7_1"));
	}
	bzero(&psci_smp_start_fault, sizeof(psci_smp_start_fault));
	FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
	FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_diag, sizeof(psci_smp_diag));
	FlushPoC_DcacheRegion((vm_offset_t)&hakios_pan_off, sizeof(hakios_pan_off));
	/*
	 * m3_flush=1 (a test of cluster coherency): everything this core's caches hold, written back to memory and
	 * dropped, by set/way at every level CLIDR names, just before the new core is started: if the big cluster
	 * did not see the little one's caches, page tables and data would then be right in memory all the same.
	 */
	v = 0;
	if (PE_parse_boot_argn("m3_flush", &v, sizeof(v)) && v != 0) {
		uint64_t t = mach_absolute_time();

		FlushPoC_Dcache();
		__builtin_arm_dsb(DSB_SY);
		psci_smp_step("psci: m3_flush: cpu %d (MPIDR 0x%x): the boot core's whole data cache cleaned to PoC in %llu us\n",
		    cpu, phys_id, psci_smp_us(mach_absolute_time() - t));
	}
}

/* The boot processor, after a core being started left its record (m3_diag=1). */
static void
psci_smp_diag_report(int cpu, cpu_data_t *cdp, uint32_t stage)
{
	struct psci_smp_start_fault *f = &psci_smp_start_fault;
	uint32_t ec = (uint32_t)(f->esr >> 26) & 0x3f, iss = (uint32_t)f->esr & 0x1ffffff;
	uint64_t elr_u = psci_smp_unslid(f->elr), insn_va = 0;
	uint32_t insn = 0;

	psci_smp_step("psci: m3_diag: cpu %d (MPIDR 0x%llx) took its first exception %s (stage %u), and is parked\n",
	    cpu, f->mpidr, f->source == 1 ? "in the early vectors (before it had its thread)" : "in sleh (it had its thread)",
	    stage);
	psci_smp_step("psci: m3_diag:   ESR 0x%llx: EC 0x%x %s; ISS 0x%x; FAR 0x%llx\n", f->esr, ec, psci_smp_ec_name(ec), iss,
	    f->far);
	if (ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21) {
		psci_smp_step("psci: m3_diag:   abort: FSC 0x%x (0x04-0x07 translation L0-L3, 0x09-0x0b access flag, 0x0d-0x0f "
		    "permission, 0x10 external, 0x21 alignment) WnR %u S1PTW %u\n", iss & 0x3f, (iss >> 6) & 1, (iss >> 7) & 1);
	}
	if (ec == 0x18) {
		psci_smp_step("psci: m3_diag:   trapped: %s S%u_%u_C%u_C%u_%u (x%u)\n", (iss & 1) ? "MRS" : "MSR", (iss >> 20) & 3,
		    (iss >> 14) & 7, (iss >> 10) & 0xf, (iss >> 1) & 0xf, (iss >> 17) & 7, (iss >> 5) & 0x1f);
	}
	/* the instruction itself, read through the kernel's own mapping of its text */
	if (elr_u != 0) {
		insn_va = elr_u + vm_kernel_slide;
		insn = *(volatile uint32_t *)insn_va;
	}
	psci_smp_step("psci: m3_diag:   ELR 0x%llx (unslid 0x%llx) insn 0x%08x%s\n", f->elr, elr_u, insn,
	    (insn & 0xfffff0ffu) == 0xd500409fu ? " = MSR PAN, #imm (ARMv8.1 PAN: pan=0)" :
	    (insn & 0xffffffe0u) == 0xd5384260u ? " = MRS Xt, PAN (pan=0)" :
	    (insn & 0xffffffe0u) == 0xd5184260u ? " = MSR PAN, Xt (pan=0)" :
	    (insn & 0x3fa07c00u) == 0x08a07c00u || (insn & 0x3f200c00u) == 0x38200000u ? " = an LSE atomic (ARMv8.1)" : "");
	psci_smp_step("psci: m3_diag:   LR 0x%llx (unslid 0x%llx) SP 0x%llx SPSR 0x%llx\n", f->lr, psci_smp_unslid(f->lr), f->sp,
	    f->spsr);
	psci_smp_step("psci: m3_diag:   TPIDR_EL1 0x%llx TTBR0 0x%llx TTBR1 0x%llx\n", f->tpidr, f->ttbr0, f->ttbr1);
	psci_smp_step("psci: m3_diag:   TCR 0x%llx SCTLR 0x%llx VBAR 0x%llx x21 0x%llx\n", f->tcr, f->sctlr, f->vbar, f->x21);
	psci_smp_step("psci: m3_diag:   the boot core's TCR 0x%llx SCTLR 0x%llx TTBR1 0x%llx; cpu_ttep 0x%llx\n",
	    __builtin_arm_rsr64("TCR_EL1"), __builtin_arm_rsr64("SCTLR_EL1"), __builtin_arm_rsr64("TTBR1_EL1"), cpu_ttep);
	psci_smp_step("psci: m3_diag:   its cpu data at %p (PA 0x%llx): number %d phys_id 0x%x flags 0x%x cluster %u\n", cdp,
	    (uint64_t)ml_static_vtop((vm_offset_t)cdp), cdp->cpu_number, cdp->cpu_phys_id, cdp->cpu_flags, cdp->cpu_cluster_id);
	psci_smp_step("psci: m3_diag:   active thread %p (TPIDR_EL1 %s) intstack_top 0x%lx excepstack_top 0x%lx\n",
	    cdp->cpu_active_thread, f->tpidr == (uint64_t)cdp->cpu_active_thread ? "matches" :
	    f->tpidr == 0 ? "still 0" : "DIFFERS", (unsigned long)cdp->intstack_top, (unsigned long)cdp->excepstack_top);
}

/*
 * The core being started, in arm_init_cpu() once its thread is in TPIDR_EL1: the kernel's vectors from now on, the ones
 * common_start would have given it (m3_diag=1 only; start.s left it the physical early ones).
 */
void
psci_smp_diag_vectors(void)
{
	if (psci_smp_diag != 0) {
		__builtin_arm_wsr64("VBAR_EL1", (uint64_t)&ExceptionVectorsBase);
		__builtin_arm_isb(ISB_SY);
	}
}

/*
 * Called first thing by sleh_synchronous() when m3_diag=1: if this is a core being started that has not joined the
 * scheduler yet, record the exception and park here for good. Anything else returns at once.
 */
void
psci_smp_diag_sleh(uint32_t esr, uint64_t far, uint64_t elr, uint64_t spsr, uint64_t lr, uint64_t sp)
{
	struct psci_smp_start_fault *f = &psci_smp_start_fault;
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	int cpu = ml_get_cpu_number(ml_phys_id_from_mpidr(mpidr));
	uint32_t stage;

	if (cpu < 0 || cpu >= MAX_CPUS || cpu == master_cpu) {
		return;
	}
	stage = os_atomic_load(&psci_smp_cpu_stage[cpu], acquire);
	if (stage == 0 || stage >= PSCI_SMP_CPU_RUNNING) {
		return;
	}
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	f->mpidr = mpidr;
	f->esr = esr;
	f->elr = elr;
	f->far = far;
	f->lr = lr;
	f->sp = sp;
	f->spsr = spsr;
	f->tpidr = __builtin_arm_rsr64("TPIDR_EL1");
	f->ttbr0 = __builtin_arm_rsr64("TTBR0_EL1");
	f->ttbr1 = __builtin_arm_rsr64("TTBR1_EL1");
	f->tcr = __builtin_arm_rsr64("TCR_EL1");
	f->sctlr = __builtin_arm_rsr64("SCTLR_EL1");
	f->vbar = __builtin_arm_rsr64("VBAR_EL1");
	f->x21 = 0;
	f->source = 2;
	__builtin_arm_dmb(DMB_SY);
	f->valid = 1;
	FlushPoC_DcacheRegion((vm_offset_t)f, sizeof(*f));
	__builtin_arm_dsb(DSB_SY);
	for (;;) {
		__builtin_arm_wfe();
	}
}

/*
 * Start a registered processor: the platform's half of cpu_start(). Waits until the core has
 * joined the scheduler, and says what became of it if it has not: on a phone the log is all
 * there is to go by.
 */
/*
 * HakiOS (patch 1505): each step of a core's start, straight into the loader's log. On the Galaxy S9 the start of the
 * first big core (an M3, MPIDR 0x100) left the boot processor silent for good (round 4): this says whether the
 * firmware's CPU_ON came back, and how far the new core got.
 */
static void psci_smp_step(const char *fmt, ...) __printflike(1, 2);

static void
psci_smp_step(const char *fmt, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	loader_log_line(line);
}

static uint64_t
psci_smp_us(uint64_t abs)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000;
}

kern_return_t
psci_smp_start_cpu(int cpu)
{
	cpu_data_t *cdp = cpu_datap(cpu);
	uint32_t phys_id = cdp->cpu_phys_id, stage;
	uint64_t entry = psci_smp_entry ? psci_smp_entry : ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	uint64_t now, deadline, t0 = mach_absolute_time(), t1;
	uint32_t seen = 0;
	int64_t status;

	if (hakios_psci_cpu_on_prepare(cpu, phys_id) != 0) {
		psci_smp_step("psci: cpu %d (MPIDR 0x%x) is not started: its preparation says no\n", cpu, phys_id);
		printf("psci: cpu %d (MPIDR 0x%x) was not started: its preparation says no\n", cpu, phys_id);
		return KERN_FAILURE;
	}

	/*
	 * What that core reads with its MMU off: the code and the page tables of start.s, and what
	 * the reset vector finds its way by (ResetHandlerData, CpuDataEntries, the boot arguments,
	 * cpu_ttep, psci_smp_hvc ...), all of it in the kernel's own segments; and the cpu data of
	 * every processor, which the reset vector goes through looking for its own.
	 * Clean, not flush: the processors that are running keep using all of it.
	 */
	psci_smp_start_fault.valid = 0;
	psci_smp_clean_segment("__TEXT");
	psci_smp_clean_segment("__DATA_CONST");
	psci_smp_clean_segment("__TEXT_EXEC");
	psci_smp_clean_segment("__DATA");
	psci_smp_clean_segment("__BOOTDATA");
	for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
		if (cpu_datap(other) != NULL) {
			CleanPoC_DcacheRegion((vm_offset_t)cpu_datap(other), sizeof(cpu_data_t));
		}
	}
	__builtin_arm_dsb(DSB_SY);

	os_atomic_store(&psci_smp_cpu_stage[cpu], PSCI_SMP_CPU_ASKED, release);
	t1 = mach_absolute_time();
	psci_smp_step("psci: cpu %d (MPIDR 0x%x): the kernel's segments cleaned in %llu us; CPU_ON with entry 0x%llx (%s)\n",
	    cpu, phys_id, psci_smp_us(t1 - t0), entry, psci_smp_hvc ? "HVC" : "SMC");
	{
		uint32_t ids = 0;

		if (PE_parse_boot_argn("m3_ids", &ids, sizeof(ids)) && ids != 0 && (phys_id & 0xff00) == 0x100) {
			hakios_m3_park = 1;
			FlushPoC_DcacheRegion((vm_offset_t)&hakios_m3_park, sizeof(hakios_m3_park));
		}
	}
	psci_smp_diag_before(cpu, phys_id);
	status = psci_call(PSCI_CPU_ON, phys_id, entry, 0);
	if (hakios_m3_park) {
		volatile uint64_t *slot = (volatile uint64_t *)ml_io_map(0xFDA10000 + 32 * ((phys_id >> 8) & 0xf), 0x1000);

		for (int i = 0; i < 5; i++) {
			delay_for_interval(20, NSEC_PER_MSEC);
			if (slot != NULL) {
				psci_smp_step("psci: m3_ids: cpu %d: MMFR0 0x%016llx MIDR 0x%llx MPIDR 0x%llx PFR0 0x%016llx\n", cpu, slot[0],
				    slot[1], slot[2], slot[3]);
			}
		}
		psci_smp_step("psci: m3_ids: done; the big core is parked (not started)\n");
		return KERN_FAILURE;
	}
	psci_smp_step("psci: cpu %d: the firmware answered %lld (%s) in %llu us\n", cpu, status, psci_error(status),
	    psci_smp_us(mach_absolute_time() - t1));
	if (status != 0) {
		os_atomic_store(&psci_smp_cpu_stage[cpu], 0, release);
		printf("psci: cpu %d (MPIDR 0x%x) was not started: the firmware answers %lld (%s)\n",
		    cpu, phys_id, status, psci_error(status));
		return KERN_FAILURE;
	}
	os_atomic_or(&psci_smp_started, 1u << cpu, relaxed);
	hakios_psci_cpu_on_started(cpu, phys_id);

	clock_interval_to_deadline(PSCI_SMP_START_SECONDS, NSEC_PER_SEC, &deadline);
	do {
		stage = os_atomic_load(&psci_smp_cpu_stage[cpu], acquire);
		if (psci_smp_diag != 0) {
			FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
			if (psci_smp_start_fault.valid != 0) {
				break;
			}
		}
		if (stage != seen) {
			seen = stage;
			psci_smp_step("psci: cpu %d: stage %u after %llu us\n", cpu, stage, psci_smp_us(mach_absolute_time() - t1));
		}
		if (stage >= PSCI_SMP_CPU_RUNNING) {
			return KERN_SUCCESS;
		}
		/* memory, not this processor's copy of it */
		FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
		if (psci_smp_start_fault.valid != 0) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
		now = mach_absolute_time();
	} while (now < deadline);

	if (psci_smp_start_fault.valid != 0 && psci_smp_diag != 0) {
		psci_smp_diag_report(cpu, cdp, stage);
		os_atomic_andnot(&psci_smp_started, 1u << cpu, relaxed);        /* parked: not to be stopped, nor waited for */
		os_atomic_store(&psci_smp_cpu_stage[cpu], 0, release);
		return KERN_FAILURE;
	}
	if (psci_smp_start_fault.valid != 0) {
		struct psci_smp_start_fault *f = &psci_smp_start_fault;
		uint64_t to_virtual = gVirtBase - gPhysBase;

		printf("psci: cpu %d (MPIDR 0x%llx) took an exception on its way in and has switched itself off: "
		    "ESR 0x%llx ELR 0x%llx FAR 0x%llx LR 0x%llx SP 0x%llx (ELR and LR as kernel addresses: 0x%llx 0x%llx)\n",
		    cpu, f->mpidr, f->esr, f->elr, f->far, f->lr, f->sp, f->elr + to_virtual, f->lr + to_virtual);
		os_atomic_andnot(&psci_smp_started, 1u << cpu, relaxed);        /* nothing of it is left to stop */
	} else {
		printf("psci: cpu %d (MPIDR 0x%x) was started %d seconds ago and has not joined: %s\n",
		    cpu, phys_id, PSCI_SMP_START_SECONDS,
		    stage >= PSCI_SMP_CPU_IN_KERNEL ? "it got as far as arm_init_cpu()" : "no sign of it since the firmware took the request");
	}
	return KERN_FAILURE;
}

/*
 * The calling processor switches itself off. Its interrupted work, if any, is abandoned where
 * it was: only for the end of the system.
 */
void
psci_smp_cpu_off(void)
{
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	/* leave nothing behind that would interrupt a core that is no longer there */
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
	gic_v3_irq_cpu_shutdown();
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], PSCI_SMP_CPU_OFF, release);
	__builtin_arm_dsb(DSB_SY);

	(void) psci_call(PSCI_CPU_OFF, 0, 0, 0);
	for (;;) {
		__builtin_arm_wfi();    /* the firmware refused: stay out of the way all the same */
	}
}

static void
psci_smp_note(const char *what, int cpu)
{
	static const char prefix[] = "\npsci: cpu ";

	for (const char *c = prefix; *c; c++) {
		loader_log_putc(*c);
	}
	loader_log_putc((char)('0' + cpu));
	for (const char *c = what; *c; c++) {
		loader_log_putc(*c);
	}
}

/* Tell every core but this one to switch itself off (psci_smp_stop_here() is where they hear it). */
static void
psci_smp_stop_rest(void)
{
	int me = cpu_number(), max_cpu = ml_get_max_cpu_number();

	os_atomic_store(&psci_smp_stopping, true, release);
	__builtin_arm_dsb(DSB_SY);
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu != me && cdp != NULL && (cpu == master_cpu || (psci_smp_started & (1u << cpu)))) {
			gic_v3_irq_signal_stop(cdp->cpu_phys_id);
		}
	}
	/* for those that wait with their interrupts off (wait_while_mp_kdp_trap(), after a panic) */
	__builtin_arm_sev();
}

/*
 * The system is going back to the boot loader (loader_return()). Returns on the boot processor
 * only, once the other cores are off -- or have been given up on. On any other processor it
 * does not return: that one hands the job to the boot processor, which the loader's side of
 * the return belongs to, and switches itself off like the rest.
 */
void
psci_smp_stop_others(void)
{
	int me, max_cpu;
	uint64_t now, deadline;

	if (psci_smp_started == 0) {
		return;                 /* there is only this one */
	}
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	me = cpu_number();
	max_cpu = ml_get_max_cpu_number();
	if (!os_atomic_cmpxchg(&psci_smp_stop_owner, -1, me, acq_rel) && psci_smp_stop_owner != me && me != master_cpu) {
		psci_smp_cpu_off();     /* another processor is at it already */
	}

	if (me != master_cpu) {
		/*
		 * Nobody is stopped before the boot processor has answered: a core that is switched
		 * off while it holds a lock the boot processor is waiting for would take the return
		 * with it.
		 */
		psci_smp_note(" ends the system: the return to the boot loader is the boot processor's to make\n", me);
		os_atomic_store(&psci_smp_boot_wanted, true, release);
		__builtin_arm_dsb(DSB_SY);
		gic_v3_irq_signal_stop(cpu_datap(master_cpu)->cpu_phys_id);
		__builtin_arm_sev();
		now = mach_absolute_time();
		nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
		deadline += now;
		while (!os_atomic_load(&psci_smp_stopping, acquire) && now < deadline) {
			now = mach_absolute_time();
		}
		if (!os_atomic_load(&psci_smp_stopping, acquire)) {
			/*
			 * It does not hear (stuck with its interrupts off). There is no return without
			 * it; a loader that watches from its own core will find the kernel silent and
			 * take the log as it is, and so this goes into the log.
			 */
			psci_smp_note(" cannot return to the boot loader and the boot processor does not answer: all cores stop here\n", me);
			psci_smp_stop_rest();
		}
		psci_smp_cpu_off();
	}

	psci_smp_stop_rest();
	now = mach_absolute_time();
	nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
	deadline += now;
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu == me || cdp == NULL || !(psci_smp_started & (1u << cpu))) {
			continue;
		}
		while (now < deadline) {
			if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) == PSCI_SMP_CPU_OFF) {
				/* it has said it is going; the firmware knows when it has gone (if it can tell at all) */
				int64_t state = psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0);

				if (state == PSCI_AFFINITY_OFF || state < 0) {
					break;
				}
			}
			now = mach_absolute_time();
		}
		if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) != PSCI_SMP_CPU_OFF &&
		    psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0) != PSCI_AFFINITY_OFF) {
			psci_smp_note(" did not stop: it is left running\n", cpu);
		}
	}
}

/*
 * Where a processor learns that the system is ending: the interrupt psci_smp_stop_rest() sends,
 * and the loop in which the processors wait while another one panics (they have their
 * interrupts off there). Comes back if there is nothing of the kind going on.
 */
void
psci_smp_stop_here(void)
{
	if (psci_smp_started == 0) {
		return;
	}
	if (cpu_number() != master_cpu) {
		if (os_atomic_load(&psci_smp_stopping, acquire)) {
			psci_smp_cpu_off();
		}
		return;
	}
	if (os_atomic_load(&psci_smp_boot_wanted, acquire) && !psci_smp_boot_busy) {
		psci_smp_boot_busy = true;
		loader_return();        /* stops the rest (psci_smp_stop_others()); back only if no loader listens */
		for (;;) {
			__builtin_arm_wfi();
		}
	}
}

#endif /* defined(GENERIC_PSCI_SMP) */
