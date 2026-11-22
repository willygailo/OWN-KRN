#include <lib/printf.h>
#include <lib/string.h>
#include <stdbool.h>
#include <stdint.h>

struct out_ctx {
    char  *buf;
    size_t cap;
    size_t pos;
};

static void out_char(struct out_ctx *o, char c) {
    if (o->pos + 1 < o->cap) {
        o->buf[o->pos] = c;
    }
    o->pos++;
}

static void out_pad(struct out_ctx *o, char pad, int count) {
    for (int i = 0; i < count; i++) {
        out_char(o, pad);
    }
}

static void out_uint(struct out_ctx *o, uint64_t val, int base, bool upper,
                     bool is_neg, int width, bool zero_pad, bool left_align,
                     bool alt_hex) {
    static const char lo_digits[] = "0123456789abcdef";
    static const char hi_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? hi_digits : lo_digits;

    char tmp[64];
    int len = 0;

    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0) {
            tmp[len++] = digits[val % (uint64_t)base];
            val /= (uint64_t)base;
        }
    }

    int prefix_len = (is_neg ? 1 : 0) + (alt_hex ? 2 : 0);
    int total = len + prefix_len;
    int pad = (width > total) ? (width - total) : 0;

    if (!left_align && !zero_pad) {
        out_pad(o, ' ', pad);
    }
    if (is_neg) {
        out_char(o, '-');
    }
    if (alt_hex) {
        out_char(o, '0');
        out_char(o, upper ? 'X' : 'x');
    }
    if (!left_align && zero_pad) {
        out_pad(o, '0', pad);
    }
    for (int i = len - 1; i >= 0; i--) {
        out_char(o, tmp[i]);
    }
    if (left_align) {
        out_pad(o, ' ', pad);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct out_ctx o = { .buf = buf, .cap = size, .pos = 0 };

    while (*fmt) {
        if (*fmt != '%') {
            out_char(&o, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out_char(&o, '%');
            fmt++;
            continue;
        }

        bool left_align = false;
        bool zero_pad = false;
        bool alt_form = false;
        while (*fmt == '-' || *fmt == '0' || *fmt == '#') {
            if (*fmt == '-') left_align = true;
            if (*fmt == '0') zero_pad = true;
            if (*fmt == '#') alt_form = true;
            fmt++;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                prec = prec * 10 + (*fmt - '0');
                fmt++;
            }
        }

        int len_mod = 0; /* 0=int, 1=long, 2=long long, 3=size_t */
        if (*fmt == 'l') {
            fmt++;
            len_mod = 1;
            if (*fmt == 'l') {
                fmt++;
                len_mod = 2;
            }
        } else if (*fmt == 'z') {
            fmt++;
            len_mod = 3;
        }

        char spec = *fmt ? *fmt++ : '\0';
        switch (spec) {
            case 'c': {
                char c = (char)va_arg(ap, int);
                if (!left_align && width > 1) out_pad(&o, ' ', width - 1);
                out_char(&o, c);
                if (left_align && width > 1) out_pad(&o, ' ', width - 1);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                size_t slen = (prec >= 0) ? strnlen(s, (size_t)prec) : strlen(s);
                int pad = (width > (int)slen) ? (width - (int)slen) : 0;
                if (!left_align) out_pad(&o, ' ', pad);
                for (size_t i = 0; i < slen; i++) out_char(&o, s[i]);
                if (left_align) out_pad(&o, ' ', pad);
                break;
            }
            case 'd':
            case 'i': {
                int64_t sv;
                if (len_mod == 2) sv = va_arg(ap, long long);
                else if (len_mod == 1) sv = va_arg(ap, long);
                else if (len_mod == 3) sv = (int64_t)va_arg(ap, ptrdiff_t);
                else sv = va_arg(ap, int);

                bool neg = (sv < 0);
                uint64_t uv = neg ? (uint64_t)(-(sv + 1)) + 1ULL : (uint64_t)sv;
                out_uint(&o, uv, 10, false, neg, width, zero_pad, left_align, false);
                break;
            }
            case 'u':
            case 'x':
            case 'X': {
                uint64_t uv;
                if (len_mod == 2) uv = va_arg(ap, unsigned long long);
                else if (len_mod == 1) uv = va_arg(ap, unsigned long);
                else if (len_mod == 3) uv = (uint64_t)va_arg(ap, size_t);
                else uv = va_arg(ap, unsigned int);

                int base = (spec == 'u') ? 10 : 16;
                bool upper = (spec == 'X');
                out_uint(&o, uv, base, upper, false, width, zero_pad, left_align,
                         alt_form && base == 16 && uv != 0);
                break;
            }
            case 'p': {
                uintptr_t pv = (uintptr_t)va_arg(ap, void *);
                out_uint(&o, (uint64_t)pv, 16, false, false, width, zero_pad,
                         left_align, true);
                break;
            }
            default:
                out_char(&o, '%');
                if (spec) out_char(&o, spec);
                break;
        }
    }

    if (o.cap > 0) {
        size_t term = (o.pos < o.cap) ? o.pos : (o.cap - 1);
        o.buf[term] = '\0';
    }
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}
