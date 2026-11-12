/*
 * The Mali GPU of an Exynos phone brought up from the kernel step by step and then handed to HakiosMali
 * (drivers/gpu/hakios.mali.cpp), the IOKit service Mesa's panfrost draws through: the Galaxy S9's Mali-G72 MP18 (Exynos
 * 9810, Bifrost) and the Galaxy Note FE's Mali-T880 MP12 (Exynos 8890, Midgard). Only with boot-arg mali=N, and nothing
 * is touched without it. What belongs to the SoC -- where the GPU is, its supply, clock and power domain -- is a glue's
 * (hakios.mali_glue.h; mali_soc=): drivers/gpu/hakios.exynos9810_g3d.c, devices/gracerlte/soc/drivers/core/
 * hakios.exynos8890_g3d.c. This file is the GPU's own registers, its page tables and its jobs.
 *
 * WHERE IT COMES FROM. This is HakiOS's Mali driver for the Infinix Hot 20 (X6826B: MediaTek MT6768, a Mali-G52 MC2),
 * kernel/bsd/dev/redwood_mali.c of github.com/oosexgang/hakios-mt6768 at commit 93ea250, under the Apple Public Source
 * License 2.0 as every file of that repository's kernel/ is (its NOTICE), and as this tree's drivers/ are. It is renamed
 * (redwood_ to hakios_, kern.redwood to kern.hakios) and MediaTek's power and clocks are replaced by the Exynos glues.
 * What is the original's: the GPU's reset and cores, address space 0 and the GPU's own page tables, the two first jobs,
 * Linux panfrost's parameters, the GPU addresses and what is remembered of each buffer, the queue of job chains, the
 * waits, the kept lines and the minute's report. On the X6826B it drew iOS's window server through Mesa's panfrost and a
 * Metal driver (that repository's README, "GPU: works"). Here it has not run.
 *
 * THE GPU. A Mali of either generation needs no firmware: once its glue has it supplied, clocked and powered, the rest
 * is its own registers, which are Linux panfrost's (drivers/gpu/drm/panfrost/panfrost_regs.h).
 *   G72           GPU_ID >> 16 reads 0x6221: architecture 6.2, revision 2, product 1 ("tHEx"); AArch64-format page
 *                 tables (AS_TRANSCFG); COHERENCY_ENABLE = 31 after a reset
 *   T880          GPU_ID >> 16 reads 0x0880 (the old format: the product, then the version); the Note FE's is r2p0.
 *                 What is Midgard's here, from Samsung's kbase r28p0 for this phone (mali_kbase_mmu_mode_lpae.c,
 *                 mali_kbase_pm_driver.c, mali_base_hwconfig_issues.h: tFRx r2p0) and Linux panfrost:
 *                 - page tables in the LPAE format: no AS_TRANSCFG; AS_TRANSTAB holds the table and its mode, [1:0] 3
 *                   (its 2, identity, still walks a table: no step 7), READ_INNER (bit 2) left clear, so the walk
 *                   reads memory, as mali_fastmap assumes; four levels from level 0 (48 bits of address); a page's
 *                   entry is its address, read and write (bits 6, 7), shareable (bits 9:8 = 2, Linux's
 *                   ARM_LPAE_PTE_SH_OS), attribute 0, type 1 (not 3); a table's is type 3; MEMATTR attribute 0 is 0x48.
 *                   Without the shareable bits the tiler did not see what the vertex job had just written: it binned
 *                   each draw with the positions the draw before had left (runs of 20261108)
 *                 - no COHERENCY_ENABLE (the register is not in tFxx's features); not coherent either
 *                 - after a reset: SHADER_CONFIG SC_LS_ALLOW_ATTR_TYPES (bit 16, T76x to T88x), TILER_CONFIG
 *                   TC_CLOCK_GATE_OVERRIDE (bit 0, issue T76X_3953), JM_CONFIG's job throttle limit at its most (0x3F
 *                   << 3, T86x and T88x from r2p0, the device tree having no jm_config); L2_MMU_CONFIG as on the G72
 *                 - issue 10883 (a job's end may raise no interrupt) needs no workaround here: kbase's checks a slot
 *                   whose next job was queued behind the one that ended, and this driver queues none (one job at a
 *                   time, each waited for)
 *   interrupts    not used: as the original did, the driver asks the registers (a moment of polling, then asleep 200 us
 *                 at a time)
 *   coherency     none: the GPU does not look into the processors' caches. The processor cleans its cache to memory
 *                 around what the GPU reads and writes of the kernel's (job pages, page tables), and the buffers of
 *                 programs are write-combined, as in the original
 *
 * WHAT CHANGED FROM THE ORIGINAL, AND WHY
 *   - MediaTek's clock mux, SPM power domains, bus protection and MFG clock gate are gone; the SoC's steps 1 to 3 are
 *     its glue's (hakios.mali_glue.h), and step 1 looks before anything is written.
 *   - The original's steps ran all at once with mali=1; here mali=N makes the first N of nine, each said before it is
 *     made. If the steps end short of serving, a GPU that has answered is reset (its cores off) and the glue puts back
 *     what steps 2 and 3 changed.
 *   - Its page tables assume 35 bits of GPU address (the G52's MMU_FEATURES, 0x2823). Step 4 reads the G72's and refuses
 *     to go on with fewer, or with fewer than 36 bits of physical address: the S9's memory reaches 0x8FFFFFFFF. On a GPU
 *     of 40 bits or more the walk is given a level 0 above them, as kbase and panfrost walk a G72 (the page tables).
 *   - Step 4 refuses a GPU that is not a G72 or a T880, or whose shader cores do not fit the 32 bits of affinity the
 *     jobs are given.
 *   - COHERENCY_ENABLE = 31 after every reset on a Bifrost GPU, the one of a hung job's recovery included (a reset may
 *     clear it).
 *   - A reset is panfrost's: the reset interrupt cleared first, a hard reset when the soft one does not finish, and the
 *     one setting panfrost makes on a G72 after it: L2_MMU_CONFIG's limits on outstanding external reads and writes
 *     ([25:24], [27:26]) cleared. The cores come up as panfrost brings them: the L2 first and waited for, then the
 *     shader cores, then the tiler.
 *   - A physical address is checked to be memory the kernel manages (pmap_valid_address()) before the kernel's address
 *     of it is asked for and the cache cleaned over it -- a table of the GPU's, found through an entry -- and before a
 *     page is entered into the GPU's tables. phystokv() panics on an address outside, and PixOS, on another phone,
 *     panicked flushing the cache over an address it had not checked.
 *   - The page of the first jobs is taken out of the GPU's tables before it is freed (the original left its entry).
 *   - NR_CORE_GROUPS is the number of L2 caches present, as Linux panfrost answers it, not a fixed 1.
 *   - Programs' buffers get GPU addresses between 32 MB and 4 GB, given back when a buffer is freed, and a buffer of code
 *     never crosses a 16 MB line: Linux panfrost's range and rule, which Mesa's panfrost assumes (a Bifrost blend
 *     shader's address is 32 bits, its upper half the fragment shader's). The original gave 32 GB from 16 MB, never an
 *     address twice, whatever the buffer (hakios.mali_va.h, checked by tools/mali-va-test.sh).
 *   - The steps' lines go to the loader's log as they are said (say()), as this tree's drivers do: the end of a run
 *     that dies in a step is what is kept. The minute's report and the per-job lines stay in the kernel's log only.
 *   - Waits are bounded by the clock, not by a count of delay()s (which may each sleep far longer than asked).
 *
 * THE STEPS. mali=N makes the first N and stops; 0, or no mali= at all, leaves the GPU alone. A step says what it will
 * touch before it does, and one that does not find what it needs ends the sequence:
 *   1  reads   the glue's look: what the boot left of the supply, the clock and the domain. THE GPU IS NOT READ
 *   2  writes  the glue's power: whatever it takes (and is allowed) until the GPU can be read
 *   3  asks    the glue's after_power: what a power-on of step 2's own needs after it (the 9810's DTZPC restore)
 *   4  reads   GPU_ID (a G72 or a T880, or nothing more is done), MMU_FEATURES (35 bits of GPU address at least), the
 *              cores
 *   5  writes  a reset (soft; hard if that does not finish); then what the generation wants after one (above)
 *   6  writes  the L2, then the shader cores, then the tiler powered up
 *   7  runs    one job, the smallest there is (write a value into memory), through address space 0 in identity mode
 *              (not on a T880, which has no such mode)
 *   8  runs    the same job through the GPU's own page tables, at a GPU address of their making
 *   9  serves  HakiosMali started: programs can open the GPU (the original's "step 6 of 6" and RedwoodMaliStart())
 * sysctl kern.hakios.mali says the last step done: 9 is the GPU there for programs.
 *
 * Boot-args:
 *   mali=N           the first N of the GPU's nine steps of bring-up (9: all, then served); 0 or none: left alone
 *   mali_soc=NAME    the SoC's glue: exynos9810 (the Galaxy S9's, the default) or exynos8890 (the Note FE's)
 *                    (a glue's own boot-args are in its file)
 *   mali_fastmap=0   the GPU walks its tables through its own caches, and every buffer entered is an UPDATE of AS 0
 *   mali_trace=1     a line for every job: "tl: j SEQ f|v q QUEUED b BEGAN e ENDED", in microseconds
 *   mali_gov=1       the GPU's clock follows its load (gov_window()) between the glue's rates (the 9810's: 260, 455 or
 *                    572 MHz): the lowest at which the last 16 ms would have kept it busy half the time at most -- up
 *                    at once, down after five such windows in a row --, and no more than the second rate from 85 C on
 *                    the G3D's sensor, the first from 95 (5 C back). Only on a glue that can switch its clock
 *   mali_nap_us=US   a job not done after its first 200 us of polling is looked at every US microseconds (50 unless
 *                    said; 200 before): the GPU's interrupts are not used, and a job's end was seen 100 us late on
 *                    average, a frame of CoreAnimation's being a dozen jobs and more
 *   mali_peek=VA[,VA...]
 *                    after every job chain, the 128 bytes the GPU left at each GPU address VA (up to 4, 128-aligned),
 *                    read through its tables, to the loader's log: what a job wrote, where a program's trace
 *                    (PAN_MESA_DEBUG=trace) says it goes
 *
 * The user client is opened by the window server's process, backboardd, which does not run as root (PixOS learned that
 * on another phone): nothing here or in hakios.mali.cpp asks for root.
 *
 * Status: on the S9, written, not yet run on the phone; the S9+ port's sequence has run panfrost on the same SoC from
 * S-Boot's state, and what is ours (the steps, the jobs, the tables) has run only on the X6826B's G52. On the Note FE
 * (its glue: S-Boot's 260 MHz, nothing of the G3D block written), all nine steps, and Mesa's panfrost draws through
 * HakiosMali: gl_test's triangle, right to the pixel, at 1080 x 2400 (run 20261108T190222).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

#include "hakios.mali_glue.h"
#include "hakios.mali_va.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);                       /* pmap_paddr_t */
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */
extern vm_offset_t phystokv(uint64_t pa);
extern boolean_t pmap_valid_address(uint64_t addr);             /* memory the kernel manages */
extern int hakios_scanout_carved(uint64_t phys);                /* the DPU's pool (hakios.exynos_dpu.c; stubs.c) */
extern void HakiosMaliStart(void);                              /* drivers/gpu/hakios.mali.cpp */
extern uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;   /* hakios.mali.cpp */
extern void HakiosMaliCacheSecond(void);
extern void HakiosMaliCallsSecond(void);
extern struct mali_made { uint64_t address, size; uint32_t asked, flags; } gMaliMade[32];

