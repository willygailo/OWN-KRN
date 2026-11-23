#include <ulibc.h>

/* ---------------- Syscall wrappers ---------------- */

ssize_t u_read(int fd, void *buf, size_t len) {
    return (ssize_t)u_syscall(SYS_read, (uint64_t)fd, (uint64_t)(uintptr_t)buf, len, 0, 0, 0);
}

ssize_t u_write(int fd, const void *buf, size_t len) {
    return (ssize_t)u_syscall(SYS_write, (uint64_t)fd, (uint64_t)(uintptr_t)buf, len, 0, 0, 0);
}

int u_open(const char *path, uint32_t flags, uint32_t mode) {
    return (int)u_syscall(SYS_open, (uint64_t)(uintptr_t)path, flags, mode, 0, 0, 0);
}

int u_close(int fd) {
    return (int)u_syscall(SYS_close, (uint64_t)fd, 0, 0, 0, 0, 0);
}

int u_stat(const char *path, struct uapi_stat *st) {
    return (int)u_syscall(SYS_stat, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)st, 0, 0, 0, 0);
}

int u_fstat(int fd, struct uapi_stat *st) {
    return (int)u_syscall(SYS_fstat, (uint64_t)fd, (uint64_t)(uintptr_t)st, 0, 0, 0, 0);
}

off_t u_lseek(int fd, off_t off, int whence) {
    return (off_t)u_syscall(SYS_lseek, (uint64_t)fd, (uint64_t)off, (uint64_t)whence, 0, 0, 0);
}

void *u_sbrk(intptr_t inc) {
    uint64_t cur = (uint64_t)u_syscall(SYS_brk, 0, 0, 0, 0, 0, 0);
    if (inc == 0) {
        return (void *)(uintptr_t)cur;
    }
    uint64_t want = (uint64_t)((int64_t)cur + inc);
    uint64_t got = (uint64_t)u_syscall(SYS_brk, want, 0, 0, 0, 0, 0);
    if (got != want) {
        return (void *)-1;
    }
    return (void *)(uintptr_t)cur;
}

int u_pipe(int pipefd[2]) {
    return (int)u_syscall(SYS_pipe, (uint64_t)(uintptr_t)pipefd, 0, 0, 0, 0, 0);
}

int u_dup(int oldfd) {
    return (int)u_syscall(SYS_dup, (uint64_t)oldfd, 0, 0, 0, 0, 0);
}

int u_dup2(int oldfd, int newfd) {
    return (int)u_syscall(SYS_dup2, (uint64_t)oldfd, (uint64_t)newfd, 0, 0, 0, 0);
}

pid_t u_getpid(void) {
    return (pid_t)u_syscall(SYS_getpid, 0, 0, 0, 0, 0, 0);
}

pid_t u_getppid(void) {
    return (pid_t)u_syscall(SYS_getppid, 0, 0, 0, 0, 0, 0);
}

pid_t u_fork(void) {
    return (pid_t)u_syscall(SYS_fork, 0, 0, 0, 0, 0, 0);
}

int u_execve(const char *path, char *const argv[]) {
    return (int)u_syscall(SYS_execve, (uint64_t)(uintptr_t)path,
                          (uint64_t)(uintptr_t)argv, 0, 0, 0, 0);
}

void u_exit(int code) {
    u_syscall(SYS_exit, (uint64_t)code, 0, 0, 0, 0, 0);
    for (;;) {
    }
}

pid_t u_waitpid_opts(pid_t pid, int *status, int options) {
    return (pid_t)u_syscall(SYS_wait4, (uint64_t)(int64_t)pid,
                            (uint64_t)(uintptr_t)status, (uint64_t)options, 0, 0, 0);
}

pid_t u_waitpid(pid_t pid, int *status) {
    return u_waitpid_opts(pid, status, 0);
}

int u_kill(pid_t pid, int sig) {
    return (int)u_syscall(SYS_kill, (uint64_t)(int64_t)pid, (uint64_t)sig, 0, 0, 0, 0);
}

int u_uname(struct utsname *u) {
    return (int)u_syscall(SYS_uname, (uint64_t)(uintptr_t)u, 0, 0, 0, 0, 0);
}

int u_getdents(int fd, struct uapi_dirent *ents, size_t max_ents) {
    return (int)u_syscall(SYS_getdents, (uint64_t)fd,
                          (uint64_t)(uintptr_t)ents, max_ents, 0, 0, 0);
}

int u_getcwd(char *buf, size_t size) {
    return (int)u_syscall(SYS_getcwd, (uint64_t)(uintptr_t)buf, size, 0, 0, 0, 0);
}

int u_chdir(const char *path) {
    return (int)u_syscall(SYS_chdir, (uint64_t)(uintptr_t)path, 0, 0, 0, 0, 0);
}

