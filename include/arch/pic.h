#ifndef OWNKRN_ARCH_PIC_H
#define OWNKRN_ARCH_PIC_H

#include <stdint.h>

void     pic_init(void);
void     pic_eoi(uint8_t irq);
void     pic_mask(uint8_t irq);
void     pic_unmask(uint8_t irq);
uint16_t pic_get_isr(void);

#endif /* OWNKRN_ARCH_PIC_H */
