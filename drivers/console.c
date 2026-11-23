#include <arch/io.h>
#include <arch/pit.h>
#include <drivers/console.h>
#include <drivers/font8x16.h>
#include <drivers/rtc.h>
#include <kernel/types.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <mm/pmm.h>

/* Legacy VGA Text Mode (fallback when VBE LFB is unavailable) */
#define VGA_PHYS_ADDR 0xB8000ULL
#define VGA_TEXT_COLS 80
#define VGA_TEXT_ROWS 25

/* Bochs VBE 16:9 Widescreen Linear Framebuffer (1024x576 @ 32bpp -> 128x36 cells) */
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF
#define VBE_DISPI_INDEX_ID     0x00
#define VBE_DISPI_INDEX_XRES   0x01
#define VBE_DISPI_INDEX_YRES   0x02
#define VBE_DISPI_INDEX_BPP    0x03
#define VBE_DISPI_INDEX_ENABLE 0x04
#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40

#define FB_WIDTH   1024
#define FB_HEIGHT  576
#define FONT_W     8
#define FONT_H     16
#define FB_COLS    (FB_WIDTH / FONT_W)   /* 128 columns */
#define FB_ROWS    (FB_HEIGHT / FONT_H)  /* 36 total rows */
#define TERM_TOP   1                     /* Row 0 is persistent Top HUD bar */
#define TERM_ROWS  (FB_ROWS - 2)         /* Rows 1..34 (34 rows) are terminal viewport */
#define TERM_COLS  FB_COLS               /* 128 columns */

static volatile uint16_t *vga_buf = NULL;
static volatile uint32_t *fb_buf  = NULL;
static bool               g_use_fb = false;
static int                g_cols = TERM_COLS;
static int                g_rows = TERM_ROWS;

static int     cursor_row = 0;
static int     cursor_col = 0;
static uint8_t vga_attr   = 0x07; /* Light grey on dark slate */

/* Shadow RAM cell grids for fast differential framebuffer blitting */
static uint16_t g_shadow[TERM_ROWS][TERM_COLS];
static uint16_t g_drawn[TERM_ROWS][TERM_COLS];
static int      g_drawn_cur_r = 0;
static int      g_drawn_cur_c = 0;

/* ANSI CSI escape parser */
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

/* Modern GitHub-Dark inspired 16-color TrueColor palette (0x00RRGGBB) */
static const uint32_t g_fb_palette[16] = {
    0x000D1117, /* 0: Dark Slate Background */
    0x002F81F7, /* 1: Blue */
    0x00238636, /* 2: Green */
    0x0039C5BB, /* 3: Cyan */
    0x00DA3633, /* 4: Red */
    0x00A371F7, /* 5: Magenta */
    0x00D29922, /* 6: Amber / Brown */
    0x00E6EDF3, /* 7: Crisp Off-White Foreground */
    0x006E7681, /* 8: Muted Grey */
    0x0058A6FF, /* 9: Bright Blue */
    0x003FB950, /* 10: Bright Green */
    0x0056D4DD, /* 11: Bright Cyan */
    0x00FF7B72, /* 12: Bright Red */
    0x00D2A8FF, /* 13: Bright Magenta */
    0x00E3B341, /* 14: Bright Yellow */
    0x00FFFFFF  /* 15: Pure White */
};

static uint16_t vga_entry(char c, uint8_t attr) {
    return (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

/* ---------------- Bochs VBE & PCI BAR0 Discovery ---------------- */

static void vbe_write(uint16_t idx, uint16_t val) {
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    outw(VBE_DISPI_IOPORT_DATA, val);
}

static uint16_t vbe_read(uint16_t idx) {
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFCU);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFCU);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

static uint64_t detect_vbe_lfb_phys(void) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id = pci_read32(0, slot, 0, 0x00);
        uint16_t vendor = (uint16_t)(id & 0xFFFFU);
        uint16_t device = (uint16_t)((id >> 16) & 0xFFFFU);
        if (vendor == 0xFFFFU) continue;

        if ((vendor == 0x1234U && device == 0x1111U) ||
            (vendor == 0x80EEU && device == 0xBEEFU)) {
            uint32_t cmd = pci_read32(0, slot, 0, 0x04);
            pci_write32(0, slot, 0, 0x04, cmd | 0x03U);
            uint32_t bar0 = pci_read32(0, slot, 0, 0x10) & 0xFFFFFFF0U;
            if (bar0 != 0) {
                return (uint64_t)bar0;
            }
        }
    }
    return 0xFD000000ULL;
}

