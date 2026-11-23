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

/* ANSI CSI escape parser: clear (\x1b[2J), home (\x1b[H), SGR colors (\x1b[3xm / 9xm / 0m) */
static int  esc_state = 0; /* 0=normal, 1=saw ESC, 2=in CSI */
static char esc_buf[24];
static int  esc_len = 0;

static const uint8_t ansi_to_vga[8] = {
    0x0, /* black */
    0x4, /* red */
    0x2, /* green */
    0x6, /* yellow/brown */
    0x1, /* blue */
    0x5, /* magenta */
    0x3, /* cyan */
    0x7  /* light grey */
};

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

static void apply_sgr(void) {
    esc_buf[esc_len] = '\0';
    int val = 0;
    bool has_digit = false;
    for (int i = 0; i <= esc_len; i++) {
        char ch = esc_buf[i];
        if (ch >= '0' && ch <= '9') {
            val = val * 10 + (ch - '0');
            has_digit = true;
        } else if (ch == ';' || ch == '\0') {
            if (!has_digit || val == 0) {
                vga_attr = 0x07;
            } else if (val == 1) {
                vga_attr |= 0x08; /* Bright foreground */
            } else if (val == 7) {
                /* Reverse video: swap fg and bg nibbles */
                vga_attr = (uint8_t)(((vga_attr & 0x07) << 4) | ((vga_attr >> 4) & 0x07));
            } else if (val == 27) {
                vga_attr = 0x07;
            } else if (val >= 30 && val <= 37) {
                vga_attr = (uint8_t)((vga_attr & 0xF8) | ansi_to_vga[val - 30]);
            } else if (val >= 90 && val <= 97) {
                vga_attr = (uint8_t)((vga_attr & 0xF0) | ansi_to_vga[val - 90] | 0x08);
            } else if (val >= 40 && val <= 47) {
                vga_attr = (uint8_t)((vga_attr & 0x0F) | (ansi_to_vga[val - 40] << 4));
            } else if (val >= 100 && val <= 107) {
                vga_attr = (uint8_t)((vga_attr & 0x0F) | (ansi_to_vga[val - 100] << 4));
            }
            val = 0;
            has_digit = false;
        }
    }
}

static void parse_csi_params(int *p1, int *p2) {
    esc_buf[esc_len] = '\0';
    int v1 = 0, v2 = 0;
    bool in_second = false;
    for (int i = 0; i < esc_len; i++) {
        char ch = esc_buf[i];
        if (ch == '?') continue;
        if (ch >= '0' && ch <= '9') {
            if (!in_second) v1 = v1 * 10 + (ch - '0');
            else v2 = v2 * 10 + (ch - '0');
        } else if (ch == ';') {
            in_second = true;
        }
    }
    *p1 = v1;
    *p2 = v2;
}

static void console_erase_line(int mode) {
    int start = 0, end = VGA_COLS;
    if (mode == 0) {
        start = cursor_col;
        end = VGA_COLS;
    } else if (mode == 1) {
        start = 0;
        end = cursor_col + 1;
    }
    if (start < 0) start = 0;
    if (end > VGA_COLS) end = VGA_COLS;
    for (int c = start; c < end; c++) {
        vga_buf[cursor_row * VGA_COLS + c] = vga_entry(' ', vga_attr);
    }
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
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
            int p1 = 0, p2 = 0;
            parse_csi_params(&p1, &p2);
            if (c == 'J') {
                if (p1 == 2) {
                    console_clear();
                } else if (p1 == 0) {
                    console_erase_line(0);
                    for (int r = cursor_row + 1; r < VGA_ROWS; r++) {
                        for (int col = 0; col < VGA_COLS; col++) {
                            vga_buf[r * VGA_COLS + col] = vga_entry(' ', vga_attr);
                        }
                    }
                }
            } else if (c == 'K') {
                console_erase_line(p1);
            } else if (c == 'H' || c == 'f') {
                int r = (p1 > 0) ? (p1 - 1) : 0;
                int col = (p2 > 0) ? (p2 - 1) : 0;
                if (r < 0) r = 0;
                if (r >= VGA_ROWS) r = VGA_ROWS - 1;
                if (col < 0) col = 0;
                if (col >= VGA_COLS) col = VGA_COLS - 1;
                cursor_row = r;
                cursor_col = col;
                update_hw_cursor();
            } else if (c == 'A') {
                int n = (p1 > 0) ? p1 : 1;
                cursor_row -= n;
                if (cursor_row < 0) cursor_row = 0;
                update_hw_cursor();
            } else if (c == 'B') {
                int n = (p1 > 0) ? p1 : 1;
                cursor_row += n;
                if (cursor_row >= VGA_ROWS) cursor_row = VGA_ROWS - 1;
                update_hw_cursor();
            } else if (c == 'C') {
                int n = (p1 > 0) ? p1 : 1;
                cursor_col += n;
                if (cursor_col >= VGA_COLS) cursor_col = VGA_COLS - 1;
                update_hw_cursor();
            } else if (c == 'D') {
                int n = (p1 > 0) ? p1 : 1;
                cursor_col -= n;
                if (cursor_col < 0) cursor_col = 0;
                update_hw_cursor();
            } else if (c == 'G') {
                int col = (p1 > 0) ? (p1 - 1) : 0;
                if (col < 0) col = 0;
                if (col >= VGA_COLS) col = VGA_COLS - 1;
                cursor_col = col;
                update_hw_cursor();
            } else if (c == 'm') {
                apply_sgr();
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
