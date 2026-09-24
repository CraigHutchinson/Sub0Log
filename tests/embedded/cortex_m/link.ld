/* Minimal image for QEMU's MPS2 boards: everything in one RAM region the
 * loader places directly (no flash-to-RAM copy), vector table first.
 * RAM_ORIGIN is 0x00000000 on AN385/AN386/AN500 (Cortex-M3/M4/M7) and
 * 0x10000000 on AN505 (Cortex-M33, secure alias) -- passed with --defsym. */
MEMORY
{
    RAM (rwx) : ORIGIN = RAM_ORIGIN, LENGTH = 4M
}

ENTRY(Reset_Handler)

SECTIONS
{
    .vectors : { KEEP(*(.vectors)) } > RAM
    .text : { *(.text .text.*) KEEP(*(.init)) KEEP(*(.fini)) } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > RAM
    .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > RAM
    .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } > RAM
    .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } > RAM
    .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } > RAM
    .data : { *(.data .data.*) } > RAM
    /* One task, so one TLS block: __aeabi_read_tp returns its base. */
    .tdata : { __tdata_start = .; *(.tdata .tdata.*) __tdata_end = .; } > RAM
    .tbss : { *(.tbss .tbss.*) } > RAM
    .bss (NOLOAD) : { __bss_start__ = .; *(.bss .bss.* COMMON) __bss_end__ = .; } > RAM
    . = ALIGN(8);
    end = .;
    __end__ = .;
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
