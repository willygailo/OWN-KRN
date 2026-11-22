OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5); /* R | X */
    rodata PT_LOAD FLAGS(4); /* R */
    data   PT_LOAD FLAGS(6); /* R | W */
}

SECTIONS
{
    . = 0x0000000000400000;

    .text ALIGN(4K) :
    {
        *(.text .text.*)
    } :text

    .rodata ALIGN(4K) :
    {
        *(.rodata .rodata.*)
    } :rodata

    .data ALIGN(4K) :
    {
        *(.data .data.*)
    } :data

    .bss ALIGN(4K) (NOLOAD) :
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.comment)
        *(.note.*)
    }
}
