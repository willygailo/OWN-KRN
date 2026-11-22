#include <arch/gdt.h>
#include <kernel/types.h>
#include <lib/string.h>

extern char kernel_boot_stack_top[];

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
} PACKED;

struct gdt_tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} PACKED;

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} PACKED;

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} PACKED;

struct gdt_table {
    struct gdt_entry     null_desc;
    struct gdt_entry     kcode;
    struct gdt_entry     kdata;
    struct gdt_entry     udata;
    struct gdt_entry     ucode;
    struct gdt_tss_entry tss;
} PACKED;

static struct gdt_table g_gdt ALIGNED(16);
static struct tss64     g_tss ALIGNED(16);
static uint8_t          g_df_stack[16384] ALIGNED(16);

extern void gdt_flush(const struct gdt_ptr *gdtr, uint16_t tss_sel);

static void set_entry(struct gdt_entry *e, uint8_t access, uint8_t gran) {
    e->limit_low = 0xFFFF;
    e->base_low  = 0;
    e->base_mid  = 0;
    e->access    = access;
    e->gran      = gran;
    e->base_high = 0;
}

static void set_tss(struct gdt_tss_entry *e, uint64_t base, uint32_t limit) {
    e->limit_low  = (uint16_t)(limit & 0xFFFF);
    e->base_low   = (uint16_t)(base & 0xFFFF);
    e->base_mid   = (uint8_t)((base >> 16) & 0xFF);
    e->access     = 0x89; /* Present, DPL=0, 64-bit available TSS */
    e->gran       = (uint8_t)((limit >> 16) & 0x0F);
    e->base_high  = (uint8_t)((base >> 24) & 0xFF);
    e->base_upper = (uint32_t)(base >> 32);
    e->reserved   = 0;
}

void gdt_init(void) {
    memset(&g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss, 0, sizeof(g_tss));

    /* 0x08: Kernel Code (64-bit, DPL=0) */
    set_entry(&g_gdt.kcode, 0x9A, 0xAF);
    /* 0x10: Kernel Data (DPL=0) */
    set_entry(&g_gdt.kdata, 0x92, 0xCF);
    /* 0x18: User Data (DPL=3) — MUST precede User Code for SYSRET */
    set_entry(&g_gdt.udata, 0xF2, 0xCF);
    /* 0x20: User Code (64-bit, DPL=3) */
    set_entry(&g_gdt.ucode, 0xFA, 0xAF);

    g_tss.rsp0        = (uint64_t)(uintptr_t)kernel_boot_stack_top;
    g_tss.ist1        = (uint64_t)(uintptr_t)(g_df_stack + sizeof(g_df_stack));
    g_tss.iopb_offset = sizeof(struct tss64);

    set_tss(&g_gdt.tss, (uint64_t)(uintptr_t)&g_tss, sizeof(struct tss64) - 1);

    struct gdt_ptr gdtr = {
        .limit = sizeof(g_gdt) - 1,
        .base  = (uint64_t)(uintptr_t)&g_gdt,
    };
    gdt_flush(&gdtr, GDT_TSS);
}

void tss_set_rsp0(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}

uint64_t tss_get_rsp0(void) {
    return g_tss.rsp0;
}
