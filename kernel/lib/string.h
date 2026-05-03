#ifndef KERNEL_LIB_STRING_H
#define KERNEL_LIB_STRING_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void    *kmemset(void *dst, int c, uint32_t n);
void    *kmemcpy(void *dst, const void *src, uint32_t n);
int      kmemcmp(const void *a, const void *b, uint32_t n);
uint32_t kstrlen(const char *s);
int      kstrcmp(const char *a, const char *b);
int      kstrncmp(const char *a, const char *b, uint32_t n);
char    *kstrcpy(char *dst, const char *src);
char    *kstrncpy(char *dst, const char *src, uint32_t n);
const char *kstrchr(const char *s, int c);
int      katoi(const char *s);
void     kitoa(int n, char *buf);
void     kuitoa(uint32_t n, char *buf, int base);

#ifdef __cplusplus
}
#endif
#endif