int u_rename(const char *oldpath, const char *newpath) {
    return (int)u_syscall(SYS_rename, (uint64_t)(uintptr_t)oldpath,
                          (uint64_t)(uintptr_t)newpath, 0, 0, 0, 0);
}

int u_mkdir(const char *path, uint32_t mode) {
    return (int)u_syscall(SYS_mkdir, (uint64_t)(uintptr_t)path, mode, 0, 0, 0, 0);
}

int u_unlink(const char *path) {
    return (int)u_syscall(SYS_unlink, (uint64_t)(uintptr_t)path, 0, 0, 0, 0, 0);
}

int u_msleep(uint64_t ms) {
    struct timespec ts = {
        .tv_sec  = (int64_t)(ms / 1000ULL),
        .tv_nsec = (int64_t)((ms % 1000ULL) * 1000000ULL),
    };
    return (int)u_syscall(SYS_nanosleep, (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0, 0);
}

int u_clock_gettime(int clk_id, struct timespec *ts) {
    return (int)u_syscall(SYS_clock_gettime, (uint64_t)clk_id,
                          (uint64_t)(uintptr_t)ts, 0, 0, 0, 0);
}

int u_ioctl(int fd, uint64_t req, uint64_t arg) {
    return (int)u_syscall(SYS_ioctl, (uint64_t)fd, req, arg, 0, 0, 0);
}

uint32_t u_tty_get_mode(void) {
    uint32_t m = 0;
    u_ioctl(0, OWNKRN_TIOCGETM, (uint64_t)(uintptr_t)&m);
    return m;
}

int u_tty_set_mode(uint32_t mode) {
    return u_ioctl(0, OWNKRN_TIOCSETM, (uint64_t)mode);
}

void u_poweroff(void) {
    u_syscall(SYS_reboot, OWNKRN_CMD_POWEROFF, 0, 0, 0, 0, 0);
}

void u_reboot(void) {
    u_syscall(SYS_reboot, OWNKRN_CMD_REBOOT, 0, 0, 0, 0, 0);
}

/* ---------------- Coalescing brk-backed malloc ---------------- */

struct ublock {
    size_t         size;
    int            free;
    struct ublock *prev;
    struct ublock *next;
};

#define UHDR_SIZE sizeof(struct ublock)
#define UMIN_SPLIT (UHDR_SIZE + 32)

static struct ublock *g_uhead = NULL;
static struct ublock *g_utail = NULL;

void *malloc(size_t size) {
    if (size == 0) size = 16;
    size = (size + 15) & ~15ULL;

    for (struct ublock *b = g_uhead; b != NULL; b = b->next) {
        if (!b->free || b->size < size) continue;

        /* Split block when remainder can hold another allocation */
        if (b->size >= size + UMIN_SPLIT) {
            struct ublock *rem = (struct ublock *)((uint8_t *)b + UHDR_SIZE + size);
            rem->size = b->size - size - UHDR_SIZE;
            rem->free = 1;
            rem->prev = b;
            rem->next = b->next;
            if (b->next) b->next->prev = rem;
            else g_utail = rem;
            b->next = rem;
            b->size = size;
        }
        b->free = 0;
        return (uint8_t *)b + UHDR_SIZE;
    }

    size_t grow = UHDR_SIZE + size;
    if (grow < 4096) grow = 4096;
    grow = (grow + 15) & ~15ULL;

    void *mem = u_sbrk((intptr_t)grow);
    if (mem == (void *)-1) {
        return NULL;
    }

    struct ublock *b = (struct ublock *)mem;
    b->size = grow - UHDR_SIZE;
    b->free = 0;
    b->prev = g_utail;
    b->next = NULL;
    if (g_utail) g_utail->next = b;
    else g_uhead = b;
    g_utail = b;

    if (b->size >= size + UMIN_SPLIT) {
        struct ublock *rem = (struct ublock *)((uint8_t *)b + UHDR_SIZE + size);
        rem->size = b->size - size - UHDR_SIZE;
        rem->free = 1;
        rem->prev = b;
        rem->next = NULL;
        b->next = rem;
        b->size = size;
        g_utail = rem;
    }

    return (uint8_t *)b + UHDR_SIZE;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) return NULL;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void free(void *ptr) {
    if (!ptr) return;
    struct ublock *b = (struct ublock *)((uint8_t *)ptr - UHDR_SIZE);
    b->free = 1;

    /* Coalesce with next contiguous free block */
    if (b->next && b->next->free &&
        ((uint8_t *)b + UHDR_SIZE + b->size == (uint8_t *)b->next)) {
        struct ublock *nxt = b->next;
        b->size += UHDR_SIZE + nxt->size;
        b->next = nxt->next;
        if (b->next) b->next->prev = b;
        else g_utail = b;
    }
    /* Coalesce with previous contiguous free block */
    if (b->prev && b->prev->free &&
        ((uint8_t *)b->prev + UHDR_SIZE + b->prev->size == (uint8_t *)b)) {
        struct ublock *prv = b->prev;
        prv->size += UHDR_SIZE + b->size;
        prv->next = b->next;
        if (prv->next) prv->next->prev = prv;
        else g_utail = prv;
    }
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    struct ublock *b = (struct ublock *)((uint8_t *)ptr - UHDR_SIZE);
    if (b->size >= size) return ptr;
    void *np = malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, b->size);
    free(ptr);
    return np;
}

