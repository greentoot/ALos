/* kernel/lib/string.c */
#include "string.h"

void *kmemset(void *dst, int c, uint32_t n) {
    uint8_t *p = (uint8_t*)dst;
    while (n--) *p++ = (uint8_t)c;
    return dst;
}
void *kmemcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t*)dst; const uint8_t *s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}
int kmemcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *p=(const uint8_t*)a, *q=(const uint8_t*)b;
    while (n--) { if (*p!=*q) return *p-*q; p++; q++; }
    return 0;
}
uint32_t kstrlen(const char *s) { uint32_t l=0; while(*s++) l++; return l; }
int kstrcmp(const char *a, const char *b) {
    while (*a && *a==*b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int kstrncmp(const char *a, const char *b, uint32_t n) {
    while (n-- && *a && *a==*b) { a++; b++; }
    return n==(uint32_t)-1 ? 0 : (unsigned char)*a-(unsigned char)*b;
}
char *kstrcpy(char *dst, const char *src) {
    char *d=dst; while ((*d++=*src++)); return dst;
}
char *kstrncpy(char *dst, const char *src, uint32_t n) {
    char *d=dst;
    while (n && (*d++=*src++)) n--;
    while (n--) *d++='\0';
    return dst;
}
const char *kstrchr(const char *s, int c) {
    while (*s) { if (*s==(char)c) return s; s++; }
    return (c==0)?s:0;
}
int katoi(const char *s) {
    int n=0, neg=0;
    while(*s==' ')s++;
    if(*s=='-'){neg=1;s++;} else if(*s=='+')s++;
    while(*s>='0'&&*s<='9') n=n*10+(*s++-'0');
    return neg?-n:n;
}
void kitoa(int n, char *buf) {
    if (n<0) { *buf++='-'; n=-n; }
    char tmp[12]; int i=0;
    if (n==0) tmp[i++]='0';
    else while(n>0){tmp[i++]='0'+(n%10);n/=10;}
    while(i-->0)*buf++=tmp[i];
    *buf='\0';
}
void kuitoa(uint32_t n, char *buf, int base) {
    const char *digits="0123456789abcdef";
    char tmp[33]; int i=0;
    if (n==0) tmp[i++]='0';
    else while(n>0){tmp[i++]=digits[n%base];n/=base;}
    while(i-->0)*buf++=tmp[i];
    *buf='\0';
}
