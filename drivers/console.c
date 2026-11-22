#include <arch/io.h>
#include <drivers/console.h>
#include <kernel/types.h>

#define VGA_PHYS_ADDR 0xB8000ULL
#define VGA_COLS      80
#define VGA_ROWS      25

static volatile uint16_t *vga_buf;
static int cursor_row = 0;
static int cursor_col = 0;
static uint8_t vga_attr = 0x07; /* Light grey on black */

/* Minimal ANSI CSI escape parser for clear-screen (\x1b[2J) and home (\x1b[H) */
static int  esc_state = 0; /* 0=normal, 1=saw ESC, 2=in CSI */
static char esc_buf[16];
static int  esc_len = 0;

static uint16_t vga_entry(char c, uint8_t attr) {
    return (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

static void update_hw_cursor(void) {
    uint16_t pos = (uint16_t)(cursor_row * VGA_COLS + cursor_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void console_scroll(void) {
    for (int r = 1; r < VGA_ROWS; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            vga_buf[(r - 1) * VGA_COLS + c] = vga_buf[r * VGA_COLS + c];
        }
    }
    for (int c = 0; c < VGA_COLS; c++) {
        vga_buf[(VGA_ROWS - 1) * VGA_COLS + c] = vga_entry(' ', vga_attr);
    }
    cursor_row = VGA_ROWS - 1;
}

void console_init(void) {
    vga_buf = (volatile uint16_t *)PHYS_TO_VIRT(VGA_PHYS_ADDR);
    vga_attr = 0x07;
    esc_state = 0;
    console_clear();
}

void console_set_color(uint8_t fg, uint8_t bg) {
    vga_attr = (uint8_t)((fg & 0x0F) | ((bg & 0x0F) << 4));
}

void console_clear(void) {
    if (!vga_buf) {
        return;
    }
    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++) {
        vga_buf[i] = vga_entry(' ', vga_attr);
    }
    cursor_row = 0;
    cursor_col = 0;
    update_hw_cursor();
}

void console_putc(char c) {
    if (!vga_buf) {
        return;
    }

    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_len = 0;
            return;
        }
        esc_state = 0;
    } else if (esc_state == 2) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            if (c == 'J' && esc_len == 1 && esc_buf[0] == '2') {
                console_clear();
            } else if (c == 'H' && esc_len == 0) {
                cursor_row = 0;
                cursor_col = 0;
                update_hw_cursor();
            }
            esc_state = 0;
            return;
        }
        if (esc_len < (int)sizeof(esc_buf) - 1) {
            esc_buf[esc_len++] = c;
        }
        return;
    }

    if (c == 0x1B) {
        esc_state = 1;
        return;
    }

    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            vga_buf[cursor_row * VGA_COLS + cursor_col] = vga_entry(' ', vga_attr);
        }
    } else if (c == '\t') {
        int next = (cursor_col + 8) & ~7;
        while (cursor_col < next && cursor_col < VGA_COLS) {
            vga_buf[cursor_row * VGA_COLS + cursor_col] = vga_entry(' ', vga_attr);
            cursor_col++;
        }
    } else {
        vga_buf[cursor_row * VGA_COLS + cursor_col] = vga_entry(c, vga_attr);
        cursor_col++;
    }

    if (cursor_col >= VGA_COLS) {
        cursor_col = 0;
        cursor_row++;
    }
    if (cursor_row >= VGA_ROWS) {
        console_scroll();
    }
    update_hw_cursor();
}

void console_puts(const char *s) {
    while (*s) {
        console_putc(*s++);
    }
}
