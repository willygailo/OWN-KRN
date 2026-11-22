#ifndef OWNKRN_KERNEL_PRINTK_H
#define OWNKRN_KERNEL_PRINTK_H

#include <stdarg.h>

int vprintk(const char *fmt, va_list ap);
int printk(const char *fmt, ...);

#define pr_info(fmt, ...) printk("[INFO] " fmt, ##__VA_ARGS__)
#define pr_ok(fmt, ...)   printk("[ OK ] " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk("[WARN] " fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)  printk("[ERR ] " fmt, ##__VA_ARGS__)

#endif /* OWNKRN_KERNEL_PRINTK_H */
