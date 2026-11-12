// Bare-metal CPU probe for the Note FE (Exynos 8890). S-Boot jumps here like a Linux arm64 Image:
// MMU off, x0 = DTB, on an A53. Everything is PC-relative, so the load address does not matter.

#define PMU_INFORM2	0x105C0808
#define PMU_INFORM3	0x105C080C

.macro set_vbar tmp, tmp2
	adr	\tmp, vectors
	mrs	\tmp2, CurrentEL
	cmp	\tmp2, #(2 << 2)
	b.eq	1f
	msr	vbar_el1, \tmp
	b	2f
1:	msr	vbar_el2, \tmp
2:	isb
.endm

// stack top for slot \slot (a register): stacks + (slot + 1) * 4096
.macro set_stack slot, tmp
	adr	\tmp, stacks
	add	\tmp, \tmp, \slot, lsl #12
	add	sp, \tmp, #4096
.endm

.section .text.head, "ax"
.global _start
_start:
	b	primary_entry		// code0
	.long	0			// code1
	.quad	0x80000			// text_offset, as the stock kernel
	.quad	_image_end - _start	// image_size, .bss included
	.quad	0			// flags
	.quad	0, 0, 0
	.ascii	"ARM\x64"
	.long	0

primary_entry:
	msr	daifset, #0xf
	// First thing: any reset from here on lands in TWRP (sec_reboot's recovery mode), even a watchdog one.
	ldr	x1, =PMU_INFORM3
	ldr	w2, =0x12345674
	str	w2, [x1]
	ldr	x1, =PMU_INFORM2
	ldr	w2, =0x12345678
	str	w2, [x1]
	dsb	sy

	mov	x20, x0			// DTB
	// .bss is whatever RAM held: drop any stale cache lines over it, then zero it (MMU off: uncached stores)
	adr	x1, _bss_start
	adr	x2, _image_end
3:	dc	civac, x1
	add	x1, x1, #64
	cmp	x1, x2
	b.lo	3b
	dsb	sy
	adr	x1, _bss_start
4:	stp	xzr, xzr, [x1], #16
	cmp	x1, x2
	b.lo	4b
	dsb	sy

	mov	x1, #8			// the primary uses the last stack slot
	set_stack x1, x2
	set_vbar x2, x3
	mov	x0, x20
	bl	probe_main
	b	hang

.global secondary_entry
secondary_entry:
	msr	daifset, #0xf
	mrs	x0, mpidr_el1
	ubfx	x1, x0, #8, #8		// cluster
	and	x2, x0, #0xff		// core
	add	x1, x2, x1, lsl #2	// slot = cluster * 4 + core
	cmp	x1, #7
	b.ls	1f
	mov	x1, #7
	// alive[slot] = CurrentEL << 32 | 1, before anything that could fault
1:	mrs	x3, CurrentEL
	lsl	x3, x3, #32
	orr	x3, x3, #1
	adr	x4, alive
	str	x3, [x4, x1, lsl #3]
	dsb	sy
	set_stack x1, x2
	set_vbar x2, x3
	bl	secondary_main
hang:
	wfi
	b	hang

// Every exception goes to exc_common with its vector index in x0.
.macro vec idx
	.balign	0x80
	mov	x0, #\idx
	b	exc_common
.endm

.balign 2048
vectors:
	vec 0
	vec 1
	vec 2
	vec 3
	vec 4
	vec 5
	vec 6
	vec 7
	vec 8
	vec 9
	vec 10
	vec 11
	vec 12
	vec 13
	vec 14
	vec 15

exc_common:
	mrs	x4, CurrentEL
	cmp	x4, #(2 << 2)
	b.eq	1f
	mrs	x1, esr_el1
	mrs	x2, elr_el1
	mrs	x3, far_el1
	b	2f
1:	mrs	x1, esr_el2
	mrs	x2, elr_el2
	mrs	x3, far_el2
2:	bl	exception_main
	b	hang

.ltorg

.section .bss
.global alive
.balign 64
alive:
	.space	8 * 8
.balign 4096
.global stacks
stacks:
	.space	4096 * 9