static void fb_draw_glyph_raw(int screen_row, int screen_col, char ch,
                              uint32_t fg, uint32_t bg, bool draw_cursor) {
    if (!fb_buf) return;
    uint8_t uc = (uint8_t)ch;
    const uint8_t *glyph = (uc >= 0x20 && uc <= 0x7E)
                           ? g_font8x16[uc - 0x20]
                           : g_font8x16[0];

    size_t base_y = (size_t)screen_row * FONT_H;
    size_t base_x = (size_t)screen_col * FONT_W;

    for (size_t y = 0; y < FONT_H; y++) {
        uint8_t bits = glyph[y];
        volatile uint32_t *dst = fb_buf + (base_y + y) * FB_WIDTH + base_x;
        if (draw_cursor && (y == 13 || y == 14)) {
            for (size_t x = 0; x < FONT_W; x++) {
                dst[x] = 0x003FB950U;
            }
        } else {
            dst[0] = (bits & 0x80) ? fg : bg;
            dst[1] = (bits & 0x40) ? fg : bg;
            dst[2] = (bits & 0x20) ? fg : bg;
            dst[3] = (bits & 0x10) ? fg : bg;
            dst[4] = (bits & 0x08) ? fg : bg;
            dst[5] = (bits & 0x04) ? fg : bg;
            dst[6] = (bits & 0x02) ? fg : bg;
            dst[7] = (bits & 0x01) ? fg : bg;
        }
    }
}

static void fb_render_cell(int r, int c) {
    if (!g_use_fb || r < 0 || r >= TERM_ROWS || c < 0 || c >= TERM_COLS) return;
    uint16_t entry = g_shadow[r][c];
    char ch = (char)(entry & 0xFF);
    uint8_t attr = (uint8_t)((entry >> 8) & 0xFF);
    uint32_t fg = g_fb_palette[attr & 0x0F];
    uint32_t bg = g_fb_palette[(attr >> 4) & 0x0F];
    bool is_cur = (r == cursor_row && c == cursor_col);
    fb_draw_glyph_raw(TERM_TOP + r, c, ch, fg, bg, is_cur);
    g_drawn[r][c] = entry;
}

static void fb_draw_bar_str(int screen_row, int start_col, const char *s,
                            uint32_t fg, uint32_t bg) {
    for (int i = 0; s[i] && (start_col + i) < FB_COLS; i++) {
        fb_draw_glyph_raw(screen_row, start_col + i, s[i], fg, bg, false);
    }
}

static void fb_draw_top_hud(void) {
    if (!g_use_fb) return;
    uint32_t bar_bg  = 0x00161B22;
    uint32_t pill_bg = 0x001F6FEB;
    uint32_t ok_bg   = 0x00238636;
    uint32_t text_fg = 0x00E6EDF3;
    uint32_t cyan_fg = 0x0056D4DD;
    uint32_t yell_fg = 0x00E3B341;

    for (int c = 0; c < FB_COLS; c++) {
        fb_draw_glyph_raw(0, c, ' ', text_fg, bar_bg, false);
    }

    uint64_t up_sec = pit_ticks() / PIT_HZ;
    struct pmm_stats ps;
    pmm_get_stats(&ps);
    uint64_t used_mb = (ps.total_bytes - ps.free_bytes) / (1024ULL * 1024ULL);
    uint64_t tot_mb  = ps.total_bytes / (1024ULL * 1024ULL);

    struct rtc_time rt;
    rtc_read(&rt);

    char telemetry[96];
    snprintf(telemetry, sizeof(telemetry),
             "| Up: %llus | RAM: %llu/%llu MiB | UTC %02u:%02u:%02u | 1024x576x32",
             (unsigned long long)up_sec,
             (unsigned long long)used_mb,
             (unsigned long long)tot_mb,
             rt.hour, rt.minute, rt.second);

    fb_draw_bar_str(0, 0,  " OWNKRN 0.3.0-x86_64 ", 0x00FFFFFF, pill_bg);
    fb_draw_bar_str(0, 22, "CPU: Long Mode (NX+WP+SMEP)", cyan_fg, bar_bg);
    fb_draw_bar_str(0, 50, telemetry, yell_fg, bar_bg);
    fb_draw_bar_str(0, 115, " RING 0/3 OK ", 0x00FFFFFF, ok_bg);
}

