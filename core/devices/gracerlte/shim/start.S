/*
 * The Note FE boot shim's entry. S-Boot jumps here as to a Linux arm64 Image (Documentation/arch/arm64/booting.rst):
 * x0 = the device tree, MMU off, at EL1 on an A53 (seen by probe/, PLAN.md Phase 0). PSCI is by SMC to EL3.
 *
 * Derived from Scion's Tensor G5 shim (hakios-soc-lga, MIT); its EL2 half is gone: nothing here runs at EL2.
 */

	.section .text.head, "ax"
	.globl	_head
_head:
	b	entry                   /* code0 */
	.long	0                       /* code1 */
	.quad	0x80000                 /* text_offset: as the stock kernel's, where S-Boot was seen to load the probe */
	.quad	0                       /* image_size: patched by mkboot.sh to the shim and payload's size */
	.quad	0xa                     /* flags: little endian, 4 KB pages, anywhere in RAM */
	.quad	0, 0, 0                 /* res2-4 */
	.ascii	"ARM\x64"               /* magic */
	.long	0                       /* res5 */

	.text
entry:
	msr	daifset, #0xf
	mov	x20, x0                 /* the device tree */
	adr	x21, _head              /* where S-Boot put us */

	/* relocate: every R_AARCH64_RELATIVE in .rela.dyn, *(base + offset) = base + addend */
	adrp	x9, __rela_start
	add	x9, x9, :lo12:__rela_start
	adrp	x10, __rela_end
	add	x10, x10, :lo12:__rela_end
1:	cmp	x9, x10
	b.hs	2f
	ldp	x11, x12, [x9], #16     /* r_offset, r_info */
	ldr	x13, [x9], #8           /* r_addend */
	cmp	w12, #1027              /* R_AARCH64_RELATIVE */
	b.ne	1b
	add	x13, x13, x21
	str	x13, [x21, x11]
	b	1b
2:
	/* .bss is part of the file (shim.ld), already zero: the payload lies behind it, not over it */
	adrp	x9, boot_stack_top
	add	x9, x9, :lo12:boot_stack_top
	mov	sp, x9
	/* FP and SIMD on at EL1 (CPACR_EL1.FPEN = 0b11), as UEFI leaves them for an application */
	mov	x9, #(3 << 20)
	msr	cpacr_el1, x9
	adrp	x9, el1_vectors
	add	x9, x9, :lo12:el1_vectors
	msr	vbar_el1, x9
	isb
	mov	x0, x20
	bl	shim_main
3:	wfe
	b	3b

/*
 * EL1's vectors while the shim runs (the loader and XNU install their own): any exception is reported,
 * shim_el1_fault(ESR, ELR, FAR, which vector), and the phone restarted.
 */
	.balign	2048
el1_vectors:
	.irp	n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
	.balign	128
	mov	x3, #\n
	b	el1_fault
	.endr
el1_fault:
	mrs	x0, esr_el1
	mrs	x1, elr_el1
	mrs	x2, far_el1
	adrp	x9, boot_stack_top
	add	x9, x9, :lo12:boot_stack_top
	mov	sp, x9
	bl	shim_el1_fault
4:	wfe
	b	4b

	.bss
	.balign	16
boot_stack:
	.space	0x40000
boot_stack_top:
