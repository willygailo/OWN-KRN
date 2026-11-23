#include <arch/idt.h>
#include <arch/io.h>
#include <arch/pit.h>
#include <drivers/keyboard.h>
#include <drivers/serial.h>
#include <lib/string.h>

static char     g_clip_buf[SERIAL_CLIP_MAX];
static size_t   g_clip_len = 0;

static char     g_com2_rx_buf[SERIAL_CLIP_MAX];
static size_t   g_com2_rx_len = 0;
static bool     g_com2_in_stx = false;
static bool     g_serial_poll_ready = false;
static volatile uint32_t g_com2_sync_seq = 0;

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

void serial_com2_poll(void) {
    while (com2_has_data()) {
        uint8_t b = inb(COM2_PORT);
        if (b == 0x02) { /* STX: start of host clipboard frame */
            g_com2_in_stx = true;
            g_com2_rx_len = 0;
            com2_putc(0x06); /* ACK */
        } else if (b == 0x03 && g_com2_in_stx) { /* ETX: end of host clipboard frame */
            g_com2_in_stx = false;
            memcpy(g_clip_buf, g_com2_rx_buf, g_com2_rx_len);
            g_clip_buf[g_com2_rx_len] = '\0';
            g_clip_len = g_com2_rx_len;
            g_com2_sync_seq++;
            com2_putc(0x06); /* ACK */
        } else if (b == 0x07 && g_com2_in_stx) { /* BEL: 12-byte chunk flow-control ping */
            com2_putc(0x06); /* ACK */
        } else if (g_com2_in_stx) {
            if (b != '\r' && g_com2_rx_len + 1 < SERIAL_CLIP_MAX) {
                g_com2_rx_buf[g_com2_rx_len++] = (char)b;
            }
        }
    }
}

static void com2_rx_irq(struct interrupt_frame *frame) {
    (void)frame;
    if (inb(COM2_PORT + 5) != 0xFF) {
        (void)inb(COM2_PORT + 2); /* Acknowledge IIR */
        serial_com2_poll();
    }
}

void serial_enable_rx_irq(void) {
    outb(COM1_PORT + 1, 0x01); /* Enable COM1 RX interrupt (IRQ 4) */
    if (inb(COM2_PORT + 5) != 0xFF) {
        outb(COM2_PORT + 1, 0x01); /* Enable COM2 RX interrupt (IRQ 3) */
        irq_register_handler(3, com2_rx_irq);
    }
    g_serial_poll_ready = true;
}

static bool serial_tx_ready(void) {
    return (inb(COM1_PORT + 5) & 0x20) != 0;
}

void serial_putc(char c) {
    if (c == '\n') {
        while (!serial_tx_ready()) {
            if (g_serial_poll_ready) {
                keyboard_poll_serial();
                serial_com2_poll();
            }
            cpu_pause();
        }
        outb(COM1_PORT, '\r');
    }
    while (!serial_tx_ready()) {
        if (g_serial_poll_ready) {
            keyboard_poll_serial();
            serial_com2_poll();
        }
        cpu_pause();
    }
    outb(COM1_PORT, (uint8_t)c);
    if (g_serial_poll_ready) {
        keyboard_poll_serial();
        serial_com2_poll();
    }
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

size_t serial_clipboard_read(char *dst, size_t max_len) {
    if (!dst || max_len == 0) {
        return 0;
    }

    serial_com2_poll();

    /* Request fresh snapshot from Host Bridge via ENQ (0x05) */
    if (inb(COM2_PORT + 5) != 0xFF) {
        uint32_t seq_before = g_com2_sync_seq;
        com2_putc(0x05);

        /* Wait up to ~60ms (6 PIT ticks) for the host bridge to start/complete STX..ETX,
           or up to ~400ms while a multi-kilobyte STX transfer is actively streaming */
        uint64_t start_tick = pit_ticks();
        uint64_t last_progress_tick = start_tick;
        size_t last_rx_len = g_com2_rx_len;

        for (uint32_t spin = 0; spin < 2000000U; spin++) {
            serial_com2_poll();
            if (g_com2_sync_seq != seq_before) {
                break; /* Fresh clipboard snapshot received! */
            }
            uint64_t now = pit_ticks();
            if (g_com2_in_stx) {
                if (g_com2_rx_len != last_rx_len) {
                    last_rx_len = g_com2_rx_len;
                    last_progress_tick = now;
                }
                if (now - last_progress_tick > 25) { /* 250ms stall timeout */
                    g_com2_in_stx = false;
                    break;
                }
            } else {
                /* If no STX started within 4 ticks (40ms) or 150k spins, use cached buffer */
                if ((now > start_tick && (now - start_tick) >= 4) || spin > 150000U) {
                    break;
                }
            }
            cpu_pause();
        }
    }

    size_t to_copy = (g_clip_len < max_len) ? g_clip_len : max_len;
    if (to_copy > 0) {
        memcpy(dst, g_clip_buf, to_copy);
    }
    return to_copy;
}

void serial_clipboard_write(const char *src, size_t len) {
    if (!src) {
        return;
    }
    size_t to_copy = (len < SERIAL_CLIP_MAX - 1) ? len : (SERIAL_CLIP_MAX - 1);
    memcpy(g_clip_buf, src, to_copy);
    g_clip_buf[to_copy] = '\0';
    g_clip_len = to_copy;

    if (inb(COM2_PORT + 5) == 0xFF) {
        return;
    }
    com2_putc(0x02); /* STX */
    for (size_t i = 0; i < to_copy; i++) {
        uint8_t b = (uint8_t)src[i];
        if (b == 0x02 || b == 0x03 || b == 0x04 || b == 0x05 || b == 0x07) {
            continue;
        }
        com2_putc(b);
    }
    com2_putc(0x03); /* ETX */
}