static void fb_draw_bottom_dock(void) {
    if (!g_use_fb) return;
    uint32_t bar_bg  = 0x00161B22;
    uint32_t ok_bg   = 0x00238636;
    uint32_t text_fg = 0x00E6EDF3;
    uint32_t cyan_fg = 0x0056D4DD;
    uint32_t yell_fg = 0x00E3B341;

    for (int c = 0; c < FB_COLS; c++) {
        fb_draw_glyph_raw(FB_ROWS - 1, c, ' ', text_fg, bar_bg, false);
    }

    fb_draw_bar_str(FB_ROWS - 1, 0,  " /bin/sh ", 0x00FFFFFF, ok_bg);
    fb_draw_bar_str(FB_ROWS - 1, 10, "Apps:", yell_fg, bar_bg);
    fb_draw_bar_str(FB_ROWS - 1, 16, "fetch  top  fm (files)  edit (nano)  snake  calc  cal  df", cyan_fg, bar_bg);
    fb_draw_bar_str(FB_ROWS - 1, 75, "| Keys: [Up/Down] Hist [Tab] Auto [Ctrl+L] Clr", text_fg, bar_bg);
}

static void fb_draw_hud_bars(void) {
    if (!g_use_fb) return;
    fb_draw_top_hud();
    fb_draw_bottom_dock();
}

void console_tick_hud(uint64_t ticks) {
    (void)ticks;
    if (!g_use_fb) return;
    uint64_t flags = irq_save();
    fb_draw_top_hud();
    irq_restore(flags);
}

static void update_hw_cursor(void) {
    if (g_use_fb) {
        int old_r = g_drawn_cur_r;
        int old_c = g_drawn_cur_c;
        g_drawn_cur_r = cursor_row;
        g_drawn_cur_c = cursor_col;
        if (old_r != cursor_row || old_c != cursor_col) {
            fb_render_cell(old_r, old_c);
        }
        fb_render_cell(cursor_row, cursor_col);
        return;
    }

    uint16_t pos = (uint16_t)(cursor_row * g_cols + cursor_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void put_cell(int r, int c, char ch, uint8_t attr) {
    if (r < 0 || r >= g_rows || c < 0 || c >= g_cols) return;
    uint16_t entry = vga_entry(ch, attr);
    if (g_use_fb) {
        g_shadow[r][c] = entry;
        if (g_drawn[r][c] != entry || (r == cursor_row && c == cursor_col)) {
            fb_render_cell(r, c);
        }
    } else if (vga_buf) {
        vga_buf[r * g_cols + c] = entry;
    }
}

static void console_scroll(void) {
    if (g_use_fb) {
        for (int r = 1; r < g_rows; r++) {
            for (int c = 0; c < g_cols; c++) {
                g_shadow[r - 1][c] = g_shadow[r][c];
            }
        }
        uint16_t blank = vga_entry(' ', vga_attr);
        for (int c = 0; c < g_cols; c++) {
            g_shadow[g_rows - 1][c] = blank;
        }
        cursor_row = g_rows - 1;

        for (int r = 0; r < g_rows; r++) {
            for (int c = 0; c < g_cols; c++) {
                if (g_shadow[r][c] != g_drawn[r][c]) {
                    fb_render_cell(r, c);
                }
            }
        }
        return;
    }

    for (int r = 1; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            vga_buf[(r - 1) * g_cols + c] = vga_buf[r * g_cols + c];
        }
    }
    for (int c = 0; c < g_cols; c++) {
        vga_buf[(g_rows - 1) * g_cols + c] = vga_entry(' ', vga_attr);
    }
    cursor_row = g_rows - 1;
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
                vga_attr |= 0x08;
            } else if (val == 7) {
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
    int start = 0, end = g_cols;
    if (mode == 0) {
        start = cursor_col;
        end = g_cols;
    } else if (mode == 1) {
        start = 0;
        end = cursor_col + 1;
    }
    if (start < 0) start = 0;
    if (end > g_cols) end = g_cols;
    for (int c = start; c < end; c++) {
        put_cell(cursor_row, c, ' ', vga_attr);
    }
}

void console_init(void) {
    vga_buf = (volatile uint16_t *)PHYS_TO_VIRT(VGA_PHYS_ADDR);
    vga_attr = 0x07;
    esc_state = 0;

    uint16_t vbe_id = vbe_read(VBE_DISPI_INDEX_ID);
    if ((vbe_id & 0xFFF0U) == 0xB0C0U) {
        uint64_t lfb_phys = detect_vbe_lfb_phys();
        vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        vbe_write(VBE_DISPI_INDEX_XRES, FB_WIDTH);
        vbe_write(VBE_DISPI_INDEX_YRES, FB_HEIGHT);
        vbe_write(VBE_DISPI_INDEX_BPP, 32);
        vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

        fb_buf = (volatile uint32_t *)PHYS_TO_VIRT(lfb_phys);
        g_use_fb = true;
        g_cols = TERM_COLS;
        g_rows = TERM_ROWS;
        memset(g_drawn, 0xFF, sizeof(g_drawn));
        fb_draw_hud_bars();
    } else {
        g_use_fb = false;
        g_cols = VGA_TEXT_COLS;
        g_rows = VGA_TEXT_ROWS;
    }

    console_clear();
}

void console_set_color(uint8_t fg, uint8_t bg) {
    vga_attr = (uint8_t)((fg & 0x0F) | ((bg & 0x0F) << 4));
}

void console_clear(void) {
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            put_cell(r, c, ' ', vga_attr);
        }
    }
    if (g_use_fb) {
        fb_draw_hud_bars();
    }
    cursor_row = 0;
    cursor_col = 0;
    update_hw_cursor();
}

