/* kernel/lib/string.c */
#include "string.h"
#include "simd.h"

/* Seuil en-dessous duquel le chemin scalaire reste utilise meme si SSE2 est
 * disponible : pour de petites copies, le cout d'une boucle d'instructions
 * SSE (alignement de la queue, etc.) n'est pas rentable face a une simple
 * boucle octet par octet. */
#define SIMD_MIN_BYTES 64u

/* Ces deux fonctions sont les SEULS points du noyau qui emettent du code
 * SSE2 (voir kernel/lib/simd.c pour l'explication du choix de ne PAS
 * compiler tout le fichier avec -msse2). "target/sse2" autorise gcc a
 * accepter les noms de registres xmm* dans l'asm inline de CETTE fonction
 * uniquement, sans changer les options de compilation du reste du fichier.
 * xmm0 est utilise en pur scratch, jamais suppose survivre a un appel de
 * fonction ni a un changement de contexte (voir la note dans simd.c sur
 * l'absence de FXSAVE/FXRSTOR par tache). */
__attribute__((target("sse2")))
static void simd_copy_blocks(uint8_t *d, const uint8_t *s, uint32_t chunks) {
    for (uint32_t i = 0; i < chunks; i++) {
        __asm__ volatile (
            "movdqu (%0), %%xmm0\n\t"
            "movdqu %%xmm0, (%1)\n\t"
            :
            : "r" (s + i * 16u), "r" (d + i * 16u)
            : "xmm0", "memory"
        );
    }
}

__attribute__((target("sse2")))
static void simd_set_blocks(uint8_t *d, uint8_t val, uint32_t chunks) {
    uint32_t pattern = ((uint32_t)val << 24) | ((uint32_t)val << 16) | ((uint32_t)val << 8) | (uint32_t)val;
    __asm__ volatile (
        "movd %0, %%xmm0\n\t"
        "pshufd $0, %%xmm0, %%xmm0\n\t"
        :
        : "r" (pattern)
        : "xmm0"
    );
    for (uint32_t i = 0; i < chunks; i++) {
        __asm__ volatile (
            "movdqu %%xmm0, (%0)\n\t"
            :
            : "r" (d + i * 16u)
            : "memory"
        );
    }
}

void *kmemset(void *dst, int c, uint32_t n) {
    uint8_t *p = (uint8_t*)dst;
    uint8_t val = (uint8_t)c;
    if (simd_available() && n >= SIMD_MIN_BYTES) {
        uint32_t chunks = n / 16u;
        uint32_t done = chunks * 16u;
        simd_set_blocks(p, val, chunks);
        for (uint32_t k = done; k < n; k++) p[k] = val;
        return dst;
    }
    while (n--) *p++ = val;
    return dst;
}
void *kmemcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t*)dst; const uint8_t *s = (const uint8_t*)src;
    if (simd_available() && n >= SIMD_MIN_BYTES) {
        uint32_t chunks = n / 16u;
        uint32_t done = chunks * 16u;
        simd_copy_blocks(d, s, chunks);
        for (uint32_t k = done; k < n; k++) d[k] = s[k];
        return dst;
    }
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
