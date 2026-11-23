#include <arch/io.h>
#include <drivers/keyboard.h>
#include <drivers/serial.h>

static void uart_init_port(uint16_t port) {
    if (inb(port + 5) == 0xFF) {
        return; /* UART port not present */
    }
    outb(port + 1, 0x00); /* Disable interrupts */
    outb(port + 3, 0x80); /* Enable DLAB (set baud rate divisor) */
    outb(port + 0, 0x01); /* Divisor low byte: 115200 baud */
    outb(port + 1, 0x00); /* Divisor high byte */
    outb(port + 3, 0x03); /* 8 bits, no parity, one stop bit (8N1) */
    outb(port + 2, 0x07); /* Enable FIFO, clear RX/TX, 1-byte trigger threshold */
    outb(port + 4, 0x0B); /* IRQs enabled, RTS/DSR set */

    /* Drain any stale RX bytes (bounded) and clear pending IIR/LSR latches */
    for (int i = 0; i < 32; i++) {
        uint8_t lsr = inb(port + 5);
        if (lsr == 0xFF || !(lsr & 0x01)) {
            break;
        }
        (void)inb(port);
    }
    (void)inb(port + 2);
}

void serial_init(void) {
    uart_init_port(COM1_PORT);
    uart_init_port(COM2_PORT);
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
            keyboard_poll_serial();
            cpu_pause();
        }
        outb(COM1_PORT, '\r');
    }
    while (!serial_tx_ready()) {
        keyboard_poll_serial();
        cpu_pause();
    }
    outb(COM1_PORT, (uint8_t)c);
    keyboard_poll_serial();
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

/* ---------------- COM2 (0x2F8) Host Clipboard Bridge ---------------- */

static bool com2_tx_ready(void) {
    uint8_t lsr = inb(COM2_PORT + 5);
    return lsr != 0xFF && (lsr & 0x20) != 0;
}

static bool com2_has_data(void) {
    uint8_t lsr = inb(COM2_PORT + 5);
    return lsr != 0xFF && (lsr & 0x01) != 0;
}

static void com2_putc(uint8_t b) {
    for (uint32_t spin = 0; spin < 50000U; spin++) {
        if (com2_tx_ready()) {
            outb(COM2_PORT, b);
            return;
        }
        cpu_pause();
    }
}

size_t serial_clipboard_sync_from_host(char *dst, size_t max_len) {
    if (!dst || max_len == 0) {
        return 0;
    }
    /* Check if COM2 UART exists (LSR != 0xFF) */
    if (inb(COM2_PORT + 5) == 0xFF) {
        return 0;
    }

    /* Drain stale bytes on COM2 */
    while (com2_has_data()) {
        (void)inb(COM2_PORT);
    }

    /* Send ENQ (0x05) requesting current Host OS clipboard */
    com2_putc(0x05);

    /* Wait briefly for ACK (0x06) from scripts/clipboard_bridge.py */
    bool got_ack = false;
    for (uint32_t spin = 0; spin < 120000U; spin++) {
        if (com2_has_data()) {
            uint8_t b = inb(COM2_PORT);
            if (b == 0x06) {
                got_ack = true;
                break;
            }
        }
        cpu_pause();
    }
    if (!got_ack) {
        return 0;
    }

    /* Read clipboard payload until EOT (0x04) or timeout */
    size_t n = 0;
    uint32_t idle = 0;
    while (n < max_len && idle < 200000U) {
        if (com2_has_data()) {
            uint8_t b = inb(COM2_PORT);
            idle = 0;
            if (b == 0x04) { /* EOT */
                break;
            }
            if (b == '\r') {
                continue;
            }
            dst[n++] = (char)b;
        } else {
            idle++;
            cpu_pause();
        }
    }
    return n;
}

void serial_clipboard_send_to_host(const char *src, size_t len) {
    if (!src || inb(COM2_PORT + 5) == 0xFF) {
        return;
    }
    com2_putc(0x02); /* STX */
    for (size_t i = 0; i < len; i++) {
        uint8_t b = (uint8_t)src[i];
        if (b == 0x02 || b == 0x03 || b == 0x04 || b == 0x05) {
            continue;
        }
        com2_putc(b);
    }
    com2_putc(0x03); /* ETX */
}
