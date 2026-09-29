/* kernel/exec/asm_exec.c — Micro-assembleur i386 intégré, v2
 *
 * Corrections v2 :
 *  - g_image remplacé par un tableau par slot (plus de race condition)
 *  - SYS_EXIT : boucle hlt après zombie (la tâche ne revient plus jamais)
 *  - asm_task_entry robuste : même sans int 0x80, hlt si fn() retourne
 *  - Pas de syscall write dans les exemples (écrirait sur ptr NULL)
 */

#include "asm_exec.h"
#include "../process/task.h"
#include "../memory/heap.h"
#include "../lib/string.h"

/* ── Limites ────────────────────────────────────────────────────────────── */
#define MAX_CODE_BYTES  4096
#define MAX_LABELS       64
#define MAX_LABEL_LEN    32
#define MAX_PATCHES     128

/* ── Image exécutable ───────────────────────────────────────────────────── */
typedef struct {
    uint8_t  code[MAX_CODE_BYTES];
    uint32_t size;
    int      used;
} ExecImage;

/* Pool statique : pas de kmalloc nécessaire, et pas de race condition */
#define MAX_IMAGES  MAX_TASKS
static ExecImage image_pool[MAX_IMAGES];

static ExecImage *image_alloc(void) {
    for (int i = 0; i < MAX_IMAGES; i++)
        if (!image_pool[i].used) { image_pool[i].used = 1; return &image_pool[i]; }
    return 0;
}

static void image_free(ExecImage *img) {
    if (img) { img->used = 0; img->size = 0; }
}

/* ── Tables labels / patches ────────────────────────────────────────────── */
typedef struct { char name[MAX_LABEL_LEN]; uint32_t offset; } Label;
typedef struct { uint32_t patch_offset; char name[MAX_LABEL_LEN]; } Patch;

static Label  labels[MAX_LABELS];
static int    nlabels;
static Patch  patches[MAX_PATCHES];
static int    npatches;

static void label_define(const char *name, uint32_t offset) {
    if (nlabels >= MAX_LABELS) return;
    kstrncpy(labels[nlabels].name, name, MAX_LABEL_LEN);
    labels[nlabels].offset = offset;
    nlabels++;
}
static int label_find(const char *name, uint32_t *out) {
    for (int i = 0; i < nlabels; i++)
        if (kstrcmp(labels[i].name, name) == 0) { *out = labels[i].offset; return 1; }
    return 0;
}

/* ── Buffer de code ─────────────────────────────────────────────────────── */
static uint8_t  code_buf[MAX_CODE_BYTES];
static uint32_t code_len;

static int emit(uint8_t b) {
    if (code_len >= MAX_CODE_BYTES) return 0;
    code_buf[code_len++] = b; return 1;
}
static int emit32(int32_t v) {
    if (code_len + 4 > MAX_CODE_BYTES) return 0;
    code_buf[code_len++] = (uint8_t)(v);
    code_buf[code_len++] = (uint8_t)(v>>8);
    code_buf[code_len++] = (uint8_t)(v>>16);
    code_buf[code_len++] = (uint8_t)(v>>24);
    return 1;
}

/* ── Erreurs ────────────────────────────────────────────────────────────── */
static char    *g_errbuf;
static uint32_t g_errsize;
static int      g_errline;

static int err(const char *msg) {
    if (g_errbuf && g_errsize > 0) {
        char num[8]; kitoa(g_errline, num);
        kstrncpy(g_errbuf, "Ligne ", g_errsize);
        uint32_t l = kstrlen(g_errbuf);
        kstrncpy(g_errbuf+l, num, g_errsize-l); l = kstrlen(g_errbuf);
        kstrncpy(g_errbuf+l, ": ", g_errsize-l); l = kstrlen(g_errbuf);
        kstrncpy(g_errbuf+l, msg, g_errsize-l);
    }
    return 0;
}

/* ── Tokeniseur ─────────────────────────────────────────────────────────── */
#define MAX_TOK  5
#define TOK_LEN  48
static char tok[MAX_TOK][TOK_LEN];
static int  ntok;

