#ifndef OWNKRN_ARCH_GDT_H
#define OWNKRN_ARCH_GDT_H

#include <stdint.h>

/* Selector layout strictly ordered for x86_64 SYSCALL/SYSRET:
 *   STAR[47:32] = 0x08 -> SYSCALL: CS=0x08, SS=0x10
 *   STAR[63:48] = 0x10 -> SYSRET:  SS=(0x10+8)|3=0x1B, CS=(0x10+16)|3=0x23
 */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

#define USER_DS         (GDT_USER_DATA | 3)
#define USER_CS         (GDT_USER_CODE | 3)

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);
uint64_t tss_get_rsp0(void);

#endif /* OWNKRN_ARCH_GDT_H */