/* (every line of the steps to the loader's log as well, as the other drivers do: see hakios.buttons.c) */
extern void loader_log_line(const char *line);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#define MALI_SIZE               0x5000
#define MALI_G72                0x6221u         /* GPU_ID >> 16 */
#define MALI_T880               0x0880u

SYSCTL_DECL(_kern_hakios);              /* the node is drivers/core/hakios.platform.c's */

static const struct hakios_mali_glue *glue;
static vm_offset_t mali;
static uint32_t reached, gpu_id, shader_mask, l2_mask, last_step, step;
static bool gpu_answered;               /* step 4 read a GPU it knows: a reset if the steps end short */
static bool midgard;                    /* a T880: the LPAE page tables, Midgard's settings after a reset */
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &reached, 0,
    "how far the Mali came up: 9 is the GPU there for programs (drivers/gpu/hakios.mali.c)");
/*
 * What a program said about the GPU (the original's userland/src/mali_test.c writes it): repeated in the log once a
 * minute, so that it lands in the end of a run's log, which is the part that is kept.
 */
static uint32_t user_said;
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_user, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &user_said, 0,
    "what user space found: 1 a job ran, 2 no service, 3 no client, 4 no buffer, 5 no mapping, 6 the job did nothing");

/*
 * What gl_test drew, for the log's end as well: who drew it (1 the Mali, 2 the processor) times 1000 plus the share of
 * the picture inside the triangle in percent; and the middle pixel, red in the low byte.
 */
/*
 * kern.hakios.say: a line from a program, into the kernel's log (a job's own output is kept nowhere):
 * sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line)).
 */
#define KEPT_LINES 96
static char kept_lines[KEPT_LINES][200];
static uint32_t kept_next;
static LCK_GRP_DECLARE(kept_lock_group, "hakios-said");
static LCK_MTX_DECLARE(kept_lock, &kept_lock_group);

/* A line from the kernel itself, kept and said again every minute as kern.hakios.say's "!" ones are. */
void
hakios_keep(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
	lck_mtx_lock(&kept_lock);
	strlcpy(kept_lines[kept_next % KEPT_LINES], line, sizeof(kept_lines[0]));
	kept_next++;
	lck_mtx_unlock(&kept_lock);
}

static int
say_line(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char line[512];
	size_t length = req->newlen;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if (length > sizeof(line) - 1) {
		length = sizeof(line) - 1;
	}
	if (SYSCTL_IN(req, line, length) != 0) {
		return EINVAL;
	}
	line[length] = 0;
	printf("%s%s", line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
	if (line[0] == '!') {                   /* kept, and said again every minute (mali_thread) */
		lck_mtx_lock(&kept_lock);
		strlcpy(kept_lines[kept_next % KEPT_LINES], line + 1, sizeof(kept_lines[0]));
		kept_next++;
		lck_mtx_unlock(&kept_lock);
	}
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, say, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    say_line, "A", "");

static uint32_t gl_said, gl_middle;
/*
 * sysctl kern.hakios.bind_cpu=N: the calling thread runs on logical processor N from now on (-1: on any). The
 * scheduler of this kernel does not tell big cores from small ones, and on the X6826B CoreAnimation's render thread
 * spent 14 to 18 ms a frame wherever it happened to be (the original's run 147, 26 October). The Metal driver puts
 * that thread and its own worker on the big cores.
 */
extern void *cpu_to_processor(int cpu);
extern void *thread_bind(void *processor);
extern int thread_block(void *continuation);
extern unsigned int real_ncpus;

/* Kernel callers use the same validated placement operation as the existing
 * per-calling-thread sysctl. This keeps private scheduler declarations out of
 * the IOKit framebuffer's exported header environment. */
int
hakios_mali_bind_current_cpu(int cpu)
{
	if (cpu < -1 || cpu >= (int) real_ncpus) return EINVAL;
	void *processor = cpu < 0 ? NULL : cpu_to_processor(cpu);
	if (cpu >= 0 && processor == NULL) return EINVAL;
	(void) thread_bind(processor);
	(void) thread_block(NULL);
	return 0;
}

static int
bind_cpu(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int cpu = -1, error;

	error = SYSCTL_IN(req, &cpu, sizeof(cpu));
	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	return hakios_mali_bind_current_cpu(cpu);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, bind_cpu, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    bind_cpu, "I", "bind the calling thread to a logical processor");

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gl, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_said, 0, "");
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gl_middle, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_middle, 0, "");
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_id, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_id, 0, "GPU_ID");

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
	__asm__ volatile ("dsb sy");
}

/* Until (register & mask) == want, for at most `us` microseconds by the clock (delay() may sleep far longer than asked). */
static bool
wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &deadline);
	deadline += mach_absolute_time();
	while ((rd(base, at) & mask) != want) {
		if (mach_absolute_time() >= deadline) {
			return (rd(base, at) & mask) == want;
		}
	}
	return true;
}

/* `us` microseconds, by the clock */
static void
pause_us(uint32_t us)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &deadline);
	deadline += mach_absolute_time();
	while (mach_absolute_time() < deadline) {
	}
}

/* ---- the GPU's registers (Linux drivers/gpu/drm/panfrost/panfrost_regs.h) ---- */

#define GPU_ID              0x000
#define GPU_L2_FEATURES     0x004
#define GPU_CORE_FEATURES   0x008
#define GPU_TILER_FEATURES  0x00c
#define GPU_MEM_FEATURES    0x010
#define GPU_MMU_FEATURES    0x014       /* [7:0] bits of GPU address, [15:8] bits of physical address */
#define GPU_AS_PRESENT      0x018
#define GPU_JS_PRESENT      0x01c
#define GPU_INT_RAWSTAT     0x020
#define GPU_INT_CLEAR       0x024
#define GPU_INT_MASK        0x028
#define GPU_CMD             0x030
#define THREAD_MAX_THREADS  0x0a0
#define SHADER_PRESENT      0x100
#define SHADER_PRESENT_HI   0x104
#define TILER_PRESENT       0x110
#define L2_PRESENT          0x120
#define SHADER_READY        0x140
#define TILER_READY         0x150
#define L2_READY            0x160
#define SHADER_PWRON        0x180
#define TILER_PWRON         0x190
#define L2_PWRON            0x1a0
#define COHERENCY_FEATURES  0x300
#define COHERENCY_ENABLE    0x304
#define COHERENCY_NONE      31u
#define THREAD_TLS_ALLOC    0x310
#define IRQ_RESET_COMPLETED (1u << 8)
#define GPU_CMD_SOFT_RESET  1u
#define GPU_CMD_HARD_RESET  2u
#define GPU_JM_CONFIG       0xf00
#define GPU_SHADER_CONFIG   0xf04
#define GPU_TILER_CONFIG    0xf08
#define GPU_L2_MMU_CONFIG   0xf0c
#define L2_MMU_LIMIT_EXTERNAL (0xfu << 24)    /* outstanding external reads [25:24], writes [27:26]: 0, no limit */
#define SC_LS_ALLOW_ATTR_TYPES (1u << 16)       /* the T880's settings after a reset (the header's "T880") */
#define TC_CLOCK_GATE_OVERRIDE (1u << 0)
#define JM_JOB_THROTTLE_MOST (0x3fu << 3)
#define RESET_WAIT_US       100000
#define CORES_WAIT_US       50000

#define JOB_INT_RAWSTAT     0x1000
#define JOB_INT_CLEAR       0x1004
#define JOB_INT_MASK        0x1008
#define JS1                 0x1880      /* job slot 1, where compute-like jobs go */
#define JS_STATUS           0x24
#define JS_HEAD_NEXT_LO     0x40
#define JS_HEAD_NEXT_HI     0x44
#define JS_AFFINITY_NEXT_LO 0x50
#define JS_AFFINITY_NEXT_HI 0x54
#define JS_CONFIG_NEXT      0x58
#define JS_COMMAND_NEXT     0x60
#define JS_CONFIG           ((3u << 8) | (3u << 12) | (8u << 16))  /* AS 0, caches flushed both ends */

#define AS0                 0x2400
#define AS_TRANSTAB_LO      0x00
#define AS_TRANSTAB_HI      0x04
#define AS_MEMATTR_LO       0x08
#define AS_MEMATTR_HI       0x0c
#define AS_COMMAND          0x18
#define AS_FAULTSTATUS      0x1c
#define AS_STATUS           0x28
#define AS_ACTIVE           1u
#define AS_WAIT_US          50000
#define AS_TRANSCFG_LO      0x30        /* (none on Midgard) */
#define AS_TRANSCFG_HI      0x34
#define TRANSCFG_IDENTITY   2u
#define TRANSTAB_LPAE_TABLE 3u          /* Midgard: AS_TRANSTAB [1:0] */

/*
 * A reset as Linux panfrost makes one (panfrost_gpu_soft_reset(), panfrost_gpu_init_quirks()): interrupts masked, the
 * reset's own interrupt cleared, a soft reset, and a hard one (GPU_CMD = 2) when that does not finish; everything
 * cleared. Then what a reset clears put back: on a G72 COHERENCY_ENABLE, which says the GPU is to expect no coherency
 * from the rest of the chip (the original left it as the reset made it); on a T880 its three settings (the header's
 * "T880"); on both L2_MMU_CONFIG's limits on outstanding external reads and writes cleared (two-bit fields on both).
 * False if no reset finished.
 */
