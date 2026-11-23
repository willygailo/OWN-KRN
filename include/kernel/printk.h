#ifndef OWNKRN_KERNEL_PRINTK_H
#define OWNKRN_KERNEL_PRINTK_H

#include <stdarg.h>
#include <stddef.h>

int    vprintk(const char *fmt, va_list ap);
int    printk(const char *fmt, ...);
size_t printk_read_kmsg(char *dst, size_t cap);
void   printk_clear_kmsg(void);

#define pr_info(fmt, ...) printk("[INFO] " fmt, ##__VA_ARGS__)
#define pr_ok(fmt, ...)   printk("[ OK ] " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk("[WARN] " fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)  printk("[ERR ] " fmt, ##__VA_ARGS__)

#endif /* OWNKRN_KERNEL_PRINTK_H */