static void tokenize_line(const char *line) {
    ntok = 0;
    const char *p = line;
    while (*p==' '||*p=='\t') p++;
    while (*p && *p!=';' && ntok < MAX_TOK) {
        if (*p==' '||*p=='\t'||*p==',') { p++; continue; }
        int i = 0;
        while (*p && *p!=' ' && *p!='\t' && *p!=',' && *p!=';')
            { if (i < TOK_LEN-1) tok[ntok][i++]=*p; p++; }
        tok[ntok][i]='\0';
        if (i > 0) ntok++;
    }
}

/* ── Parseur registre ───────────────────────────────────────────────────── */
typedef struct { const char *name; uint8_t id; } RegEntry;
static const RegEntry regs[] = {
    {"eax",0},{"ecx",1},{"edx",2},{"ebx",3},
    {"esp",4},{"ebp",5},{"esi",6},{"edi",7},{0,0}
};
static int parse_reg(const char *s) {
    for (int i = 0; regs[i].name; i++)
        if (kstrcmp(s, regs[i].name)==0) return regs[i].id;
    return -1;
}

/* ── Parseur immédiat (décimal / 0x…) ──────────────────────────────────── */
static int parse_imm(const char *s, int32_t *out) {
    if (s[0]=='0'&&(s[1]=='x'||s[1]=='X')) {
        uint32_t v=0; const char *p=s+2;
        while (*p) {
            uint8_t d;
            if (*p>='0'&&*p<='9')    d=(uint8_t)(*p-'0');
            else if (*p>='a'&&*p<='f') d=(uint8_t)(*p-'a'+10);
            else if (*p>='A'&&*p<='F') d=(uint8_t)(*p-'A'+10);
            else return 0;
            v=v*16+d; p++;
        }
        *out=(int32_t)v; return 1;
    }
    if (*s=='-'||(*s>='0'&&*s<='9')) { *out=katoi(s); return 1; }
    return 0;
}

/* ── Émission de sauts ──────────────────────────────────────────────────── */
static int emit_jump(uint8_t opc8, uint8_t opc32_1, uint8_t opc32_2,
                     const char *label) {
    uint32_t target;
    if (label_find(label, &target)) {
        int32_t rel8 = (int32_t)target - (int32_t)(code_len + 2);
        if (opc8 && rel8>=-128 && rel8<=127) {
            emit(opc8); emit((uint8_t)(int8_t)rel8);
        } else {
            uint32_t inst_len = opc32_1 ? 6u : 5u;
            int32_t rel32 = (int32_t)target - (int32_t)(code_len + inst_len);
            if (opc32_1) emit(opc32_1);
            emit(opc32_2); emit32(rel32);
        }
    } else {
        /* Forward ref → rel32 */
        if (npatches >= MAX_PATCHES) return err("trop de forward refs");
        kstrncpy(patches[npatches].name, label, MAX_LABEL_LEN);
        if (opc32_1) emit(opc32_1);
        emit(opc32_2);
        patches[npatches].patch_offset = code_len;
        emit32(0); npatches++;
    }
    return 1;
}