static bool
gpu_reset(void)
{
	wr(mali, GPU_INT_MASK, 0);
	wr(mali, GPU_INT_CLEAR, IRQ_RESET_COMPLETED);
	wr(mali, GPU_CMD, GPU_CMD_SOFT_RESET);
	if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, RESET_WAIT_US)) {
		say("mali: the soft reset did not finish (GPU_INT_RAWSTAT %#x): a hard reset\n", rd(mali, GPU_INT_RAWSTAT));
		wr(mali, GPU_CMD, GPU_CMD_HARD_RESET);
		if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, RESET_WAIT_US)) {
			return false;
		}
	}
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	if (midgard) {
		wr(mali, GPU_SHADER_CONFIG, SC_LS_ALLOW_ATTR_TYPES);
		wr(mali, GPU_TILER_CONFIG, rd(mali, GPU_TILER_CONFIG) | TC_CLOCK_GATE_OVERRIDE);
		wr(mali, GPU_JM_CONFIG, JM_JOB_THROTTLE_MOST);
	} else {
		wr(mali, COHERENCY_ENABLE, COHERENCY_NONE);
	}
	wr(mali, GPU_L2_MMU_CONFIG, rd(mali, GPU_L2_MMU_CONFIG) & ~L2_MMU_LIMIT_EXTERNAL);
	return true;
}

/*
 * The cores, as panfrost powers them (panfrost_gpu_power_on()): the L2 and waited for, then the shader cores, then the
 * tiler. False, and which did not come, if one did not.
 */
static bool
gpu_cores_on(const char **which)
{
	uint32_t tiler = rd(mali, TILER_PRESENT);

	wr(mali, L2_PWRON, l2_mask);
	if (!wait_for(mali, L2_READY, l2_mask, l2_mask, CORES_WAIT_US)) {
		*which = "the L2";
		return false;
	}
	wr(mali, SHADER_PWRON, shader_mask);
	if (!wait_for(mali, SHADER_READY, shader_mask, shader_mask, CORES_WAIT_US)) {
		*which = "the shader cores";
		return false;
	}
	wr(mali, TILER_PWRON, tiler);
	if (!wait_for(mali, TILER_READY, tiler, tiler, CORES_WAIT_US)) {
		*which = "the tiler";
		return false;
	}
	return true;
}

/* ---- one job ---- */

/*
 * The smallest job there is, in a page of the kernel's own: a job header saying "write value", and its payload --
 * where, what kind of value (a 32-bit immediate), and the value -- the "where" being the same page 0x800 further on.
 * Midgard has no immediate values (Mesa's genxml v5: cycle counter, timestamp, zero; the T880 finished a job of type 6
 * and wrote nothing, run 20261108T163340): there the job writes zero over a pattern put there first. `gpu` is the
 * address the GPU knows the page by: its physical address in identity mode, or wherever the page tables put it. The
 * processor's caches are cleaned either side, since the GPU here does not look into them.
 */
static bool
run_write_job(vm_offset_t page, uint64_t gpu, const char *what)
{
	volatile uint32_t *job = (volatile uint32_t *)page, *target = (volatile uint32_t *)(page + 0x800);
	bool ok;

	bzero((void *)page, PAGE_SIZE);
	job[4] = (2u << 1) | 1u | (1u << 16);   /* write value; 64-bit pointers; job index 1 */
	job[8] = (uint32_t)(gpu + 0x800);
	job[9] = (uint32_t)((gpu + 0x800) >> 32);
	if (midgard) {
		job[10] = 3;                    /* zero */
		target[0] = target[1] = 0xc0ffee01u;
	} else {
		job[10] = 6;                    /* a 32-bit immediate */
		job[12] = 0xc0ffee01u;
	}
	FlushPoC_DcacheRegion(page, PAGE_SIZE);

	wr(mali, JOB_INT_MASK, 0);
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	wr(mali, JS1 + JS_HEAD_NEXT_LO, (uint32_t)gpu);
	wr(mali, JS1 + JS_HEAD_NEXT_HI, (uint32_t)(gpu >> 32));
	wr(mali, JS1 + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, JS1 + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, JS1 + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, JS1 + JS_COMMAND_NEXT, 1);     /* start */
	(void)wait_for(mali, JOB_INT_RAWSTAT, (1u << 1) | (1u << 17), 1u << 1, 100000);

	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	ok = midgard ? target[0] == 0 && target[1] == 0 : target[0] == 0xc0ffee01u;
	say("mali: %u: %s: interrupts %#x, slot status %#x, MMU fault %#x (exception %#x) at %#x:%08x, job status %#x, the "
	    "words %#x %#x (%s)%s\n", step, what, rd(mali, JOB_INT_RAWSTAT), rd(mali, JS1 + JS_STATUS),
	    rd(mali, AS0 + AS_FAULTSTATUS), rd(mali, AS0 + AS_FAULTSTATUS) & 0xff, rd(mali, AS0 + 0x24), rd(mali, AS0 + 0x20),
	    job[0], target[0], target[1], midgard ? "zero wanted" : "0xc0ffee01 wanted",
	    ok ? " -- the GPU ran it" : " -- NOT WRITTEN");
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	return ok;
}

/* `transcfg` is not written on Midgard, whose address space has no AS_TRANSCFG: its mode is in `transtab` */
static void
as0_update(uint64_t transtab, uint32_t transcfg)
{
	uint32_t memattr = midgard ? 0x48484848u : 0x88888888u;   /* every attribute: the GPU's own caches (kbase's) */

	(void)wait_for(mali, AS0 + AS_STATUS, AS_ACTIVE, 0, AS_WAIT_US);   /* no command while one runs (panfrost) */
	wr(mali, AS0 + AS_TRANSTAB_LO, (uint32_t)transtab);
	wr(mali, AS0 + AS_TRANSTAB_HI, (uint32_t)(transtab >> 32));
	if (!midgard) {
		wr(mali, AS0 + AS_TRANSCFG_LO, transcfg);
		wr(mali, AS0 + AS_TRANSCFG_HI, 0);
	}
	wr(mali, AS0 + AS_MEMATTR_LO, memattr);
	wr(mali, AS0 + AS_MEMATTR_HI, memattr);
	wr(mali, AS0 + AS_COMMAND, 1);                  /* update */
	(void)wait_for(mali, AS0 + AS_STATUS, AS_ACTIVE, 0, AS_WAIT_US);
}

/* ---- the GPU's own page tables ---- */

/*
 * The format Linux's panfrost gives a Bifrost GPU: ordinary AArch64 stage-1 tables with 4 KB pages (AS_TRANSCFG mode
 * 6). They translate 35 bits of address (the G52's MMU_FEATURES were 0x2823; step 4 checks the G72 has at least as
 * many), which makes three levels: the top table (pt_top) is indexed by bits 34..30, then 29..21, then 20..12. A page's
 * entry is its address, the access flag, inner shareable, readable and writable (AP 01), memory attribute 0, and the
 * low bits 11 ("page").
 *
 * Where the GPU's walk starts. On the G52 the GPU itself had 35 bits of address, AS_TRANSCFG's INA_BITS [10:6] said 55 -
 * 35 = 20, and the walk began at pt_top. A G72 is expected to have 48 (the G71's and G72's MMU_FEATURES read 0x2830
 * elsewhere), and on it both of Linux's drivers walk four levels from level 0: Samsung's kbase always (its tables start
 * at MIDGARD_MMU_TOPLEVEL 0, INA_BITS left 0), panfrost with INA_BITS = 55 - 48. Neither has the GPU start at level 1 on
 * a GPU of 48 bits, so nothing says that a G72 honours INA_BITS = 20 that way. On a GPU of more than 39 bits, then, the
 * tables get the level 0 that both drivers give it: one more page (pt_root), its entry 0 pointing at pt_top (every GPU
 * address here is below 2^35, so its other 511 stay empty), AS_TRANSTAB at it, and INA_BITS = 55 - the GPU's bits, as
 * panfrost writes. pt_transtab is what AS_TRANSTAB holds, either way.
 */
#define PTE_TABLE           3ull
#define PTE_PAGE            3ull
#define PTE_AF              (1ull << 10)
#define PTE_SH_INNER        (3ull << 8)
#define PTE_AP_RW           (1ull << 6)
#define PTE_LPAE_PAGE       1ull        /* Midgard's page entry: type 1, readable and writable, attribute 0 */
#define PTE_LPAE_RW         (3ull << 6)
#define PTE_LPAE_SH         (2ull << 8)     /* Mali's own: coherent among the GPU's units (and outside it) */
#define PTE_ADDRESS         0x0000fffffffff000ull
#define GPU_VA_BITS         35u
#define GPU_PA_BITS_NEEDED  36u         /* the S9's memory: 0x80000000-0xFFFFFFFF and 0x880000000-0x8FFFFFFFF */
#define TRANSCFG_AARCH64    6u                          /* [3:0]: AArch64 tables, 4 KB pages (2 is identity) */
#define TRANSCFG_INA(bits)  ((55u - (bits)) << 6)       /* [10:6]: 55 - the bits of input address */
#define TRANSCFG_WALK_WB    ((2u << 24) | (1u << 30))   /* the walk's reads write-back, read-allocate (kbase's) */
#define TRANSCFG_WALK_NC    (1u << 24)                  /* ... not cached */
#define PT_FOUR_LEVELS_BITS 40u                         /* from here on, a GPU's walk is given level 0 as well */
/*
 * Boot-arg mali_fastmap (1 unless =0): the GPU walks its tables without caching them, so an entry the processor has
 * written (and cleaned to memory) is what the next walk reads -- and a buffer newly entered needs no command to the MMU
 * at all, an empty entry being one no TLB keeps. Each new buffer used to be an UPDATE of address space 0 and a wait for
 * it: 2.4 ms a buffer, 320 buffers every five seconds in the window server (the original's run 139, 26 October). Taking
 * a buffer out still updates.
 */
static int mali_fastmap = 1;

static uint64_t *pt_top, *pt_root;      /* pt_root: level 0, on a GPU of more than 39 bits (above) */
static uint64_t pt_top_phys, pt_root_phys, pt_transtab;
static uint32_t pt_gpu_bits = GPU_VA_BITS;  /* the GPU's own, from step 4 (48 at most is used) */

static inline bool
pt_four_levels(void)
{
	return pt_gpu_bits >= PT_FOUR_LEVELS_BITS;
}

/* AS_TRANSCFG for the tables: AArch64, 4 KB pages, the input bits the walk starts from, how it reads */
static inline uint32_t
pt_transcfg(void)
{
	return TRANSCFG_AARCH64 | TRANSCFG_INA(pt_four_levels() ? pt_gpu_bits : GPU_VA_BITS) |
	       (mali_fastmap ? TRANSCFG_WALK_NC : TRANSCFG_WALK_WB);
}
static uint32_t pt_refused;             /* addresses not taken into, or not followed through, the tables */

