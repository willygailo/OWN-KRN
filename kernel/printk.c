#include <arch/io.h>
#include <drivers/console.h>
#include <drivers/serial.h>
#include <kernel/printk.h>
#include <lib/printf.h>

int vprintk(const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    uint64_t flags = irq_save();
    serial_puts(buf);
    console_puts(buf);
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
