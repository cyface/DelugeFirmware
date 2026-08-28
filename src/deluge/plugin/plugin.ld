/* Link a plugin kernel the way the tier-2 loader needs it: one image, .text then .rodata, at address 0.
 *
 * The blob carries no header of its own - pack_dlp.py prepends the .dlp header, tables and strings and records
 * the entry points as offsets into this image (see plugin_blob.h), so nothing in here depends on where it lands.
 * The asserts are the contract from plugin/README.md made structural: a kernel with a global, a static or an
 * import fails the link rather than the load.
 *
 *   arm-none-eabi-gcc <target flags> -fPIC -mpic-data-is-text-relative -ffreestanding -nostdlib \
 *       -Wl,-T,plugin.ld -Wl,-e,0 kernel.o -o kernel.elf
 */
SECTIONS
{
	. = 0;
	.image : {
		*(.text .text.*)
		. = ALIGN(4);
		*(.rodata .rodata.*)
		. = ALIGN(4);
	}
	/* Kept (rather than discarded) so the asserts below can see they are empty. */
	.data : { *(.data .data.* .sdata .sdata.*) }
	.bss : { *(.bss .bss.* COMMON) }
	.got : { *(.got .got.plt .got.*) }
	/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.ARM.attributes) }
}
ASSERT(SIZEOF(.data) == 0, "plugin kernel must have no .data: no globals, no statics, state comes from the host")
ASSERT(SIZEOF(.bss) == 0, "plugin kernel must have no .bss: no globals, no statics, state comes from the host")
ASSERT(SIZEOF(.got) == 0, "plugin kernel must need no GOT: reach the firmware through DelugePluginHostApi only")