/*
 * The kernel's address of the table an entry points to; NULL for an empty entry, and for one whose address is not
 * memory the kernel manages (said, the first few times). The check comes before phystokv(), which panics on an address
 * outside the kernel's memory, and before the cache is cleaned over the table's entries: PixOS, on another phone,
 * panicked cleaning the cache over an address it had not checked. The tables are all the kernel's own pages, so this
 * should never refuse; if it does, an entry has been overwritten.
 */
static uint64_t *
table_of(uint64_t entry)
{
	uint64_t phys = entry & PTE_ADDRESS;

	if (!(entry & 1)) {
		return NULL;
	}
	if (phys == 0 || !pmap_valid_address(phys)) {
		if (pt_refused++ < 4) {
			printf("mali: a table entry %#llx points outside the kernel's memory: not followed\n", entry);
		}
		return NULL;
	}
	return (uint64_t *)phystokv(phys);
}

static uint64_t *
pt_page(uint64_t *phys)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys = kvtophys(page);
	if (*phys == 0 || !pmap_valid_address(*phys)) {
		kmem_free(kernel_map, page, PAGE_SIZE);
		return NULL;
	}
	FlushPoC_DcacheRegion(page, PAGE_SIZE);  /* its zeroes in memory, where the GPU's walk reads them */
	return (uint64_t *)page;
}

/* The table below `entry`, made if there is none yet. */
static uint64_t *
pt_below(uint64_t *entry)
{
	uint64_t phys;
	uint64_t *table;

	if (*entry & 1) {
		return table_of(*entry);
	}
	table = pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = phys | PTE_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* One 4 KB page of physical memory at GPU address `gpu` (with the lock held, or before anyone else runs). */
static bool
map_page(uint64_t gpu, uint64_t phys)
{
	uint64_t *middle, *bottom, *entry;

	if (gpu >= (1ull << GPU_VA_BITS)) {
		return false;
	}
	if (phys == 0 || (!pmap_valid_address(phys) && !hakios_scanout_carved(phys))) {
		/*
		 * the GPU is given memory the kernel manages, and nothing else (a register block, memory of the firmware's) --
		 * but for the scan-out pool of the DPU driver (fb_zc: the display surfaces the window server draws into)
		 */
		if (pt_refused++ < 4) {
			printf("mali: physical address %#llx is not memory the kernel manages: not entered at %#llx\n", phys, gpu);
		}
		return false;
	}
	if (pt_top == NULL && (pt_top = pt_page(&pt_top_phys)) == NULL) {
		return false;
	}
	if (pt_four_levels() && pt_root == NULL) {
		/* level 0 above it: entry 0 covers GPU addresses 0 to 2^39, and every one here is below 2^35 */
		if ((pt_root = pt_page(&pt_root_phys)) == NULL) {
			return false;
		}
		pt_root[0] = pt_top_phys | PTE_TABLE;
		FlushPoC_DcacheRegion((vm_offset_t)pt_root, sizeof(pt_root[0]));
	}
	pt_transtab = (pt_root != NULL ? pt_root_phys : pt_top_phys) | (midgard ? TRANSTAB_LPAE_TABLE : 0);
	if ((middle = pt_below(&pt_top[(gpu >> 30) & 31])) == NULL || (bottom = pt_below(&middle[(gpu >> 21) & 511])) == NULL) {
		return false;
	}
	entry = &bottom[(gpu >> 12) & 511];
	*entry = (phys & PTE_ADDRESS) | (midgard ? PTE_LPAE_SH | PTE_LPAE_RW | PTE_LPAE_PAGE : PTE_AF | PTE_SH_INNER | PTE_AP_RW | PTE_PAGE);
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return true;
}

/* The entries of `size` bytes from GPU address `gpu` emptied, as far as there are tables (with the lock held). */
static void
unmap_pages(uint64_t gpu, uint64_t size)
{
	if (pt_top == NULL) {
		return;
	}
	for (uint64_t at = gpu; at < gpu + size; at += 4096) {        /* the GPU's pages, not the kernel's 16 KB */
		uint64_t *table = table_of(pt_top[(at >> 30) & 31]), *entry;

		if (table == NULL) {
			continue;
		}
		table = table_of(table[(at >> 21) & 511]);
		if (table == NULL) {
			continue;
		}
		entry = &table[(at >> 12) & 511];
		*entry = 0;
		FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	}
}

/* ---- the steps ---- */

/* 1: what the boot has left, read only (the glue's); the GPU is mapped, not read. */
static bool
step_look(void)
{
	say("mali: 1: the %s glue; the GPU's registers at %#llx mapped, not read\n", glue->name, glue->gpu_pa);
	if ((mali = ml_io_map(glue->gpu_pa, MALI_SIZE)) == 0) {
		say("mali: 1: cannot map the GPU\n");
		return false;
	}
	return glue->look();
}

/* 2: the supply, the clock, power (the glue's). */
static bool
step_power(void)
{
	return glue->power();
}

/* 3: what a power-on of step 2's own needs after it (the glue's). */
static bool
step_after_power(void)
{
	return glue->after_power();
}

/* 4: which GPU, and what it has. */
static bool
step_gpu(void)
{
	uint32_t mmu, va_bits, pa_bits, shaders_hi, as_present, js_present, tiler;

	say("mali: 4: reading GPU_ID at %#lx, the first read of the GPU: should the run end here, the domain says on and the "
	    "GPU does not answer (its supply, its clock, or its protection)\n", (unsigned long)glue->gpu_pa);
	gpu_id = rd(mali, GPU_ID);
	say("mali: 4: GPU_ID %#x: architecture %u.%u revision %u, product %u, version r%up%u, status %u\n", gpu_id,
	    gpu_id >> 28, (gpu_id >> 24) & 0xf, (gpu_id >> 20) & 0xf, (gpu_id >> 16) & 0xf, (gpu_id >> 12) & 0xf,
	    (gpu_id >> 4) & 0xff, gpu_id & 0xf);
	if ((gpu_id >> 16) == MALI_T880) {
		midgard = true;
		say("mali: 4: a Mali-T880 (Midgard): LPAE page tables, no COHERENCY_ENABLE, Midgard's settings after a reset\n");
	} else if ((gpu_id >> 16) != MALI_G72) {
		if (((gpu_id >> 16) & 0xf00f) != 0x6001) {
			say("mali: 4: neither a Mali-G72 (GPU_ID >> 16 %#x) nor a T880 (%#x): nothing more is done with it\n", MALI_G72,
			    MALI_T880);
			return false;
		}
		say("mali: 4: a G72 of another revision than the %#x expected: going on\n", MALI_G72);
	}
	mmu = rd(mali, GPU_MMU_FEATURES);
	va_bits = mmu & 0xff;
	pa_bits = (mmu >> 8) & 0xff;
	say("mali: 4: MMU_FEATURES %#x: %u bits of GPU address, %u of physical (the page tables want %u and %u; the G52 "
	    "the driver was written for had 0x2823)\n", mmu, va_bits, pa_bits, GPU_VA_BITS, GPU_PA_BITS_NEEDED);
	if (va_bits < GPU_VA_BITS) {
		say("mali: 4: fewer than %u bits of GPU address: the page tables here start at bit 34; not going on\n", GPU_VA_BITS);
		return false;
	}
	if (pa_bits < GPU_PA_BITS_NEEDED) {
		say("mali: 4: fewer than %u bits of physical address, and this phone's memory reaches 0x8FFFFFFFF: not going on\n",
		    GPU_PA_BITS_NEEDED);
		return false;
	}
	pt_gpu_bits = va_bits > 48 ? 48 : va_bits;
	say("mali: 4: the GPU's tables will be walked %s; %s %#x\n", pt_four_levels() ?
	    "from level 0, four levels, as kbase and panfrost have a GPU of this many bits walk them" :
	    "from level 1, three levels, as on the G52", midgard ? "AS_TRANSTAB's mode" : "AS_TRANSCFG",
	    midgard ? TRANSTAB_LPAE_TABLE : pt_transcfg());
	shader_mask = rd(mali, SHADER_PRESENT);
	shaders_hi = rd(mali, SHADER_PRESENT_HI);
	tiler = rd(mali, TILER_PRESENT);
	l2_mask = rd(mali, L2_PRESENT);
	as_present = rd(mali, GPU_AS_PRESENT);
	js_present = rd(mali, GPU_JS_PRESENT);
	say("mali: 4: shader cores %#x (%u of them; above bit 31 %#x), tiler %#x, L2 %#x; address spaces %#x, job slots %#x\n",
	    shader_mask, __builtin_popcount(shader_mask), shaders_hi, tiler, l2_mask, as_present, js_present);
	say("mali: 4: L2_FEATURES %#x, CORE_FEATURES %#x, TILER_FEATURES %#x, MEM_FEATURES %#x, THREAD_MAX_THREADS %u, "
	    "THREAD_TLS_ALLOC %u, COHERENCY_FEATURES %#x\n", rd(mali, GPU_L2_FEATURES), rd(mali, GPU_CORE_FEATURES),
	    rd(mali, GPU_TILER_FEATURES), rd(mali, GPU_MEM_FEATURES), rd(mali, THREAD_MAX_THREADS), rd(mali, THREAD_TLS_ALLOC),
	    rd(mali, COHERENCY_FEATURES));
	if (shader_mask == 0 || shaders_hi != 0) {
		say("mali: 4: shader cores %#x:%08x: the jobs are given 32 bits of affinity; not going on\n", shaders_hi, shader_mask);
		return false;
	}
	if (!(as_present & 1) || (js_present & 3) != 3 || tiler == 0 || l2_mask == 0) {
		say("mali: 4: no address space 0, job slots 0 and 1, tiler or L2: not going on\n");
		return false;
	}
	gpu_answered = true;
	return true;
}

/* 5: a reset, no coherency, and panfrost's setting after a reset. */
static bool
step_reset(void)
{
	say("mali: 5: JM_CONFIG %#x, SHADER_CONFIG %#x, TILER_CONFIG %#x, L2_MMU_CONFIG %#x before the reset\n",
	    rd(mali, GPU_JM_CONFIG), rd(mali, GPU_SHADER_CONFIG), rd(mali, GPU_TILER_CONFIG), rd(mali, GPU_L2_MMU_CONFIG));
	if (midgard) {
		say("mali: 5: writing GPU_INT_MASK = 0, GPU_CMD = 1 (soft reset; 2, hard, if it does not finish), then "
		    "SHADER_CONFIG = %#x, TILER_CONFIG |= %#x, JM_CONFIG = %#x and L2_MMU_CONFIG &= ~%#x\n", SC_LS_ALLOW_ATTR_TYPES,
		    TC_CLOCK_GATE_OVERRIDE, JM_JOB_THROTTLE_MOST, L2_MMU_LIMIT_EXTERNAL);
	} else {
		say("mali: 5: writing GPU_INT_MASK = 0, GPU_CMD = 1 (soft reset; 2, hard, if it does not finish), then "
		    "COHERENCY_ENABLE = %u (none) and L2_MMU_CONFIG &= ~%#x\n", COHERENCY_NONE, L2_MMU_LIMIT_EXTERNAL);
	}
	if (!gpu_reset()) {
		say("mali: 5: GPU_ID %#x, but it did not finish a reset (GPU_INT_RAWSTAT %#x)\n", gpu_id, rd(mali, GPU_INT_RAWSTAT));
		return false;
	}
	say("mali: 5: reset; JM_CONFIG %#x, SHADER_CONFIG %#x, TILER_CONFIG %#x, L2_MMU_CONFIG %#x%s\n",
	    rd(mali, GPU_JM_CONFIG), rd(mali, GPU_SHADER_CONFIG), rd(mali, GPU_TILER_CONFIG), rd(mali, GPU_L2_MMU_CONFIG),
	    midgard ? "" : ", COHERENCY_ENABLE as below");
	if (!midgard) {
		say("mali: 5: COHERENCY_ENABLE reads %#x\n", rd(mali, COHERENCY_ENABLE));
	}
	return true;
}

/* 6: the cores. */
static bool
step_cores(void)
{
	uint32_t tiler = rd(mali, TILER_PRESENT);
	const char *which = NULL;

	say("mali: 6: writing L2_PWRON %#x, then SHADER_PWRON %#x, then TILER_PWRON %#x, each waited for\n", l2_mask,
	    shader_mask, tiler);
	if (!gpu_cores_on(&which)) {
		say("mali: 6: %s did not power up: L2 ready %#x, shaders %#x, tiler %#x\n", which, rd(mali, L2_READY),
		    rd(mali, SHADER_READY), rd(mali, TILER_READY));
		return false;
	}
	say("mali: 6: GPU_ID %#x (Mali-%s r%up%u), %u shader cores %#x, tiler %#x, L2 %#x: all powered\n", gpu_id,
	    midgard ? "T880" : "G72", (gpu_id >> 12) & 0xf, (gpu_id >> 4) & 0xff, __builtin_popcount(shader_mask), shader_mask,
	    tiler, l2_mask);
	return true;
}

/*
 * 7 and 8: two jobs, first in identity mode, which needs no tables and so says whether the job manager works at all;
 * then the same job at a GPU address the tables give the page, which says whether they do. Afterwards address space 0
 * stays on the tables, which is how anything that draws will need it, and the page's entry is taken out of them again.
 */
#define FIRST_JOB_GPU       0x00100000ull

static vm_offset_t job_page;

/*
 * (Not on Midgard: with AS_TRANSTAB's mode 2 the Note FE's T880 walked a table at physical address 0 all the same, a
 * TRANSTAB_BUS_FAULT at level 0 (exception 0xd0; run 20261108T162853), and panfrost never uses that mode either. Its
 * first job is step 8's.)
 */
static bool
step_job_identity(void)
{
	if (kmem_alloc(kernel_map, &job_page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		job_page = 0;
		say("mali: 7: no page for the first jobs\n");
		return false;
	}
	if (midgard) {
		say("mali: 7: a T880 has no identity mode to run a job in: the first job is step 8's, through the tables\n");
		return true;
	}
	say("mali: 7: address space 0 in identity mode (AS_TRANSCFG 2); one job on slot 1 at %#llx\n", kvtophys(job_page));
	as0_update(0, TRANSCFG_IDENTITY);       /* identity: a GPU address is the physical one */
	return run_write_job(job_page, kvtophys(job_page), "first job, identity mode");
}

static bool
step_job_tables(void)
{
	if (!map_page(FIRST_JOB_GPU, kvtophys(job_page))) {
		say("mali: 8: no memory for page tables\n");
		return false;
	}
	say("mali: 8: the page at GPU address %#llx in the GPU's tables (AS_TRANSTAB %#llx, %s levels%s), address space 0 "
	    "on them; the same job again\n", FIRST_JOB_GPU, pt_transtab, pt_four_levels() ? "four" : "three",
	    midgard ? ", the LPAE format" : ", AArch64's");
	as0_update(pt_transtab, pt_transcfg());
	return run_write_job(job_page, FIRST_JOB_GPU, "second job, through the GPU's page tables");
}

/* 9: there for programs. */
static bool
step_serve(void)
{
	say("mali: 9: HakiosMali started: programs can draw with the GPU (Mesa's panfrost; drivers/gpu/hakios.mali.cpp)\n");
	reached = 9;                            /* (the user client asks hakios_mali_ready()) */
	HakiosMaliStart();
	return true;
}

static const struct {
	const char *name;
	bool (*make)(void);
} steps[] = {
	{ "what the boot left, read only", step_look },
	{ "the supply, the clock, power", step_power },
	{ "after a power-on of step 2's", step_after_power },
	{ "GPU_ID and what the GPU has", step_gpu },
	{ "a reset", step_reset },
	{ "the cores", step_cores },
	{ "a job in identity mode", step_job_identity },
	{ "a job through page tables", step_job_tables },
	{ "HakiosMali", step_serve },
};
#define STEPS ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/* ---- what the service that programs talk to uses (drivers/gpu/hakios.mali.cpp) ---- */

/*
 * Linux's panfrost interface, as far as Mesa's panfrost needs it to draw, is answered by the IOKit service HakiosMali;
 * this is its half in the hardware: parameters, GPU addresses, pages entered into and taken out of the tables, and
 * jobs. One address space (0) for everyone, and one job at a time -- simple, and enough to see Mesa draw; overlapping
 * jobs and per-client address spaces come after.
 */
static LCK_GRP_DECLARE(mali_lock_group, "hakios-mali");
static LCK_MTX_DECLARE(mali_lock, &mali_lock_group);
/*
 * Held while a job runs, and by what must not change the address space under one (an UPDATE when entries are taken
 * out); taken before mali_lock, never after. mali_lock itself is no longer held for the length of a job: making a
 * buffer had to wait for the job running to finish (2.5 ms a buffer, 26 October).
 */
static LCK_MTX_DECLARE(mali_job_lock, &mali_lock_group);

/*
 * Until a job is done, without keeping a core busy: a moment of polling for the short ones, then asleep 200 us at a
 * time. The wait used to spin (delay(10) in a loop) for as long as the GPU worked -- a whole core of the four lost to
 * every frame's rendering.
 */
static int mali_wait_event;
static uint32_t mali_nap_us = 50;       /* boot-arg mali_nap_us (10 to 1000) */
static LCK_MTX_DECLARE(mali_wait_lock, &mali_lock_group);

static bool
wait_job(uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t deadline;

	for (int i = 0; i < 20; i++) {
		if ((rd(mali, at) & mask) == want) {
			return true;
		}
		delay(10);
	}
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if ((rd(mali, at) & mask) == want) {
			return true;
		}
		uint64_t nap;

		clock_interval_to_deadline(mali_nap_us, NSEC_PER_USEC, &nap);
		lck_mtx_lock(&mali_wait_lock);
		(void)lck_mtx_sleep_deadline(&mali_wait_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_wait_event, THREAD_UNINT, nap);
		lck_mtx_unlock(&mali_wait_lock);
	}
	return (rd(mali, at) & mask) == want;
}

#define MMU_INT_RAWSTAT     0x2000
#define MMU_INT_CLEAR       0x2004
#define AS_LOCKADDR_LO      0x10
#define AS_LOCKADDR_HI      0x14
#define AS_COMMAND_LOCK     2
#define AS_COMMAND_FLUSH_PT 4
#define JS0                 0x1800      /* fragment jobs; everything else goes to slot 1 */
/* The GPU addresses programs' buffers get: 32 MB to 4 GB, given out and taken back (hakios.mali_va.h says why). */
static struct mali_va mali_va;          /* with mali_lock held */

/*
 * The last buffers handed GPU addresses, and whether they have been taken out of the tables since: a translation fault
 * is a job writing into a buffer already freed or past the end of one, and this says which. An address freed is given
 * out again, but not soon (above): the newest buffer at an address is the one asked about. With mali_lock held.
 */
#define MALI_RECENT 4096
static struct {
	uint64_t address, size;
	uint32_t freed, kind;                   /* kind: 0 not said, 1 a panfrost BO, 2 an IOSurface */
	uint32_t width, height, row_bytes, format, offset;
} mali_recent[MALI_RECENT];
static uint32_t mali_recent_next;

/* What a buffer just made is, for describe_address. */
void
hakios_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == address && mali_recent[i].size != 0 && !mali_recent[i].freed) {
			mali_recent[i].kind = kind;
			mali_recent[i].width = width;
			mali_recent[i].height = height;
			mali_recent[i].row_bytes = row_bytes;
			mali_recent[i].format = format;
			mali_recent[i].offset = offset;
		}
	}
	lck_mtx_unlock(&mali_lock);
}

