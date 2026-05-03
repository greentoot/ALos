#include "string.h"
#include "kprintf.h"
#include "../memory/heap.h"
#include "../../driver/timer.h"

#include <stdint.h>
#include <stddef.h>

typedef unsigned int mode_t;
typedef unsigned int locale_t;
typedef long time_t;
typedef long suseconds_t;
typedef unsigned int useconds_t;
typedef __builtin_va_list va_list;

typedef struct { int quot; int rem; } div_t;

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

typedef struct FILE_ {
    int _dummy;
} FILE;

FILE *stdout = (FILE*)0;
FILE *stderr = (FILE*)0;
void *_GLOBAL_OFFSET_TABLE_;
char __libc_single_threaded = 1;

static int g_errno = 0;
static const time_t ALOS_RTC_EPOCH = (time_t)946684800; /* 2000-01-01 00:00:00 UTC */

int *__errno_location(void) {
    return &g_errno;
}

unsigned short **__ctype_b_loc(void) {
    static unsigned short table[384];
    static unsigned short *ptr = table + 128;
    static int init = 0;
    if (!init) {
        for (int c = '0'; c <= '9'; c++) ptr[c] |= 0x0400;
        for (int c = 'A'; c <= 'Z'; c++) ptr[c] |= 0x0100;
        for (int c = 'a'; c <= 'z'; c++) ptr[c] |= 0x0100;
        ptr[' '] |= 0x2000;
        ptr['\t'] |= 0x2000;
        ptr['\n'] |= 0x2000;
        ptr['\r'] |= 0x2000;
        init = 1;
    }
    return &ptr;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int char_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc = 0;
    int neg = 0;
    int any = 0;

    while (is_space(*s)) s++;
    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
        s++;
    } else if (base == 0) {
        base = 10;
    }

    for (;;) {
        int d = char_digit(*s);
        if (d < 0 || d >= base) break;
        acc = acc * (unsigned long)base + (unsigned long)d;
        s++;
        any = 1;
    }

    if (endptr) *endptr = (char*)(any ? s : nptr);
    return neg ? -(long)acc : (long)acc;
}

unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)__isoc23_strtol(nptr, endptr, base);
}

int abs(int x) {
    return (x < 0) ? -x : x;
}

div_t div(int numer, int denom) {
    div_t r;
    if (denom == 0) {
        r.quot = 0;
        r.rem = 0;
    } else {
        r.quot = numer / denom;
        r.rem = numer % denom;
    }
    return r;
}

static unsigned long long udiv64(unsigned long long n, unsigned long long d) {
    unsigned long long q = 0;
    unsigned long long r = 0;
    if (!d) return 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ull);
        if (r >= d) {
            r -= d;
            q |= (1ull << i);
        }
    }
    return q;
}

long long __divdi3(long long n, long long d) {
    int neg = ((n < 0) ^ (d < 0));
    unsigned long long un = (n < 0) ? (unsigned long long)(-n) : (unsigned long long)n;
    unsigned long long ud = (d < 0) ? (unsigned long long)(-d) : (unsigned long long)d;
    unsigned long long uq = udiv64(un, ud);
    return neg ? -(long long)uq : (long long)uq;
}

unsigned long long __udivdi3(unsigned long long n, unsigned long long d) {
    return udiv64(n, d);
}

void abort(void) {
    while (1) {
        __asm__ volatile ("cli; hlt");
    }
}

void *malloc(size_t size) {
    return kmalloc((uint32_t)size);
}

void free(void *ptr) {
    kfree(ptr);
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) kmemset(p, 0, (uint32_t)total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    uint32_t old_size;
    uint32_t copy_size;
    if (!ptr) return malloc(size);
    if (!size) {
        free(ptr);
        return 0;
    }
    old_size = ksize(ptr);
    void *n = malloc(size);
    if (!n) return 0;
    copy_size = old_size;
    if (copy_size > (uint32_t)size) copy_size = (uint32_t)size;
    if (copy_size) kmemcpy(n, ptr, copy_size);
    free(ptr);
    return n;
}

void *memset(void *dst, int c, size_t n) {
    return kmemset(dst, c, (uint32_t)n);
}

void *memcpy(void *dst, const void *src, size_t n) {
    return kmemcpy(dst, src, (uint32_t)n);
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    return kmemcmp(a, b, (uint32_t)n);
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return 0;
}

size_t strlen(const char *s) {
    return (size_t)kstrlen(s);
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    if (!s) return 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src) {
    return kstrcpy(dst, src);
}

char *strncpy(char *dst, const char *src, size_t n) {
    return kstrncpy(dst, src, (uint32_t)n);
}

int strcmp(const char *a, const char *b) {
    return kstrcmp(a, b);
}

int strncmp(const char *a, const char *b, size_t n) {
    return kstrncmp(a, b, (uint32_t)n);
}

char *strchr(const char *s, int c) {
    return (char*)kstrchr(s, c);
}

char *strrchr(const char *s, int c) {
    const char *last = 0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == 0) return (char*)s;
    return (char*)last;
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t sl = strlen(src);
    if (size) {
        size_t n = (sl >= size) ? (size - 1) : sl;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return sl;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char*)malloc(n);
    if (!p) return 0;
    memcpy(p, s, n);
    return p;
}

float strtof(const char *nptr, char **endptr) {
    const char *s = nptr;
    float sign = 1.0f;
    float val = 0.0f;
    float frac = 0.0f;
    float scale = 1.0f;
    int any = 0;

    while (is_space(*s)) s++;
    if (*s == '+' || *s == '-') {
        if (*s == '-') sign = -1.0f;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0f + (float)(*s - '0');
        s++;
        any = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            frac = frac * 10.0f + (float)(*s - '0');
            scale *= 10.0f;
            s++;
            any = 1;
        }
    }
    if (endptr) *endptr = (char*)(any ? s : nptr);
    return sign * (val + frac / scale);
}

static float wrap_pi(float x) {
    const float pi = 3.14159265f;
    const float tpi = 6.28318530f;
    while (x > pi) x -= tpi;
    while (x < -pi) x += tpi;
    return x;
}

float sinf(float x) {
    x = wrap_pi(x);
    return x - (x * x * x) / 6.0f + (x * x * x * x * x) / 120.0f;
}

float cosf(float x) {
    const float hpi = 1.57079632f;
    return sinf(x + hpi);
}

float exp2f(float x) {
    int n = (int)x;
    float r = 1.0f;
    if (n > 30) n = 30;
    if (n < -30) n = -30;
    while (n > 0) { r *= 2.0f; n--; }
    while (n < 0) { r *= 0.5f; n++; }
    return r;
}

float fminf(float a, float b) {
    return (a < b) ? a : b;
}

double ceil(double x) {
    long long i = (long long)x;
    if ((double)i == x) return x;
    if (x > 0.0) return (double)(i + 1);
    return (double)i;
}

float ceilf(float x) {
    return (float)ceil((double)x);
}

double fmod(double x, double y) {
    long long q;
    if (y == 0.0) return 0.0;
    q = (long long)(x / y);
    return x - ((double)q * y);
}

float fmodf(float x, float y) {
    return (float)fmod((double)x, (double)y);
}

locale_t newlocale(int mask, const char *locale, locale_t base) {
    (void)mask; (void)locale; (void)base;
    return 0;
}

locale_t uselocale(locale_t locale) {
    (void)locale;
    return 0;
}

void freelocale(locale_t locale) {
    (void)locale;
}

time_t time(time_t *tloc) {
    time_t t = ALOS_RTC_EPOCH + (time_t)(timer_ms() / 1000u);
    if (tloc) *tloc = t;
    return t;
}

int gettimeofday(struct timeval *tv, void *tz) {
    uint32_t ms;
    (void)tz;
    if (!tv) return -1;
    ms = timer_ms();
    tv->tv_sec = ALOS_RTC_EPOCH + (time_t)(ms / 1000u);
    tv->tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
    return 0;
}

static int is_leap_year(int year) {
    if ((year % 4) != 0) return 0;
    if ((year % 100) != 0) return 1;
    return (year % 400) == 0;
}

