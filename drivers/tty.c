#include <arch/io.h>
#include <drivers/console.h>
#include <drivers/keyboard.h>
#include <drivers/serial.h>
#include <drivers/tty.h>
#include <kernel/errno.h>
#include <kernel/sched.h>
#include <kernel/sync.h>
#include <uapi/syscall.h>

#define TTY_LINE_MAX  1024
#define TTY_READY_MAX 16384

static char       g_line_buf[TTY_LINE_MAX];
static size_t     g_line_len = 0;

static char       g_ready_buf[TTY_READY_MAX];
static size_t     g_ready_head = 0;
static size_t     g_ready_tail = 0;
static bool       g_eof_pending = false;
static int        g_rx_esc = 0; /* 0=normal, 1=ESC, 2=CSI */
static uint32_t   g_tty_mode = TTY_MODE_CANON;
static spinlock_t g_tty_lock = SPINLOCK_INIT;

static void tty_echo(char c) {
    serial_putc(c);
    console_putc(c);
}

static void ready_push_unlocked(char c) {
    size_t next = (g_ready_head + 1) % TTY_READY_MAX;
    if (next != g_ready_tail) {
        g_ready_buf[g_ready_head] = c;
        g_ready_head = next;
    }
}

void tty_set_mode(uint32_t mode) {
    uint64_t flags = spin_lock_irqsave(&g_tty_lock);
    if ((mode & TTY_MODE_RAW) && g_line_len > 0) {
        for (size_t i = 0; i < g_line_len; i++) {
            ready_push_unlocked(g_line_buf[i]);
        }
        g_line_len = 0;
    }
    g_rx_esc = 0;
    g_tty_mode = mode & (TTY_MODE_RAW | TTY_MODE_NONBLOCK);
    spin_unlock_irqrestore(&g_tty_lock, flags);
}

uint32_t tty_get_mode(void) {
    uint64_t flags = spin_lock_irqsave(&g_tty_lock);
    uint32_t m = g_tty_mode;
    spin_unlock_irqrestore(&g_tty_lock, flags);
    return m;
}

void tty_rx_char(char c) {
    uint64_t flags = spin_lock_irqsave(&g_tty_lock);

    if (g_tty_mode & TTY_MODE_RAW) {
        if (c == '\r') {
            c = '\n';
        }
        ready_push_unlocked(c);
        spin_unlock_irqrestore(&g_tty_lock, flags);
        return;
    }

    /* Ignore ANSI terminal escape sequences (arrow keys, Home/End, etc.) */
    if (g_rx_esc == 1) {
        if (c == '[' || c == 'O') {
            g_rx_esc = 2;
        } else {
            g_rx_esc = 0;
        }
        spin_unlock_irqrestore(&g_tty_lock, flags);
        return;
    }
    if (g_rx_esc == 2) {
        if ((c >= '@' && c <= '~')) {
            g_rx_esc = 0;
        }
        spin_unlock_irqrestore(&g_tty_lock, flags);
        return;
    }
    if (c == 0x1B) {
        g_rx_esc = 1;
        spin_unlock_irqrestore(&g_tty_lock, flags);
        return;
    }

    if (c == '\r') {
        c = '\n';
    }

    if (c == '\b' || c == 0x7F) {
        if (g_line_len > 0) {
            g_line_len--;
            tty_echo('\b');
            tty_echo(' ');
            tty_echo('\b');
        }
        spin_unlock_irqrestore(&g_tty_lock, flags);
        return;
    }

    if (c == 0x03) { /* Ctrl-C: send SIGINT to foreground task & clear line */
        g_line_len = 0;
        tty_echo('^');
        tty_echo('C');
        tty_echo('\n');
        spin_unlock_irqrestore(&g_tty_lock, flags);
        sched_signal_foreground(SIGINT);
        return;
    }

    if (c == 0x04) { /* Ctrl-D: flush current line or signal EOF */
        if (g_line_len > 0) {
            for (size_t i = 0; i < g_line_len; i++) {
                ready_push_unlocked(g_line_buf[i]);
            }
            g_line_len = 0;
        } else {
            g_eof_pending = true;
        }
        spin_unlock_irqrestore(&g_tty_lock, flags);
        return;
    }

    if (c == '\n') {
        tty_echo('\n');
        for (size_t i = 0; i < g_line_len; i++) {
            ready_push_unlocked(g_line_buf[i]);
        }
        ready_push_unlocked('\n');
        g_line_len = 0;
        spin_unlock_irqrestore(&g_tty_lock, flags);
        return;
    }

    if ((unsigned char)c >= 0x20 || c == '\t') {
        if (g_line_len + 1 < TTY_LINE_MAX) {
            g_line_buf[g_line_len++] = c;
            tty_echo(c);
        }
    }

    spin_unlock_irqrestore(&g_tty_lock, flags);
}

void tty_init(void) {
    g_line_len = 0;
    g_ready_head = 0;
    g_ready_tail = 0;
    g_eof_pending = false;
    g_rx_esc = 0;
    g_tty_mode = TTY_MODE_CANON;
    keyboard_set_sink(tty_rx_char);
}

ssize_t tty_read(void *buf, size_t len) {
    if (len == 0) {
        return 0;
    }
    char *dst = (char *)buf;
    size_t n = 0;

    for (;;) {
        struct task *cur = sched_current();
        if (cur && cur->pending_sigs != 0) {
            return -EINTR;
        }

        uint64_t flags = spin_lock_irqsave(&g_tty_lock);
        bool raw = (g_tty_mode & TTY_MODE_RAW) != 0;
        bool nonblock = (g_tty_mode & TTY_MODE_NONBLOCK) != 0;

        while (n < len && g_ready_tail != g_ready_head) {
            char c = g_ready_buf[g_ready_tail];
            g_ready_tail = (g_ready_tail + 1) % TTY_READY_MAX;
            dst[n++] = c;
            if (!raw && c == '\n') {
                spin_unlock_irqrestore(&g_tty_lock, flags);
                return (ssize_t)n;
            }
        }
        if (n > 0) {
            spin_unlock_irqrestore(&g_tty_lock, flags);
            return (ssize_t)n;
        }
        if (g_eof_pending) {
            g_eof_pending = false;
            spin_unlock_irqrestore(&g_tty_lock, flags);
            return 0; /* EOF */
        }
        if (raw && nonblock) {
            spin_unlock_irqrestore(&g_tty_lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&g_tty_lock, flags);

        irq_enable();
        cpu_halt();
        sched_yield();
    }
}

ssize_t tty_write(const void *buf, size_t len) {
    const char *src = (const char *)buf;
    for (size_t i = 0; i < len; i++) {
        keyboard_poll_serial();
        serial_putc(src[i]);
        uint64_t flags = irq_save();
        console_putc(src[i]);
        irq_restore(flags);
    }
    return (ssize_t)len;
}
