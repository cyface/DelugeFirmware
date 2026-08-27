/* Position-independent plugin blob: header, then entry code, then the rest. Linked at 0; the host
 * copies the raw bytes anywhere (SDRAM) and calls base + entryOffset. */
ENTRY(spike_render)
SECTIONS
{
    . = 0;
    .dlp : {
        KEEP(*(.dlp_header))
        . = ALIGN(16);
        __entry = .;
        KEEP(*(.text.entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    }
    .data : { *(.data .data.*) }
    .bss  : { *(.bss .bss.* COMMON) }
    .got  : { *(.got .got.*) }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
ASSERT(__entry == 16, "entry must sit at DLP_ENTRY_OFFSET (16)")
ASSERT(SIZEOF(.data) == 0, "plugin must not have .data (no relocation support)")
ASSERT(SIZEOF(.bss) == 0, "plugin must not have .bss (state comes from the host)")
ASSERT(SIZEOF(.got) == 0, "plugin must not need a GOT (-mpic-data-is-text-relative, no globals)")