static int month_days(int year, int month) {
    static const int days[12] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };
    if (month == 1 && is_leap_year(year)) return 29;
    return days[month];
}

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    long days;
    long secs_in_day;
    int year = 1970;
    int month = 0;
    if (!timep || !result) return 0;
    days = (long)(*timep / 86400);
    secs_in_day = (long)(*timep % 86400);
    if (secs_in_day < 0) {
        secs_in_day += 86400;
        days--;
    }

    result->tm_hour = (int)(secs_in_day / 3600);
    secs_in_day %= 3600;
    result->tm_min = (int)(secs_in_day / 60);
    result->tm_sec = (int)(secs_in_day % 60);

    result->tm_wday = (int)((days + 4) % 7);
    if (result->tm_wday < 0) result->tm_wday += 7;

    while (1) {
        int ydays = is_leap_year(year) ? 366 : 365;
        if (days < ydays) break;
        days -= ydays;
        year++;
    }

    result->tm_yday = (int)days;
    while (1) {
        int mdays = month_days(year, month);
        if (days < mdays) break;
        days -= mdays;
        month++;
    }

    result->tm_mday = (int)days + 1;
    result->tm_mon = month;
    result->tm_year = year - 1900;
    result->tm_isdst = 0;
    return result;
}

time_t mktime(struct tm *tmv) {
    int year;
    int month;
    time_t days = 0;
    if (!tmv) return 0;

    year = tmv->tm_year + 1900;
    month = tmv->tm_mon;

    while (year > 1970) {
        year--;
        days += is_leap_year(year) ? 366 : 365;
    }
    for (int m = 0; m < month; m++) {
        days += month_days(tmv->tm_year + 1900, m);
    }
    days += (tmv->tm_mday - 1);
    return days * 86400 + tmv->tm_hour * 3600 + tmv->tm_min * 60 + tmv->tm_sec;
}

char *realpath(const char *path, char *resolved_path) {
    size_t n;
    if (!path) return 0;
    n = strlen(path) + 1;
    if (!resolved_path) {
        resolved_path = (char*)malloc(n);
        if (!resolved_path) return 0;
    }
    memcpy(resolved_path, path, n);
    return resolved_path;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long offset) {
    void *p;
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    p = malloc(len);
    if (p && len) kmemset(p, 0, (uint32_t)len);
    return p;
}

int munmap(void *addr, size_t len) {
    (void)len;
    free(addr);
    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    (void)path; (void)mode;
    return 0;
}

int fclose(FILE *stream) {
    (void)stream;
    return -1;
}

char *fgets(char *s, int size, FILE *stream) {
    (void)s; (void)size; (void)stream;
    return 0;
}

static int vfmt_emit(char *dst, size_t dst_sz, size_t *pos, char c) {
    if (dst && *pos + 1 < dst_sz) dst[*pos] = c;
    (*pos)++;
    return 0;
}

static int vfmt_str(char *dst, size_t dst_sz, size_t *pos, const char *s) {
    if (!s) s = "(null)";
    while (*s) {
        vfmt_emit(dst, dst_sz, pos, *s++);
    }
    return 0;
}