static void
say_kind(int i)
{
	if (mali_recent[i].kind == 2) {
		printf("mali:     an IOSurface %ux%u, %u bytes a row, format %#x, from byte %u\n", mali_recent[i].width,
		    mali_recent[i].height, mali_recent[i].row_bytes, mali_recent[i].format, mali_recent[i].offset);
	} else {
		printf("mali:     %s\n", mali_recent[i].kind == 1 ? "a panfrost BO" : "made by nothing that said");
	}
}

static void
describe_address(uint64_t address)
{
	int below = -1;

	/* newest first: an address given out again belongs to its last buffer */
	for (uint32_t n = 0; n < MALI_RECENT; n++) {
		int i = (int)((mali_recent_next - 1 - n) % MALI_RECENT);

		if (mali_recent[i].size == 0) {
			continue;
		}
		if (address >= mali_recent[i].address && address < mali_recent[i].address + mali_recent[i].size) {
			printf("mali:   the fault is %#llx into a buffer of %#llx bytes at %#llx, %s\n",
			    address - mali_recent[i].address, mali_recent[i].size, mali_recent[i].address,
			    mali_recent[i].freed ? "FREED already" : "still there (a page not entered?)");
			say_kind(i);
			return;
		}
		if (mali_recent[i].address <= address && (below < 0 || mali_recent[i].address > mali_recent[below].address ||
		    (mali_recent[i].address == mali_recent[below].address && !mali_recent[i].freed))) {
			below = i;
		}
	}
	if (below >= 0) {
		printf("mali:   the fault is %#llx past the end of a buffer of %#llx bytes at %#llx (%s)\n",
		    address - (mali_recent[below].address + mali_recent[below].size), mali_recent[below].size,
		    mali_recent[below].address, mali_recent[below].freed ? "freed" : "live");
		say_kind(below);
	} else {
		printf("mali:   the fault is in no buffer of the last %u\n", MALI_RECENT);
	}
}