void console_putc(char c) {
    if (!vga_buf && !fb_buf) {
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
                    for (int r = cursor_row + 1; r < g_rows; r++) {
                        for (int col = 0; col < g_cols; col++) {
                            put_cell(r, col, ' ', vga_attr);
                        }
                    }
                }
            } else if (c == 'K') {
                console_erase_line(p1);
            } else if (c == 'H' || c == 'f') {
                int r = (p1 > 0) ? (p1 - 1) : 0;
                int col = (p2 > 0) ? (p2 - 1) : 0;
                if (r < 0) r = 0;
                if (r >= g_rows) r = g_rows - 1;
                if (col < 0) col = 0;
                if (col >= g_cols) col = g_cols - 1;
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
                if (cursor_row >= g_rows) cursor_row = g_rows - 1;
                update_hw_cursor();
            } else if (c == 'C') {
                int n = (p1 > 0) ? p1 : 1;
                cursor_col += n;
                if (cursor_col >= g_cols) cursor_col = g_cols - 1;
                update_hw_cursor();
            } else if (c == 'D') {
                int n = (p1 > 0) ? p1 : 1;
                cursor_col -= n;
                if (cursor_col < 0) cursor_col = 0;
                update_hw_cursor();
            } else if (c == 'G') {
                int col = (p1 > 0) ? (p1 - 1) : 0;
                if (col < 0) col = 0;
                if (col >= g_cols) col = g_cols - 1;
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
            put_cell(cursor_row, cursor_col, ' ', vga_attr);
        }
    } else if (c == '\t') {
        int next = (cursor_col + 8) & ~7;
        while (cursor_col < next && cursor_col < g_cols) {
            put_cell(cursor_row, cursor_col, ' ', vga_attr);
            cursor_col++;
        }
    } else {
        put_cell(cursor_row, cursor_col, c, vga_attr);
        cursor_col++;
    }

    if (cursor_col >= g_cols) {
        cursor_col = 0;
        cursor_row++;
    }
    if (cursor_row >= g_rows) {
        console_scroll();
    }
    update_hw_cursor();
}

void console_puts(const char *s) {
    while (*s) {
        console_putc(*s++);
    }
}
