#include <arch/io.h>
#include <drivers/serial.h>

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00); /* Disable interrupts */
    outb(COM1_PORT + 3, 0x80); /* Enable DLAB (set baud rate divisor) */
    outb(COM1_PORT + 0, 0x01); /* Divisor low byte: 115200 baud */
    outb(COM1_PORT + 1, 0x00); /* Divisor high byte */
    outb(COM1_PORT + 3, 0x03); /* 8 bits, no parity, one stop bit (8N1) */
    outb(COM1_PORT + 2, 0x07); /* Enable FIFO, clear RX/TX, 1-byte trigger threshold */
    outb(COM1_PORT + 4, 0x0B); /* IRQs enabled, RTS/DSR set */

    /* Drain any stale RX bytes and clear pending IIR/LSR latches */
    while (inb(COM1_PORT + 5) & 0x01) {
        (void)inb(COM1_PORT);
    }
    (void)inb(COM1_PORT + 2);
}

void serial_enable_rx_irq(void) {
    outb(COM1_PORT + 1, 0x01); /* Enable Received Data Available interrupt */
}

static bool serial_tx_ready(void) {
    return (inb(COM1_PORT + 5) & 0x20) != 0;
}

void serial_putc(char c) {
    if (c == '\n') {
        while (!serial_tx_ready()) {
            cpu_pause();
        }
        outb(COM1_PORT, '\r');
    }
    while (!serial_tx_ready()) {
        cpu_pause();
    }
    outb(COM1_PORT, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}

bool serial_has_data(void) {
    return (inb(COM1_PORT + 5) & 0x01) != 0;
}

int serial_getc_nonblock(void) {
    if (!serial_has_data()) {
        return -1;
    }
    return (int)inb(COM1_PORT);
}