/* The last job that did not finish, to be repeated where the end of the log keeps it. */
static struct {
	uint32_t count, slot, raw, status, mmu, fault, fault_address;
	uint64_t chain, transtab, walk[3];      /* the tables the GPU had, and their entries for the fault's address */
} last_failure;

bool
hakios_mali_map(uint64_t gpu, uint64_t phys)
{
	bool done;

	lck_mtx_lock(&mali_lock);
	done = map_page(gpu, phys);
	lck_mtx_unlock(&mali_lock);
	return done;
}

bool
hakios_mali_ready(void)
{
	return reached >= STEPS;
}

/* Linux's DRM_PANFROST_PARAM_*, in its order, from the registers they come from. */
bool
hakios_mali_parameter(uint32_t which, uint64_t *value)
{
	static const uint16_t simple[] = {
		[5] = 0xe00,    /* STACK_PRESENT (none on Bifrost: reads 0) */
		[6] = 0x018, [7] = 0x01c, [8] = 0x004, [9] = 0x008, [10] = 0x00c, [11] = 0x010, [12] = 0x014,
		[13] = 0x0ac, [14] = 0x0a0, [15] = 0x0a4, [16] = 0x0a8, [17] = 0x300,
		[18] = 0x0b0, [19] = 0x0b4, [20] = 0x0b8, [21] = 0x0bc,
	};

	if (!hakios_mali_ready()) {
		return false;
	}
	switch (which) {
	case 0:
		*value = gpu_id >> 16;
		return true;
	case 1:
		*value = gpu_id & 0xffff;
		return true;
	case 2:
	case 3:
	case 4:
		*value = rd(mali, 0x100 + (which - 2) * 0x10) | ((uint64_t)rd(mali, 0x104 + (which - 2) * 0x10) << 32);
		return true;
	case 38:                                /* NR_CORE_GROUPS: as Linux panfrost counts them, one per L2 */
		*value = __builtin_popcount(l2_mask) != 0 ? (uint64_t)__builtin_popcount(l2_mask) : 1;
		return true;
	case 39:
		/*
		 * THREAD_TLS_ALLOC (0x310). Answered 0 on the G52, panfrost sized the stack of a spilling shader by
		 * THREAD_MAX_THREADS instead, and the GPU, which gives each core more stacks than that, wrote past the end of
		 * it (25 October: every write fault of the uber shaders, up to 0x15870 past a 0x60000 buffer). Never less than
		 * that fallback, whatever the register says.
		 */
		*value = rd(mali, THREAD_TLS_ALLOC);
		if (*value < rd(mali, THREAD_MAX_THREADS)) {
			*value = rd(mali, THREAD_MAX_THREADS);
		}
		{
			static int said;
			if (!said++) {
				printf("mali: THREAD_TLS_ALLOC %#x, THREAD_MAX_THREADS %#x, SHADER_PRESENT %#x\n", rd(mali, THREAD_TLS_ALLOC),
				    rd(mali, THREAD_MAX_THREADS), rd(mali, SHADER_PRESENT));
			}
		}
		return true;
	case 40:
		/*
		 * AFBC_FEATURES: 0. Mesa reads 0 as AFBC SUPPORTED, not as none (Mesa 25.0.7 pan_props.c,
		 * panfrost_query_afbc(): architecture 5 or later and the register 0), as Linux panfrost answers
		 * on a GPU without the register. So panfrost renders into AFBC textures here (a G72 has AFBC;
		 * seen under drm-shim as the G72, tools/mali-drmshim-check.sh: the triangle's render target is
		 * written AFBC). PAN_MESA_DEBUG=noafbc in a program's environment turns it off, for telling
		 * an AFBC fault from any other on the phone.
		 */
		*value = 0;
		return true;
	default:
		if (which >= 22 && which <= 37) {
			*value = rd(mali, 0x0c0 + (which - 22) * 4);
			return true;
		}
		if (which < sizeof(simple) / sizeof(simple[0]) && simple[which] != 0) {
			*value = rd(mali, simple[which]);
			return true;
		}
		return false;
	}
}

/*
 * `size` bytes of GPU addresses between 32 MB and 4 GB (hakios.mali_va.h). `executable`: a buffer the GPU may run code
 * from (Linux's flags without PANFROST_BO_NOEXEC), kept off 16 MB lines. 0: none left, or a size of nothing.
 */
uint64_t
hakios_mali_address(uint64_t size, bool executable)
{
	uint64_t address;

	lck_mtx_lock(&mali_lock);
	address = mali_va_alloc(&mali_va, size, executable);
	if (address != 0) {
		mali_recent[mali_recent_next % MALI_RECENT].address = address;
		mali_recent[mali_recent_next % MALI_RECENT].size = size;
		mali_recent[mali_recent_next % MALI_RECENT].freed = 0;
		mali_recent[mali_recent_next % MALI_RECENT].kind = 0;
		mali_recent_next++;
	}
	lck_mtx_unlock(&mali_lock);
	return address;
}

void
hakios_mali_unmap(uint64_t gpu, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == gpu && mali_recent[i].size != 0) {
			mali_recent[i].freed = 1;
		}
	}
	unmap_pages(gpu, size);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_lock(&mali_job_lock);                   /* an entry taken out: the TLB may hold it */
	lck_mtx_lock(&mali_lock);
	as0_update(pt_transtab, pt_transcfg());
	/* only now, with nothing of the buffer left in the tables or the MMU, are its addresses free to be given again */
	(void)mali_va_free(&mali_va, gpu, size);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

/*
 * After entries change: the GPU's MMU forgets what it remembered of the tables. The address space is set up again from
 * the top -- an UPDATE drops everything the MMU had cached for it -- rather than a region locked and its tables
 * flushed: with the region flush a fragment job faulted on a page of a buffer whose entry was there, written and
 * pushed out to memory. Jobs are run one at a time and to their end, so the GPU is idle whenever this runs.
 */
void
hakios_mali_mapped(void)
{
	if (mali_fastmap) {
		return;         /* see TRANSCFG_WALK_NC */
	}
	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	as0_update(pt_transtab, pt_transcfg());
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

/* The entries of each level for GPU address `gpu`, as far as there are tables (with the lock held). */
static void
walk_tables(uint64_t gpu, uint64_t walk[3])
{
	uint64_t *table = pt_top;
	static const unsigned shift[3] = { 30, 21, 12 };

	walk[0] = walk[1] = walk[2] = 0;
	for (int level = 0; level < 3 && table != NULL; level++) {
		walk[level] = table[(gpu >> shift[level]) & (level == 0 ? 31 : 511)];
		table = level < 2 ? table_of(walk[level]) : NULL;
	}
}

/* boot-arg mali_peek: with mali_lock held, after a job chain */
static uint64_t mali_peek[4];

static void
peek(uint64_t gpu)
{
	uint64_t walk[3], phys;
	volatile uint32_t *w;

	walk_tables(gpu, walk);
	phys = (walk[2] & PTE_ADDRESS) | (gpu & 0xf80);
	if ((walk[2] & 3) == 0 || !pmap_valid_address(phys)) {
		say("mali: peek %#llx: no page there\n", gpu);
		return;
	}
	w = (volatile uint32_t *)phystokv(phys);
	FlushPoC_DcacheRegion((vm_offset_t)w, 128);     /* the GPU wrote memory, not this cache */
	for (int i = 0; i < 32; i += 8) {
		say("mali: peek %#llx: %08x %08x %08x %08x %08x %08x %08x %08x\n", (gpu & ~0x7full) + i * 4, w[i], w[i + 1],
		    w[i + 2], w[i + 3], w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
	}
}

/*
 * One job chain, from GPU address `chain`, run to its end before this returns: fragment jobs (`fragment`) on slot 0,
 * the rest on slot 1. 0 when it finished; otherwise the slot's status, a fault or 0xff for no answer within a second --
 * and then the GPU is reset, so the next one finds it working.
 */
uint32_t
hakios_mali_submit(uint64_t chain, bool fragment)
{
	uint32_t slot = fragment ? 0 : 1, js = fragment ? JS0 : JS1, raw, status = 0;
	bool finished;

	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	wr(mali, js + JS_HEAD_NEXT_LO, (uint32_t)chain);
	wr(mali, js + JS_HEAD_NEXT_HI, (uint32_t)(chain >> 32));
	wr(mali, js + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, js + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, js + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, js + JS_COMMAND_NEXT, 1);
	lck_mtx_unlock(&mali_lock);
	finished = wait_job(JOB_INT_RAWSTAT, (1u << slot) | (1u << (slot + 16)), 1u << slot, 1000);
	lck_mtx_lock(&mali_lock);
	if (!finished) {
		raw = rd(mali, JOB_INT_RAWSTAT);
		status = (raw & (1u << (slot + 16))) ? rd(mali, js + JS_STATUS) : 0xff;
		last_failure.count++;
		last_failure.slot = slot;
		last_failure.raw = raw;
		last_failure.status = rd(mali, js + JS_STATUS);
		last_failure.mmu = rd(mali, MMU_INT_RAWSTAT);
		last_failure.fault = rd(mali, AS0 + AS_FAULTSTATUS);
		last_failure.fault_address = rd(mali, AS0 + 0x20);
		last_failure.chain = chain;
		last_failure.transtab = rd(mali, AS0 + AS_TRANSTAB_LO) | ((uint64_t)rd(mali, AS0 + AS_TRANSTAB_HI) << 32);
		walk_tables(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32), last_failure.walk);
		printf("mali: a job on slot %u did not finish: interrupts %#x, status %#x, MMU %#x fault %#x at %#x\n", slot, raw,
		    last_failure.status, last_failure.mmu, last_failure.fault, last_failure.fault_address);
		describe_address(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32));
		const char *which = NULL;

		(void)gpu_reset();                      /* a reset, then everything back as it was */
		if (!gpu_cores_on(&which)) {
			printf("mali: after the reset, %s did not power up again\n", which);
		}
		as0_update(pt_transtab, pt_transcfg());
		wr(mali, MMU_INT_CLEAR, 0xffffffffu);
	}
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	if (mali_peek[0] != 0) {
		say("mali: peek after the chain at %#llx (%s)\n", chain, fragment ? "fragment" : "vertex, tiler");
		for (int i = 0; i < 4 && mali_peek[i] != 0; i++) {
			peek(mali_peek[i]);
		}
	}
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
	return status;
}