/* ── Assemblage d'une ligne ─────────────────────────────────────────────── */
static int assemble_line(void) {
    if (ntok == 0) return 1;

    const char *mn = tok[0];

    /* Label (finit par ':') */
    uint32_t mlen = kstrlen(mn);
    if (mn[mlen-1] == ':') {
        char lname[MAX_LABEL_LEN];
        kstrncpy(lname, mn, MAX_LABEL_LEN);
        lname[mlen-1] = '\0';
        label_define(lname, code_len);
        if (ntok > 1) {
            for (int i=0; i<ntok-1; i++) kstrcpy(tok[i], tok[i+1]);
            ntok--; mn = tok[0];
        } else return 1;
    }

    if (kstrcmp(mn,"nop")==0) { emit(0x90); return 1; }
    if (kstrcmp(mn,"hlt")==0) { emit(0xF4); return 1; }
    if (kstrcmp(mn,"ret")==0) { emit(0xC3); return 1; }
    if (kstrcmp(mn,"cli")==0) { emit(0xFA); return 1; }
    if (kstrcmp(mn,"sti")==0) { emit(0xFB); return 1; }

    /* int imm */
    if (kstrcmp(mn,"int")==0 && ntok==2) {
        int32_t v; if (!parse_imm(tok[1],&v)) return err("operande int invalide");
        if (v==3) { emit(0xCC); return 1; }
        emit(0xCD); emit((uint8_t)v); return 1;
    }

    /* push */
    if (kstrcmp(mn,"push")==0 && ntok==2) {
        int r=parse_reg(tok[1]);
        if (r>=0) { emit((uint8_t)(0x50+r)); return 1; }
        int32_t v; if (!parse_imm(tok[1],&v)) return err("operande push invalide");
        if (v>=-128&&v<=127) { emit(0x6A); emit((uint8_t)(int8_t)v); }
        else                 { emit(0x68); emit32(v); }
        return 1;
    }

    /* pop */
    if (kstrcmp(mn,"pop")==0 && ntok==2) {
        int r=parse_reg(tok[1]); if (r<0) return err("registre pop invalide");
        emit((uint8_t)(0x58+r)); return 1;
    }

    /* inc / dec */
    if (kstrcmp(mn,"inc")==0 && ntok==2) {
        int r=parse_reg(tok[1]); if (r<0) return err("registre inc invalide");
        emit((uint8_t)(0x40+r)); return 1;
    }
    if (kstrcmp(mn,"dec")==0 && ntok==2) {
        int r=parse_reg(tok[1]); if (r<0) return err("registre dec invalide");
        emit((uint8_t)(0x48+r)); return 1;
    }

    /* not */
    if (kstrcmp(mn,"not")==0 && ntok==2) {
        int r=parse_reg(tok[1]); if (r<0) return err("registre not invalide");
        emit(0xF7); emit((uint8_t)(0xD0|r)); return 1;
    }

    /* neg */
    if (kstrcmp(mn,"neg")==0 && ntok==2) {
        int r=parse_reg(tok[1]); if (r<0) return err("registre neg invalide");
        emit(0xF7); emit((uint8_t)(0xD8|r)); return 1;
    }

    /* mov reg, reg/imm */
    if (kstrcmp(mn,"mov")==0 && ntok==3) {
        int rd=parse_reg(tok[1]); if (rd<0) return err("destination mov invalide");
        int rs=parse_reg(tok[2]);
        if (rs>=0) { emit(0x89); emit((uint8_t)(0xC0|(rs<<3)|rd)); return 1; }
        int32_t imm; if (!parse_imm(tok[2],&imm)) return err("operande mov invalide");
        emit((uint8_t)(0xB8+rd)); emit32(imm); return 1;
    }

    /* imul reg, reg/imm */
    if (kstrcmp(mn,"imul")==0 && ntok==3) {
        int rd=parse_reg(tok[1]); if (rd<0) return err("dest imul invalide");
        int rs=parse_reg(tok[2]);
        if (rs>=0) { emit(0x0F); emit(0xAF); emit((uint8_t)(0xC0|(rd<<3)|rs)); return 1; }
        int32_t imm; if (!parse_imm(tok[2],&imm)) return err("operande imul invalide");
        if (imm>=-128&&imm<=127) {
            emit(0x6B); emit((uint8_t)(0xC0|(rd<<3)|rd)); emit((uint8_t)(int8_t)imm);
        } else {
            emit(0x69); emit((uint8_t)(0xC0|(rd<<3)|rd)); emit32(imm);
        }
        return 1;
    }

    /* Arithmétiques : add sub xor and or cmp */
    struct { const char *mn; uint8_t rm_op; uint8_t imm_reg; } arith[] = {
        {"add",0x01,0}, {"or",0x09,1}, {"and",0x21,4},
        {"sub",0x29,5}, {"xor",0x31,6}, {"cmp",0x39,7}, {0,0,0}
    };
    for (int ai=0; arith[ai].mn; ai++) {
        if (kstrcmp(mn, arith[ai].mn)!=0) continue;
        if (ntok!=3) return err("syntaxe op arith");
        int rd=parse_reg(tok[1]); if (rd<0) return err("destination invalide");
        int rs=parse_reg(tok[2]);
        if (rs>=0) { emit(arith[ai].rm_op); emit((uint8_t)(0xC0|(rs<<3)|rd)); return 1; }
        int32_t imm; if (!parse_imm(tok[2],&imm)) return err("operande invalide");
        if (imm>=-128&&imm<=127) {
            emit(0x83); emit((uint8_t)(0xC0|(arith[ai].imm_reg<<3)|rd));
            emit((uint8_t)(int8_t)imm);
        } else {
            emit(0x81); emit((uint8_t)(0xC0|(arith[ai].imm_reg<<3)|rd)); emit32(imm);
        }
        return 1;
    }

    /* Sauts */
    if (ntok==2) {
        const char *lbl=tok[1];
        if      (kstrcmp(mn,"jmp") ==0) return emit_jump(0xEB,0x00,0xE9,lbl);
        else if (kstrcmp(mn,"je")  ==0||kstrcmp(mn,"jz") ==0) return emit_jump(0x74,0x0F,0x84,lbl);
        else if (kstrcmp(mn,"jne") ==0||kstrcmp(mn,"jnz")==0) return emit_jump(0x75,0x0F,0x85,lbl);
        else if (kstrcmp(mn,"jl")  ==0||kstrcmp(mn,"jnge")==0)return emit_jump(0x7C,0x0F,0x8C,lbl);
        else if (kstrcmp(mn,"jg")  ==0||kstrcmp(mn,"jnle")==0)return emit_jump(0x7F,0x0F,0x8F,lbl);
        else if (kstrcmp(mn,"jle") ==0||kstrcmp(mn,"jng") ==0)return emit_jump(0x7E,0x0F,0x8E,lbl);
        else if (kstrcmp(mn,"jge") ==0||kstrcmp(mn,"jnl") ==0)return emit_jump(0x7D,0x0F,0x8D,lbl);
        else if (kstrcmp(mn,"call")==0) return emit_jump(0x00,0x00,0xE8,lbl);
    }

    return err("instruction inconnue");
}