int vsnprintf(char *dst, size_t dst_sz, const char *fmt, va_list ap) {
    size_t pos = 0;
    while (fmt && *fmt) {
        if (*fmt != '%') {
            vfmt_emit(dst, dst_sz, &pos, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            vfmt_emit(dst, dst_sz, &pos, '%');
            fmt++;
            continue;
        }
        if (*fmt == 's') {
            const char *s = __builtin_va_arg(ap, const char*);
            vfmt_str(dst, dst_sz, &pos, s);
            fmt++;
            continue;
        }
        if (*fmt == 'c') {
            int c = __builtin_va_arg(ap, int);
            vfmt_emit(dst, dst_sz, &pos, (char)c);
            fmt++;
            continue;
        }
        if (*fmt == 'd' || *fmt == 'i') {
            int v = __builtin_va_arg(ap, int);
            char buf[32];
            kitoa(v, buf);
            vfmt_str(dst, dst_sz, &pos, buf);
            fmt++;
            continue;
        }
        if (*fmt == 'u') {
            uint32_t v = __builtin_va_arg(ap, uint32_t);
            char buf[32];
            kuitoa(v, buf, 10);
            vfmt_str(dst, dst_sz, &pos, buf);
            fmt++;
            continue;
        }
        if (*fmt == 'x' || *fmt == 'X') {
            uint32_t v = __builtin_va_arg(ap, uint32_t);
            char buf[32];
            kuitoa(v, buf, 16);
            vfmt_str(dst, dst_sz, &pos, buf);
            fmt++;
            continue;
        }
        vfmt_emit(dst, dst_sz, &pos, '?');
    }
    if (dst && dst_sz) {
        size_t end = (pos < dst_sz - 1) ? pos : (dst_sz - 1);
        dst[end] = '\0';
    }
    return (int)pos;
}

int snprintf(char *dst, size_t dst_sz, const char *fmt, ...) {
    int n;
    va_list ap;
    __builtin_va_start(ap, fmt);
    n = vsnprintf(dst, dst_sz, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int sprintf(char *dst, const char *fmt, ...) {
    int n;
    va_list ap;
    __builtin_va_start(ap, fmt);
    n = vsnprintf(dst, 0x7FFFFFFFu, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    char buf[512];
    (void)stream;
    return vsnprintf(buf, sizeof(buf), fmt, ap);
}

int printf(const char *fmt, ...) {
    int n;
    va_list ap;
    char buf[512];
    __builtin_va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    return n;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr;
    (void)stream;
    return size * nmemb;
}

int fputs(const char *s, FILE *stream) {
    (void)stream;
    return s ? (int)strlen(s) : 0;
}

int fputc(int c, FILE *stream) {
    (void)stream;
    return c;
}

int __printf_chk(int flag, const char *fmt, ...) {
    int n;
    va_list ap;
    char buf[512];
    (void)flag;
    __builtin_va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int __fprintf_chk(FILE *stream, int flag, const char *fmt, ...) {
    int n;
    va_list ap;
    char buf[512];
    (void)stream;
    (void)flag;
    __builtin_va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int __sprintf_chk(char *dst, int flag, size_t objsz, const char *fmt, ...) {
    int n;
    va_list ap;
    (void)flag;
    (void)objsz;
    __builtin_va_start(ap, fmt);
    n = vsnprintf(dst, 0x7FFFFFFFu, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen) {
    (void)dstlen;
    return memcpy(dst, src, n);
}

void *__memset_chk(void *dst, int c, size_t n, size_t dstlen) {
    (void)dstlen;
    return memset(dst, c, n);
}

char *__strncpy_chk(char *dst, const char *src, size_t n, size_t dstlen) {
    (void)dstlen;
    return strncpy(dst, src, n);
}

int __snprintf_chk(char *dst, size_t dst_sz, int flag, size_t objsz, const char *fmt, ...) {
    int n;
    va_list ap;
    (void)flag;
    (void)objsz;
    __builtin_va_start(ap, fmt);
    n = vsnprintf(dst, dst_sz, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int __vsnprintf_chk(char *dst, size_t dst_sz, int flag, size_t objsz, const char *fmt, va_list ap) {
    (void)flag;
    (void)objsz;
    return vsnprintf(dst, dst_sz, fmt, ap);
}

void __stack_chk_fail(void) {
    abort();
}

void __stack_chk_fail_local(void) {
    abort();
}

char *gettext(const char *msg) {
    return (char *)(msg ? msg : "");
}

char *secure_getenv(const char *name) {
    (void)name;
    return 0;
}

int pthread_mutex_lock(void *mutex) {
    (void)mutex;
    return 0;
}

int pthread_mutex_unlock(void *mutex) {
    (void)mutex;
    return 0;
}

unsigned int __popcountsi2(unsigned int x) {
    unsigned int c = 0;
    while (x) {
        c += x & 1u;
        x >>= 1;
    }
    return c;
}

int __ctzdi2(unsigned long long x) {
    int c = 0;
    if (!x) return 64;
    while ((x & 1ull) == 0ull) {
        c++;
        x >>= 1;
    }
    return c;
}

unsigned long long __umoddi3(unsigned long long n, unsigned long long d) {
    unsigned long long q;
    if (!d) return 0;
    q = udiv64(n, d);
    return n - (q * d);
}

long long __divmoddi4(long long n, long long d, long long *rem) {
    long long q = __divdi3(n, d);
    if (rem) *rem = n - (q * d);
    return q;
}