/*
 * The jobs in a queue, run by a thread of their own: a submission returns at once with the job's number, and whoever
 * needs a job done waits for that number (hakios_mali_wait). Run one by one in the order given, which is the order
 * panfrost needs -- a pass's vertex and tiler chain before its fragment chain, one pass before the next that reads it.
 * Before this every submission ran its job to the end before returning: a frame of CoreAnimation's with its blur passes
 * had the window server waiting for the GPU at every one of them, the processor and the GPU taking turns (24 October,
 * 11 to 17 ms a frame of the processor's).
 */
#define MALI_QUEUE      64
static struct {
	uint64_t chain, seq, queued;
	bool fragment;
} mali_queue[MALI_QUEUE];
/* boot-arg mali_trace=1: a line for every job, "tl: j SEQ f|v q QUEUED b BEGAN e ENDED" in us of mach_absolute_time */
static uint32_t mali_trace;
static unsigned mali_queue_head, mali_queue_count;
static uint64_t mali_queued_seq, mali_done_seq;
static bool mali_worker_started;
static LCK_MTX_DECLARE(mali_queue_lock, &mali_lock_group);

/*
 * The last second, for the display's line (fps_log, MiniIOMobileFramebuffer.cpp): how long the GPU ran jobs, how many,
 * how long they waited in the queue; with mali_queue_lock held.
 */
static uint64_t sec_busy, sec_queued;
static uint32_t sec_jobs;

/*
 * mali_gov=1: the GPU's clock after its load, as drivers/power/hakios.exynos_cpufreq.c sets the cores': every window of
 * GOV_WINDOW_MS, the share of it the GPU ran jobs; the lowest of the three rates at which that would have been
 * GOV_TARGET% at most, at once if higher, after GOV_DOWN windows in a row if lower; never above what the G3D's
 * temperature allows. The switch is the glue's, between two jobs (this thread runs them, one at a time) and under
 * mali_job_lock, so that nothing touches the GPU while its clock moves (the 9810's G3D is on the 26 MHz oscillator for
 * some microseconds). Three failed switches in a row and the governor stops, the clock left where it is.
 */
#define GOV_WINDOW_MS   16
#define GOV_TARGET      50              /* low: a frame waits for its jobs, the sooner done the better */
#define GOV_DOWN        5
#define GOV_IDLE_MS     50              /* asleep with no jobs: a window ends anyway, the clock may come down */
#define GOV_CAP1_MC     85000           /* the G3D's sensor: 455 MHz at most from here */
#define GOV_CAP2_MC     95000           /* 260 MHz from here */
#define GOV_HYST_MC     5000
#define GOV_NOT_READ_MC (-273000)
#define GOV_LEVELS      (glue->nrates)
static uint32_t mali_gov;
/*
 * mali_gov_frame=<MHz> (kern.hakios.mali_gov_frame while it runs): the GPU's clock while frames come -- one given to the
 * display (hakios_mali_frame(), MiniIOMobileFramebuffer.cpp present()) in the last GOV_FRAME_MS -- at least the lowest
 * rate of the three that is as fast, whatever the load says (still under the temperature's cap). A frame's jobs are on
 * the way of the next frame: CoreAnimation's render thread waits for them, so a GPU busy a third of the time at 260 MHz
 * is a frame later than one at 572, though the load alone would keep it at 260. 0: none (as mali_gov first was).
 */
#define GOV_FRAME_MS    100
static uint32_t gov_frame_mhz = 572;
static uint64_t gov_frame_last;
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gov_frame, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gov_frame_mhz, 0,
    "the GPU's clock while frames come, MHz at least (mali_gov=1; 0: none)");

void hakios_mali_frame(void);
void
hakios_mali_frame(void)
{
	__atomic_store_n(&gov_frame_last, mach_absolute_time(), __ATOMIC_RELAXED);
}
static bool gov_on;
static unsigned gov_level, gov_cap, gov_down, gov_failed;
static uint64_t gov_since, gov_busy;
static uint32_t gov_switches, gov_ups, gov_downs, gov_capped;

static void
gov_start(void)
{
	if (!mali_gov) {
		return;
	}
	if (GOV_LEVELS < 2 || (gov_level = glue->rate_now()) >= GOV_LEVELS) {
		say("mali: mali_gov=1: not started (the %s glue has %u rates, and its clock is %s one of them it may move)\n",
		    glue->name, GOV_LEVELS, GOV_LEVELS != 0 && gov_level < GOV_LEVELS ? "at" : "NOT at");
		return;
	}
	gov_cap = GOV_LEVELS - 1;
	(void)PE_parse_boot_argn("mali_gov_frame", &gov_frame_mhz, sizeof(gov_frame_mhz));
	gov_on = true;
	gov_since = mach_absolute_time();
	say("mali: mali_gov=1: the GPU's clock follows its load from %u MHz: %u%% of a %u ms window wanted at most; no more "
	    "than %u MHz from %d C on the G3D's sensor, %u MHz from %d C\n", glue->rate_mhz(gov_level), GOV_TARGET,
	    GOV_WINDOW_MS, glue->rate_mhz(1), GOV_CAP1_MC / 1000, glue->rate_mhz(0), GOV_CAP2_MC / 1000);
}

static void
gov_set(unsigned level)
{
	bool switched;

	if (level == gov_level || level >= GOV_LEVELS) {
		return;
	}
	lck_mtx_lock(&mali_job_lock);
	switched = glue->set_rate(level);
	lck_mtx_unlock(&mali_job_lock);
	if (switched) {
		gov_ups += level > gov_level;
		gov_downs += level < gov_level;
		gov_level = level;
		gov_switches++;
		gov_failed = 0;
		return;
	}
	if ((gov_level = glue->rate_now()) >= GOV_LEVELS || ++gov_failed >= 3) {
		gov_on = false;
		say("mali: mali_gov: the clock did not switch to %u MHz (%u times in a row; it runs at %u MHz): the governor stops\n",
		    glue->rate_mhz(level), gov_failed, glue->mhz_now());
	}
}

/* `busy` more of the GPU's time at `now` (mach_absolute_time); a window that has ended decides. No locks held. */
static void
gov_window(uint64_t now, uint64_t busy)
{
	uint64_t window, elapsed;
	unsigned want, cap;
	uint32_t load;
	int32_t mc;

	if (!gov_on) {
		return;
	}
	gov_busy += busy;
	nanoseconds_to_absolutetime((uint64_t)GOV_WINDOW_MS * NSEC_PER_MSEC, &window);
	elapsed = now - gov_since;
	if (elapsed < window) {
		return;
	}
	load = (uint32_t)(gov_busy >= elapsed ? 100 : gov_busy * 100 / elapsed);
	gov_since = now;
	gov_busy = 0;
	mc = glue->g3d_mc != NULL ? glue->g3d_mc() : GOV_NOT_READ_MC;
	cap = GOV_LEVELS - 1;
	if (mc != GOV_NOT_READ_MC) {
		if (mc >= GOV_CAP1_MC || (gov_cap <= 1 && mc >= GOV_CAP1_MC - GOV_HYST_MC)) {
			cap = 1;
		}
		if (mc >= GOV_CAP2_MC || (gov_cap == 0 && mc >= GOV_CAP2_MC - GOV_HYST_MC)) {
			cap = 0;
		}
	}
	if (cap != gov_cap) {
		say("mali: mali_gov: the G3D at %d.%d C: %u MHz at most now\n", mc / 1000, mc % 1000 / 100, glue->rate_mhz(cap));
		gov_capped += cap < gov_cap;
		gov_cap = cap;
	}
	for (want = 0; want + 1 < GOV_LEVELS &&
	    (uint64_t)load * glue->rate_mhz(gov_level) > (uint64_t)GOV_TARGET * glue->rate_mhz(want); want++) {
	}
	/* (mali_gov_frame: frames coming, the floor) */
	if (gov_frame_mhz != 0) {
		uint64_t last = __atomic_load_n(&gov_frame_last, __ATOMIC_RELAXED), span;

		nanoseconds_to_absolutetime((uint64_t)GOV_FRAME_MS * NSEC_PER_MSEC, &span);
		if (last != 0 && now - last < span) {
			unsigned floor = 0;

			while (floor + 1 < GOV_LEVELS && glue->rate_mhz(floor) < gov_frame_mhz) {
				floor++;
			}
			want = want < floor ? floor : want;
		}
	}
	want = want > cap ? cap : want;
	if (want > gov_level || gov_level > cap) {
		gov_down = 0;
		gov_set(want);
	} else if (want < gov_level) {
		if (++gov_down >= GOV_DOWN) {
			gov_down = 0;
			gov_set(want);
		}
	} else {
		gov_down = 0;
	}
}

/*
 * fps_log (MiniIOMobileFramebuffer.cpp), once a second: the GPU's time running jobs since the last call, the jobs, their
 * time in the queue before they ran (ns), the clock (MHz) and the governor's switches; the G3D's temperature
 * (millidegrees). 0 when the GPU is not there for programs.
 */
int hakios_mali_second(uint64_t *busy_ns, uint32_t *jobs, uint64_t *queued_ns, uint32_t *mhz, uint32_t *switches,
    int32_t *g3d_mc);

int
hakios_mali_second(uint64_t *busy_ns, uint32_t *jobs, uint64_t *queued_ns, uint32_t *mhz, uint32_t *switches,
    int32_t *g3d_mc)
{
	uint64_t busy, queued;

	if (reached < STEPS) {
		return 0;
	}
	lck_mtx_lock(&mali_queue_lock);
	busy = sec_busy;
	queued = sec_queued;
	*jobs = sec_jobs;
	sec_busy = sec_queued = 0;
	sec_jobs = 0;
	lck_mtx_unlock(&mali_queue_lock);
	absolutetime_to_nanoseconds(busy, busy_ns);
	absolutetime_to_nanoseconds(queued, queued_ns);
	*mhz = glue->mhz_now();
	*switches = gov_on ? gov_switches : 0;
	*g3d_mc = glue->g3d_mc != NULL ? glue->g3d_mc() : GOV_NOT_READ_MC;
	HakiosMaliCacheSecond();
	HakiosMaliCallsSecond();
	return gov_on ? 2 : 1;
}