/* ── Résolution forward refs ────────────────────────────────────────────── */
static int apply_patches(void) {
    for (int i=0; i<npatches; i++) {
        uint32_t target;
        if (!label_find(patches[i].name, &target)) {
            if (g_errbuf) {
                kstrncpy(g_errbuf, "Label indefini: ", g_errsize);
                uint32_t l=kstrlen(g_errbuf);
                kstrncpy(g_errbuf+l, patches[i].name, g_errsize-l);
            }
            return 0;
        }
        uint32_t po = patches[i].patch_offset;
        int32_t rel = (int32_t)target - (int32_t)(po+4);
        code_buf[po+0]=(uint8_t)(rel);
        code_buf[po+1]=(uint8_t)(rel>>8);
        code_buf[po+2]=(uint8_t)(rel>>16);
        code_buf[po+3]=(uint8_t)(rel>>24);
    }
    return 1;
}

/* ── Point d'entrée de la tâche ─────────────────────────────────────────── */
/* On passe l'image via une variable par slot de tâche :
   le pid est prévisible au moment de la création, donc on indexe
   dans pending_image[] qui est lu UNE SEULE FOIS dès que la tâche démarre. */

static ExecImage *pending_image[MAX_TASKS];  /* [slot] → image à exécuter */

static void asm_task_entry(void) {
    /* Trouver notre slot dans la table des tâches */
    Task *me = task_current();
    int slot = -1;
    Task *table = task_table();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (&table[i] == me) { slot = i; break; }
    }

    if (slot >= 0 && pending_image[slot]) {
        ExecImage *img = pending_image[slot];
        pending_image[slot] = 0;      /* consommé */

        /* Appel du code natif */
        void (*fn)(void) = (void(*)(void))img->code;
        fn();                         /* peut faire ret, hlt, ou int 0x80 */

        /* Libérer l'image */
        image_free(img);
    }

    /* ── Suicide propre ───────────────────────────────────────────────── */
    /* Marquer zombie directement (sans passer par syscall qui pourrait
       avoir un mauvais contexte de pile) puis boucler sur hlt. */
    if (slot >= 0)
        table[slot].state = TASK_ZOMBIE;

    /* Ne JAMAIS retourner : la tâche est zombie, le scheduler la sautera */
    __asm__ volatile ("cli");
    while (1) __asm__ volatile ("hlt");
}