/* ---------------- String & memory helpers ---------------- */

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    return (n == 0) ? 0 : ((int)(unsigned char)*a - (int)(unsigned char)*b);
}

char *strcpy(char *dst, const char *src) {
    char *r = dst;
    while ((*dst++ = *src++) != '\0') {}
    return r;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strstr(const char *haystack, const char *needle) {
    if (!needle[0]) return (char *)haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0) {
            return (char *)haystack;
        }
    }
    return NULL;
}

int atoi(const char *s) {
    int sign = 1, val = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return sign * val;
}

/* ---------------- Formatted output ---------------- */

struct fmt_buf {
    char  *buf;
    size_t cap;
    size_t pos;
};

static void fb_putc(struct fmt_buf *f, char c) {
    if (f->pos + 1 < f->cap) f->buf[f->pos] = c;
    f->pos++;
}

static void fb_uint(struct fmt_buf *f, uint64_t v, int base, bool neg,
                    int width, bool zero, bool left) {
    static const char digs[] = "0123456789abcdef";
    char tmp[32];
    int len = 0;
    if (v == 0) tmp[len++] = '0';
    else while (v > 0) { tmp[len++] = digs[v % (uint64_t)base]; v /= (uint64_t)base; }
    int total = len + (neg ? 1 : 0);
    int pad = (width > total) ? (width - total) : 0;
    if (!left && !zero) for (int i = 0; i < pad; i++) fb_putc(f, ' ');
    if (neg) fb_putc(f, '-');
    if (!left && zero) for (int i = 0; i < pad; i++) fb_putc(f, '0');
    for (int i = len - 1; i >= 0; i--) fb_putc(f, tmp[i]);
    if (left) for (int i = 0; i < pad; i++) fb_putc(f, ' ');
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    struct fmt_buf f = { buf, cap, 0 };
    while (*fmt) {
        if (*fmt != '%') { fb_putc(&f, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { fb_putc(&f, '%'); fmt++; continue; }
        bool left = false, zero = false;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left = true;
            if (*fmt == '0') zero = true;
            fmt++;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int lmod = 0;
        if (*fmt == 'l') { fmt++; lmod = 1; if (*fmt == 'l') { fmt++; lmod = 2; } }
        else if (*fmt == 'z') { fmt++; lmod = 1; }
        char sp = *fmt ? *fmt++ : '\0';
        switch (sp) {
            case 'c': fb_putc(&f, (char)va_arg(ap, int)); break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int sl = (int)strlen(s);
                int pad = (width > sl) ? (width - sl) : 0;
                if (!left) for (int i = 0; i < pad; i++) fb_putc(&f, ' ');
                while (*s) fb_putc(&f, *s++);
                if (left) for (int i = 0; i < pad; i++) fb_putc(&f, ' ');
                break;
            }
            case 'd': case 'i': {
                int64_t sv = (lmod == 2) ? va_arg(ap, long long) :
                             (lmod == 1) ? va_arg(ap, long) : va_arg(ap, int);
                bool neg = sv < 0;
                uint64_t uv = neg ? (uint64_t)(-(sv + 1)) + 1ULL : (uint64_t)sv;
                fb_uint(&f, uv, 10, neg, width, zero, left);
                break;
            }
            case 'u': case 'x': {
                uint64_t uv = (lmod == 2) ? va_arg(ap, unsigned long long) :
                              (lmod == 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                fb_uint(&f, uv, (sp == 'u') ? 10 : 16, false, width, zero, left);
                break;
            }
            default: fb_putc(&f, '%'); if (sp) fb_putc(&f, sp); break;
        }
    }
    if (cap > 0) buf[(f.pos < cap) ? f.pos : (cap - 1)] = '\0';
    return (int)f.pos;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    if (len > 0) u_write(1, buf, len);
    return n;
}

int puts(const char *s) {
    u_write(1, s, strlen(s));
    u_write(1, "\n", 1);
    return 0;
}

void putchar(char c) {
    u_write(1, &c, 1);
}
