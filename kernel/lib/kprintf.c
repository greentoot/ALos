/* kernel/lib/kprintf.c */
#include "kprintf.h"
#include "string.h"
/* tty_write est déclaré ici directement pour éviter les includes circulaires */
extern void tty_write(const char *str);

static int _vsprintf(char *buf, const char *fmt, __builtin_va_list ap) {
    char *out = buf;
    while (*fmt) {
        if (*fmt != '%') { *out++ = *fmt++; continue; }
        fmt++;
        int zero_pad = 0, width = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt-'0'); fmt++; }
        switch (*fmt++) {
            case 'd': {
                int v = __builtin_va_arg(ap, int);
                char tmp[16]; kitoa(v, tmp);
                int l = (int)kstrlen(tmp);
                char pad = zero_pad ? '0' : ' ';
                while (l++ < width) *out++ = pad;
                char *p = tmp; while (*p) *out++ = *p++;
                break;
            }
            case 'u': {
                uint32_t v = __builtin_va_arg(ap, uint32_t);
                char tmp[16]; kuitoa(v, tmp, 10);
                int l = (int)kstrlen(tmp);
                char pad = zero_pad ? '0' : ' ';
                while (l++ < width) *out++ = pad;
                char *p = tmp; while (*p) *out++ = *p++;
                break;
            }
            case 'x': case 'X': {
                uint32_t v = __builtin_va_arg(ap, uint32_t);
                char tmp[16]; kuitoa(v, tmp, 16);
                int l = (int)kstrlen(tmp);
                char pad = zero_pad ? '0' : ' ';
                while (l++ < width) *out++ = pad;
                char *p = tmp; while (*p) *out++ = *p++;
                break;
            }
            case 's': {
                const char *s = __builtin_va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s) *out++ = *s++;
                break;
            }
            case 'c': *out++ = (char)__builtin_va_arg(ap, int); break;
            case '%': *out++ = '%'; break;
            default:  *out++ = '?'; break;
        }
    }
    *out = '\0';
    return (int)(out - buf);
}

void kprintf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    char buf[512];
    _vsprintf(buf, fmt, ap);
    __builtin_va_end(ap);
    tty_write(buf);
}

int ksprintf(char *buf, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = _vsprintf(buf, fmt, ap);
    __builtin_va_end(ap);
    return n;
}