/* ── Point d'entrée public ──────────────────────────────────────────────── */
int asmexec_run(const char *name, const char *src,
                char *errbuf, uint32_t errsize)
{
    code_len = 0; nlabels = 0; npatches = 0;
    g_errbuf = errbuf; g_errsize = errsize; g_errline = 0;
    if (errbuf && errsize) errbuf[0] = '\0';

    kmemset(code_buf, 0x90, MAX_CODE_BYTES);   /* NOP fill */

    /* ── Passe unique : assemble ──────────────────────────────────────── */
    const char *p = src;
    int line = 1;
    while (*p) {
        char lbuf[128]; int li=0;
        while (*p && *p!='\n') { if (li<126) lbuf[li++]=*p; p++; }
        if (*p=='\n') p++;
        lbuf[li]='\0';
        g_errline = line++;
        tokenize_line(lbuf);
        if (!assemble_line()) return ASMEXEC_ERR_SYNTAX;
    }

    /* Garantir une fin sure. Un simple "hlt" isole en bout de buffer ne
     * suffit PAS : cette tache est preemptee par le timer (IRQ0), et des
     * qu'un tick survient l'execution reprend UN OCTET APRES le hlt, donc
     * dans les octets qui suivent dans le buffer. S'ils ne sont pas
     * initialises (ou juste a zero), c'est du code arbitraire qui s'execute
     * et corrompt la pile de la tache (bug reel observe : #UD/#GP aleatoire
     * peu apres le premier tick suivant un programme finissant par "hlt").
     * On termine donc systematiquement par une boucle fermee "hlt; jmp $-2"
     * (sauf si le code se termine deja par un vrai "ret", qui rend
     * proprement la main a asm_task_entry — lequel marque la tache zombie
     * puis boucle lui-meme sur hlt avec les interruptions coupees). */
    if (code_len==0 || code_buf[code_len-1]!=0xC3) {
        if (!emit(0xF4)) return ASMEXEC_ERR_TOOLONG; /* hlt          */
        if (!emit(0xEB)) return ASMEXEC_ERR_TOOLONG; /* jmp rel8     */
        if (!emit(0xFD)) return ASMEXEC_ERR_TOOLONG; /* -> vers hlt  */
    }

    if (!apply_patches()) return ASMEXEC_ERR_UNDEF;

    /* ── Allouer l'image ──────────────────────────────────────────────── */
    ExecImage *img = image_alloc();
    if (!img) return ASMEXEC_ERR_OOM;
    /* Filet de securite : le reste du buffer (au-dela de code_len) doit
     * rester inoffensif si jamais l'execution devait un jour y arriver
     * (ex. bug futur). 0xF4 = hlt, jamais 0x00. */
    kmemset(img->code, 0xF4, MAX_CODE_BYTES);
    kmemcpy(img->code, code_buf, code_len);
    img->size = code_len;

    /* ── Créer la tâche ───────────────────────────────────────────────── */
    /* Trouver le prochain slot libre AVANT task_create pour y mettre l'image */
    Task *table = task_table();
    int free_slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (table[i].name[0]==0 || table[i].state==TASK_ZOMBIE) {
            free_slot = i; break;
        }
    }
    if (free_slot < 0) { image_free(img); return ASMEXEC_ERR_OOM; }

    pending_image[free_slot] = img;   /* dépose l'image avant la création */

    int pid = task_create(name, asm_task_entry);
    if (pid < 0) {
        pending_image[free_slot] = 0;
        image_free(img);
        return ASMEXEC_ERR_OOM;
    }

    return ASMEXEC_OK;
}