static void
mali_worker(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint64_t chain, seq;
		bool fragment;

		lck_mtx_lock(&mali_queue_lock);
		while (mali_queue_count == 0) {
			uint64_t deadline;

			if (!gov_on || gov_level == 0) {
				lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT);
				continue;
			}
			clock_interval_to_deadline(GOV_IDLE_MS, NSEC_PER_MSEC, &deadline);
			if (lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT,
			    deadline) == THREAD_TIMED_OUT && mali_queue_count == 0) {
				lck_mtx_unlock(&mali_queue_lock);
				gov_window(mach_absolute_time(), 0);    /* (idle: the clock may come down) */
				lck_mtx_lock(&mali_queue_lock);
			}
		}
		chain = mali_queue[mali_queue_head].chain;
		seq = mali_queue[mali_queue_head].seq;
		uint64_t queued = mali_queue[mali_queue_head].queued, job_began = mach_absolute_time();
		fragment = mali_queue[mali_queue_head].fragment;
		lck_mtx_unlock(&mali_queue_lock);

		{
			/* how busy the GPU is: every five seconds, its share of the time and by which slot */
			static uint64_t busy[2], since, jobs[2];
			uint64_t began = mach_absolute_time(), now, ns;

			if (hakios_mali_submit(chain, fragment) != 0) {
				gMaliFailed++;
			}
			now = mach_absolute_time();
			if (mali_trace) {
				uint64_t q, b, e;

				absolutetime_to_nanoseconds(queued, &q);
				absolutetime_to_nanoseconds(began, &b);
				absolutetime_to_nanoseconds(now, &e);
				printf("tl: j %llu %c q %llu b %llu e %llu\n", seq, fragment ? 'f' : 'v', q / 1000, b / 1000, e / 1000);
			}
			absolutetime_to_nanoseconds(now - began, &ns);
			busy[fragment] += ns;
			jobs[fragment]++;
			if (since == 0) {
				since = now;
			} else {
				absolutetime_to_nanoseconds(now - since, &ns);
				if (ns >= 5ull * NSEC_PER_SEC) {
					/* (to the loader's log too: which kind of job the time goes to decides what to do about it) */
					say("mali: GPU busy %llu%% of %llu s: vertex and tiler %llu jobs %llu ms (%llu us each), "
					    "fragment %llu jobs %llu ms (%llu us each)\n", (busy[0] + busy[1]) * 100 / ns,
					    ns / NSEC_PER_SEC, jobs[0], busy[0] / 1000000, jobs[0] ? busy[0] / 1000 / jobs[0] : 0,
					    jobs[1], busy[1] / 1000000, jobs[1] ? busy[1] / 1000 / jobs[1] : 0);
					busy[0] = busy[1] = jobs[0] = jobs[1] = 0;
					since = now;
				}
			}
		}

		uint64_t ended = mach_absolute_time();

		lck_mtx_lock(&mali_queue_lock);
		mali_queue_head = (mali_queue_head + 1) % MALI_QUEUE;
		mali_queue_count--;
		mali_done_seq = seq;
		sec_jobs++;
		sec_busy += ended - job_began;
		sec_queued += job_began - queued;
		lck_mtx_unlock(&mali_queue_lock);
		wakeup((caddr_t)&mali_done_seq);
		wakeup((caddr_t)&mali_queue_head);       /* (room in the queue) */
		gov_window(ended, ended - job_began);
	}
}

/* A job chain into the queue: its number. */
uint64_t
hakios_mali_enqueue(uint64_t chain, bool fragment)
{
	uint64_t seq;

	lck_mtx_lock(&mali_queue_lock);
	if (!mali_worker_started) {
		thread_t thread;

		if (kernel_thread_start(mali_worker, NULL, &thread) == KERN_SUCCESS) {
			thread_set_thread_name(thread, "the Mali's jobs");
			thread_deallocate(thread);
			mali_worker_started = true;
		}
	}
	while (mali_queue_count == MALI_QUEUE) {
		lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_head, THREAD_UNINT);
	}
	seq = ++mali_queued_seq;
	unsigned at = (mali_queue_head + mali_queue_count) % MALI_QUEUE;

	mali_queue[at].chain = chain;
	mali_queue[at].fragment = fragment;
	mali_queue[at].seq = seq;
	mali_queue[at].queued = mach_absolute_time();
	mali_queue_count++;
	lck_mtx_unlock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	return seq;
}

/* Wait until job `seq` is done: true when it is; with timeout 0 only a look. */
bool
hakios_mali_wait(uint64_t seq, uint64_t timeout_ns)
{
	uint64_t deadline;
	bool done;

	if (timeout_ns > 10ull * NSEC_PER_SEC) {
		timeout_ns = 10ull * NSEC_PER_SEC;     /* (an "infinite" one, INT64_MAX, would wrap) */
	}
	nanoseconds_to_absolutetime(timeout_ns, &deadline);
	deadline += mach_absolute_time();
	lck_mtx_lock(&mali_queue_lock);
	while (mali_done_seq < seq && timeout_ns != 0 && mach_absolute_time() < deadline) {
		lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_done_seq, THREAD_UNINT, deadline);
	}
	done = mali_done_seq >= seq;
	lck_mtx_unlock(&mali_queue_lock);
	return done;
}

/* ---- the run ---- */

/* The steps, then, if they all went, once a minute what the GPU has done. */
static void
mali_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("mali: the %s glue's Mali: steps 1 to %u of %u (mali=%u)\n", glue->name, last_step, STEPS, last_step);
	for (step = 1; step <= last_step; step++) {
		say("mali: step %u: %s\n", step, steps[step - 1].name);
		if (!steps[step - 1].make()) {
			say("mali: step %u: FAILED; the steps after it are not made\n", step);
			break;
		}
		reached = step;
		say("mali: step %u: done\n", step);
	}
	if (job_page != 0) {
		/* (after step 9 programs may be at the GPU already: under the locks, as any change of the tables) */
		lck_mtx_lock(&mali_job_lock);
		lck_mtx_lock(&mali_lock);
		if (pt_top != NULL) {
			unmap_pages(FIRST_JOB_GPU, PAGE_SIZE);  /* nothing of the GPU's to point at the page once it is freed */
			as0_update(pt_transtab, pt_transcfg());
		}
		lck_mtx_unlock(&mali_lock);
		lck_mtx_unlock(&mali_job_lock);
		kmem_free(kernel_map, job_page, PAGE_SIZE);
		job_page = 0;
	}
	if (reached < STEPS) {
		if (gpu_answered) {
			say("mali: the steps ended short: a reset, which leaves the GPU's cores off\n");
			if (!gpu_reset()) {
				say("mali: the reset did not finish (GPU_INT_RAWSTAT %#x)\n", rd(mali, GPU_INT_RAWSTAT));
			}
			gpu_answered = false;
		}
		glue->undo();
		say("mali: came up to step %u of %u: not there for programs\n", reached, STEPS);
		return;
	}
	say("mali: came up to step %u of %u: the GPU is there for programs (HakiosMali)\n", reached, STEPS);
	gov_start();
	for (;;) {
		delay(60 * 1000 * 1000);
		printf("mali: step %u of %u; user space says %u%s; %u clients, %u buffers, %u job chains (%u failed), "
		    "%u pictures shown; %llu MB of the GPU's 4 GB of addresses in use\n", reached, STEPS, user_said,
		    user_said == 1 ? " -- a program ran a job on the GPU" : "", gMaliClients, gMaliBuffers, gMaliSubmits,
		    gMaliFailed, gMaliShown, mali_va.taken * MALI_VA_GRANULE >> 20);
		lck_mtx_lock(&kept_lock);
		for (uint32_t i = kept_next > KEPT_LINES ? kept_next - KEPT_LINES : 0; i < kept_next; i++) {
			printf("kept: %s\n", kept_lines[i % KEPT_LINES]);
		}
		lck_mtx_unlock(&kept_lock);
		if (gl_said != 0) {
			printf("mali: gl_test drew on %s; %u%% of the picture inside the triangle (26%% by geometry); "
			    "the middle pixel red %u green %u blue %u alpha %u\n", gl_said / 1000 == 1 ? "THE MALI" : "the processor",
			    gl_said % 1000, gl_middle & 0xff, (gl_middle >> 8) & 0xff, (gl_middle >> 16) & 0xff, gl_middle >> 24);
		}
		if (last_failure.count != 0) {
			printf("mali: the last of %u unfinished: slot %u, chain at %#llx, interrupts %#x, slot status %#x, "
			    "MMU interrupts %#x, fault status %#x at address %#x\n", last_failure.count, last_failure.slot,
			    last_failure.chain, last_failure.raw, last_failure.status, last_failure.mmu, last_failure.fault,
			    last_failure.fault_address);
			printf("mali: the GPU's tables at %#llx (ours at %#llx); for the fault's address: %#llx, %#llx, %#llx\n",
			    last_failure.transtab, pt_transtab, last_failure.walk[0], last_failure.walk[1], last_failure.walk[2]);
			for (uint32_t i = 0; i < gMaliBuffers && i < 32; i++) {
				printf("mali:   buffer %u at %#llx..%#llx (asked %#x, flags %#x)%s\n", i, gMaliMade[i].address,
				    gMaliMade[i].address + gMaliMade[i].size, gMaliMade[i].asked, gMaliMade[i].flags,
				    last_failure.fault_address >= gMaliMade[i].address &&
				    last_failure.fault_address < gMaliMade[i].address + gMaliMade[i].size ? "  <- the fault" : "");
			}
		}
	}
}

/* From hakios_input_init() (drivers/input/hakios.input.c), while the system starts: the steps go on in a thread. */
void
hakios_mali_init(void)
{
	thread_t thread;

	char soc[16] = "exynos9810";

	(void)PE_parse_boot_argn("mali_fastmap", &mali_fastmap, sizeof(mali_fastmap));
	(void)PE_parse_boot_argn("mali_trace", &mali_trace, sizeof(mali_trace));
	char peeks[80] = "";
	if (PE_parse_boot_arg_str("mali_peek", peeks, sizeof(peeks))) {
		char *at = peeks;

		for (int i = 0; i < 4 && *at != '\0'; i++) {
			mali_peek[i] = strtouq(at, &at, 0);
			at += *at == ',';
		}
	}
	(void)PE_parse_boot_argn("mali_gov", &mali_gov, sizeof(mali_gov));
	(void)PE_parse_boot_argn("mali_nap_us", &mali_nap_us, sizeof(mali_nap_us));
	mali_nap_us = mali_nap_us < 10 ? 10 : mali_nap_us > 1000 ? 1000 : mali_nap_us;
	if (!PE_parse_boot_argn("mali", &last_step, sizeof(last_step)) || last_step == 0) {
		return;
	}
	(void)PE_parse_boot_arg_str("mali_soc", soc, sizeof(soc));
	if (strcmp(soc, "exynos9810") == 0) {
		glue = hakios_mali_glue_exynos9810();
	} else if (strcmp(soc, "exynos8890") == 0) {
		glue = hakios_mali_glue_exynos8890();
	} else {
		say("mali: mali_soc=%s: no such glue (exynos9810, exynos8890): nothing touched\n", soc);
		return;
	}
	if (glue == NULL) {
		say("mali: mali_soc=%s: its glue is not built in, or refused: nothing touched\n", soc);
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	if (kernel_thread_start(mali_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
