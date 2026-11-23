#include <arch/idt.h>
#include <arch/io.h>
#include <drivers/keyboard.h>
#include <drivers/serial.h>
#include <stddef.h>
#include <stdint.h>

#define KBD_DATA_PORT 0x60
#define KBD_BUF_SIZE  4096

static const char scancode_normal[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,  '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,  ' ', 0
};

static const char scancode_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,  '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,  ' ', 0
};

static volatile char     g_ring[KBD_BUF_SIZE];
static volatile uint32_t g_head = 0;
static volatile uint32_t g_tail = 0;
static bool              g_shift = false;
static bool              g_ctrl = false;
static bool              g_caps = false;
static bool              g_ext = false;
static input_sink_t      g_sink = NULL;

static void push_char(char c) {
    if (g_sink) {
        g_sink(c);
        return;
    }
    uint32_t next = (g_head + 1) % KBD_BUF_SIZE;
    if (next != g_tail) {
        g_ring[g_head] = c;
        g_head = next;
    }
}

static void push_str(const char *s) {
    while (*s) {
        push_char(*s++);
    }
}

static void keyboard_irq(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) {
        return;
    }
    uint8_t sc = inb(KBD_DATA_PORT);
    if (status & 0x20) {
        return;
    }

    if (sc == 0xE0) {
        g_ext = true;
        return;
    }

    if (g_ext) {
        g_ext = false;
        if (sc & 0x80) {
            return;
        }
        switch (sc) {
            case 0x48: push_str("\x1b[A");  return; /* Up */
            case 0x50: push_str("\x1b[B");  return; /* Down */
            case 0x4D: push_str("\x1b[C");  return; /* Right */
            case 0x4B: push_str("\x1b[D");  return; /* Left */
            case 0x47: push_str("\x1b[H");  return; /* Home */
            case 0x4F: push_str("\x1b[F");  return; /* End */
            case 0x53: push_str("\x1b[3~"); return; /* Delete */
            case 0x49: push_str("\x1b[5~"); return; /* Page Up */
            case 0x51: push_str("\x1b[6~"); return; /* Page Down */
            case 0x1C: push_char('\n');     return; /* Keypad Enter */
            case 0x35: push_char('/');      return; /* Keypad / */
            default:   return;
        }
    }

    if (sc == 0x2A || sc == 0x36) { g_shift = true; return; }
    if (sc == 0xAA || sc == 0xB6) { g_shift = false; return; }
    if (sc == 0x1D)               { g_ctrl = true; return; }
    if (sc == 0x9D)               { g_ctrl = false; return; }
    if (sc == 0x3A)               { g_caps = !g_caps; return; }

    /* Ignore key releases */
    if (sc & 0x80) {
        return;
    }

    char c = g_shift ? scancode_shift[sc] : scancode_normal[sc];
    if (g_caps && c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    } else if (g_caps && c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
    }
    if (g_ctrl && c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 1);
    } else if (g_ctrl && c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 1);
    }
    if (c != 0) {
        push_char(c);
    }
}

void keyboard_poll_serial(void) {
    while (serial_has_data()) {
        int ch = serial_getc_nonblock();
        if (ch < 0) {
            break;
        }
        if (ch == '\r') {
            ch = '\n';
        }
        push_char((char)ch);
    }
}

static void serial_rx_irq(struct interrupt_frame *frame) {
    (void)frame;
    (void)inb(COM1_PORT + 2); /* Acknowledge IIR */
    keyboard_poll_serial();
}

void keyboard_init(void) {
    /* Drain any pending PS/2 and COM1 bytes */
    while (inb(0x64) & 0x01) {
        (void)inb(KBD_DATA_PORT);
    }
    while (serial_has_data()) {
        (void)serial_getc_nonblock();
    }
    serial_enable_rx_irq();
    irq_register_handler(1, keyboard_irq);
    irq_register_handler(4, serial_rx_irq);
}

void keyboard_set_sink(input_sink_t sink) {
    g_sink = sink;
}

int keyboard_getc_nonblock(void) {
    if (g_head == g_tail) {
        return -1;
    }
    char c = g_ring[g_tail];
    g_tail = (g_tail + 1) % KBD_BUF_SIZE;
    return (unsigned char)c;
}
