OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
    TEXT (rx) : ORIGIN = 0x20020000, LENGTH = 64K
    DATA (rw) : ORIGIN = 0x20030000, LENGTH = 64K
    RODATA (r) : ORIGIN = 0x20040000, LENGTH = 32K
}
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    rodata PT_LOAD FLAGS(4);
    stack PT_GNU_STACK FLAGS(6);
}
SECTIONS {
    /* NOLOAD tails reserve exact PMP regions without adding bytes to the file. */
    .text : { *(.text.start) *(.text .text.*) } > TEXT :text
    .text_tail (NOLOAD) : { . = ORIGIN(TEXT) + LENGTH(TEXT); } > TEXT :text
    .data : { __global_pointer$ = . + 0x800; *(.sdata .sdata.* .data .data.*) } > DATA :data
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(16);
        __heap_start = .;
        . = ORIGIN(DATA) + LENGTH(DATA);
        __heap_end = .;
    } > DATA :data
    .rodata : { *(.srodata .srodata.* .rodata .rodata.*) } > RODATA :rodata
    .rodata_tail (NOLOAD) : { . = ORIGIN(RODATA) + LENGTH(RODATA); } > RODATA :rodata
    /DISCARD/ : { *(.note.GNU-stack .eh_frame .comment) }
}
