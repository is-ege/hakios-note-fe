ENTRY(_start)
SECTIONS
{
	. = 0x80080000;
	.text : { KEEP(*(.text.head)) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(4096);
	_bss_start = .;
	.bss (NOLOAD) : { *(.bss*) *(COMMON) }
	. = ALIGN(4096);
	_image_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
