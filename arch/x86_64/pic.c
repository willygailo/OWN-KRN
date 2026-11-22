#include <arch/idt.h>
#include <arch/io.h>
#include <arch/pic.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x11
#define ICW4_8086 0x01

void pic_init(void) {
    /* Start initialization cascade */
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();

    /* Remap IRQs: master -> 32..39, slave -> 40..47 */
    outb(PIC1_DATA, IRQ_BASE);     io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();

    /* Cascade wiring: slave on IRQ2 */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    /* 8086/88 mode */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask all IRQs initially except cascade line IRQ2 */
    outb(PIC1_DATA, 0xFB);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}

void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq < 8) ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) | (1U << bit)));
}

void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq < 8) ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) & ~(1U << bit)));
}

uint16_t pic_get_isr(void) {
    outb(PIC1_CMD, 0x0B);
    outb(PIC2_CMD, 0x0B);
    return (uint16_t)inb(PIC1_CMD) | ((uint16_t)inb(PIC2_CMD) << 8);
}
