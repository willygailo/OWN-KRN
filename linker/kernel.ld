OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_PHYS_BOOT = 0x00100000;
KERNEL_PHYS_HIGH = 0x00200000;
KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;

PHDRS
{
    boot      PT_LOAD FLAGS(5); /* R | X */
    boot_data PT_LOAD FLAGS(6); /* R | W */
    note      PT_NOTE FLAGS(4); /* R */
    text      PT_LOAD FLAGS(5); /* R | X */
    rodata    PT_LOAD FLAGS(4); /* R */
    data      PT_LOAD FLAGS(6); /* R | W */
}

SECTIONS
{
    __kernel_phys_start = KERNEL_PHYS_BOOT;
    __kernel_virt_base  = KERNEL_VIRT_BASE;

    /* ------------------------------------------------------------------ */
    /* Low-memory 32-bit -> 64-bit bootstrap code (identity mapped)       */
    /* ------------------------------------------------------------------ */
    . = KERNEL_PHYS_BOOT;

    .boot.text ALIGN(4K) : AT(ADDR(.boot.text))
    {
        KEEP(*(.multiboot))
        *(.boot.text)
        *(.boot.rodata)
    } :boot

    .note.Xen ALIGN(4) : AT(ADDR(.note.Xen))
    {
        KEEP(*(.note.Xen))
    } :boot :note

    .boot.data ALIGN(4K) : AT(ADDR(.boot.data))
    {
        *(.boot.data)
    } :boot_data

    .boot.bss ALIGN(4K) (NOLOAD) : AT(ADDR(.boot.bss))
    {
        *(.boot.bss)
    } :boot_data

    /* ------------------------------------------------------------------ */
    /* Higher-half 64-bit kernel image (VMA = 0xFFFFFFFF80200000+)        */
    /* ------------------------------------------------------------------ */
    . = KERNEL_VIRT_BASE + KERNEL_PHYS_HIGH;

    __kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRT_BASE)
    {
        __text_start = .;
        *(.text .text.*)
        . = ALIGN(4K);
        __text_end = .;
    } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(4K);
        __rodata_end = .;
    } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRT_BASE)
    {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4K);
        __data_end = .;
    } :data

    .bss ALIGN(4K) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VIRT_BASE)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4K);
        __bss_end = .;
    } :data

    __kernel_end      = .;
    __kernel_phys_end = . - KERNEL_VIRT_BASE;

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.comment)
        *(.note.gnu.*)
    }
}
