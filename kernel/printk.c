#include <arch/io.h>
#include <arch/pit.h>
#include <drivers/console.h>
#include <drivers/serial.h>
#include <kernel/printk.h>
#include <lib/printf.h>
#include <lib/string.h>

#define KMSG_BUF_SIZE 16384

static char   g_kmsg_buf[KMSG_BUF_SIZE];
static size_t g_kmsg_len = 0;

static void kmsg_append_unlocked(const char *s) {
    size_t slen = strlen(s);
    if (slen == 0) return;

    if (g_kmsg_len + slen >= KMSG_BUF_SIZE) {
        size_t drop = (g_kmsg_len + slen + 1) - KMSG_BUF_SIZE;
        if (drop < g_kmsg_len) {
            memmove(g_kmsg_buf, g_kmsg_buf + drop, g_kmsg_len - drop);
            g_kmsg_len -= drop;
        } else {
            g_kmsg_len = 0;
        }
    }
    if (g_kmsg_len + slen < KMSG_BUF_SIZE) {
        memcpy(g_kmsg_buf + g_kmsg_len, s, slen);
        g_kmsg_len += slen;
        g_kmsg_buf[g_kmsg_len] = '\0';
    }
}

int vprintk(const char *fmt, va_list ap) {
    char raw[960];
    int n = vsnprintf(raw, sizeof(raw), fmt, ap);

    char out[1024];
    /* Prepend Linux-style [    0.000000] monotonic timestamp on tagged kernel lines */
    if (raw[0] == '[') {
        uint64_t ms = pit_uptime_ms();
        uint64_t sec = ms / 1000ULL;
        uint64_t usec = (ms % 1000ULL) * 1000ULL;
        snprintf(out, sizeof(out), "[%5llu.%06llu] %s",
                 (unsigned long long)sec,
                 (unsigned long long)usec,
                 raw);
    } else {
        strncpy(out, raw, sizeof(out) - 1);
        out[sizeof(out) - 1] = '\0';
    }

    uint64_t flags = irq_save();
    kmsg_append_unlocked(out);
    serial_puts(out);
    console_puts(out);
    irq_restore(flags);
    return n;
}

int printk(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintk(fmt, ap);
    va_end(ap);
    return n;
}

size_t printk_read_kmsg(char *dst, size_t cap) {
    if (!dst || cap == 0) return 0;
    uint64_t flags = irq_save();
    size_t len = g_kmsg_len;
    if (len >= cap) len = cap - 1;
    memcpy(dst, g_kmsg_buf, len);
    dst[len] = '\0';
    irq_restore(flags);
    return len;
}

void printk_clear_kmsg(void) {
    uint64_t flags = irq_save();
    g_kmsg_len = 0;
    g_kmsg_buf[0] = '\0';
    irq_restore(flags);
}
