/**
 * kernel/jack/vm_interp.c — Interpréteur Hack VM (Nand2Tetris) v5
 * Version propre, tout dans le bon ordre, 0 symbole manquant.
 */

#include "vm_interp.h"
#include "vm_store.h"
#include "font5x8.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include "../fs/ramfs.h"
#include "../../driver/vga.h"
#include "../../driver/gfx.h"
#include "../../driver/keyboard.h"
#include "../../driver/timer.h"

/* ════════════════════════════════════════════════════════════════
 * SECTION 0 : Tableaux statiques globaux (.bss)
 * ════════════════════════════════════════════════════════════════ */
VMInstr vm_prog [VM_MAX_INSTRS];
VMFunc  vm_funcs[VM_MAX_FUNCS];
int16_t vm_ram  [HACK_RAM_SIZE];

typedef struct { int32_t idx; char name[VM_MAX_NAME]; } OSCall;
static OSCall  os_calls[VM_MAX_OS_CALLS];
static int     nos_calls;

typedef struct { char name[VM_MAX_NAME]; int32_t idx; } LabelEnt;
typedef struct { int32_t instr; char name[VM_MAX_NAME]; } PatchEnt;
static LabelEnt label_tbl[VM_MAX_LABELS];
static int      nlabels;
static PatchEnt patch_tbl[VM_MAX_PATCHES];
static int      npatches;
static uint16_t g_static_offset;

/* ════════════════════════════════════════════════════════════════
 * SECTION 1 : Parseur .vm → bytecode
 * ════════════════════════════════════════════════════════════════ */

static int label_find(const char *name) {
    for (int i=0;i<nlabels;i++)
        if (kstrcmp(label_tbl[i].name,name)==0) return (int)label_tbl[i].idx;
    return -1;
}
static void label_def(const char *name, int32_t idx) {
    if (nlabels>=VM_MAX_LABELS) return;
    kstrncpy(label_tbl[nlabels].name,name,VM_MAX_NAME);
    label_tbl[nlabels].idx=idx; nlabels++;
}
static void os_call_register(int32_t idx,const char *name) {
    if (nos_calls>=VM_MAX_OS_CALLS) return;
    os_calls[nos_calls].idx=idx;
    kstrncpy(os_calls[nos_calls].name,name,VM_MAX_NAME); nos_calls++;
}
static const char *os_call_find(int32_t idx) {
    for (int i=0;i<nos_calls;i++) if (os_calls[i].idx==idx) return os_calls[i].name;
    return 0;
}
static int seg_id(const char *s) {
    if (kstrcmp(s,"constant")==0) return 0;
    if (kstrcmp(s,"local")   ==0) return 1;
    if (kstrcmp(s,"argument")==0) return 2;
    if (kstrcmp(s,"this")    ==0) return 3;
    if (kstrcmp(s,"that")    ==0) return 4;
    if (kstrcmp(s,"temp")    ==0) return 5;
    if (kstrcmp(s,"pointer") ==0) return 6;
    if (kstrcmp(s,"static")  ==0) return 7;
    return -1;
}
static int is_os_file(const char *n) {
    static const char *os[]={"Array.vm","Keyboard.vm","Math.vm","Memory.vm",
                              "Output.vm","Screen.vm","String.vm","Sys.vm",0};
    for (int i=0;os[i];i++) if (kstrcmp(n,os[i])==0) return 1;
    return 0;
}
static int is_os_func(const char *name) {
    static const char *pfx[]={"Array.","Keyboard.","Math.","Memory.",
                              "Output.","Screen.","String.","Sys.",0};
    for (int i=0; pfx[i]; i++) {
        uint32_t n = kstrlen(pfx[i]);
        if (kstrncmp(name, pfx[i], n) == 0) return 1;
    }
    return 0;
}

#define MAXT 4
#define TOKL 96
static char TT[MAXT][TOKL];
static int  NTT;
static void vm_tok(const char *line) {
    NTT=0; const char *p=line;
    while (*p==' '||*p=='\t') p++;
    while (*p && !(*p=='/'&&*(p+1)=='/') && NTT<MAXT) {
        if (*p==' '||*p=='\t'){p++;continue;}
        int i=0;
        while (*p&&*p!=' '&&*p!='\t'&&!(*p=='/'&&*(p+1)=='/'))
            {if(i<TOKL-1)TT[NTT][i++]=*p;p++;}
        TT[NTT][i]='\0'; if(i) NTT++;
    }
}

static int load_vm_data(VMState *vm, const char *fname, const char *data,
                        char *errbuf, uint32_t errsize) {
    if (is_os_file(fname)) return 0;
    uint16_t file_static_base = g_static_offset;
    int      file_static_max  = -1;
    char cur_func[VM_MAX_NAME]; cur_func[0]='\0';
    const char *p=data;
    while (*p) {
        char lbuf[128]; int li=0;
        while (*p&&*p!='\n'){if(li<126)lbuf[li++]=*p;p++;}
        if(*p=='\n') p++;
        lbuf[li]='\0';
        vm_tok(lbuf);
        if(NTT==0) continue;
        if(vm->prog_len>=VM_MAX_INSTRS){
            if(errbuf) kstrncpy(errbuf,"Trop d instructions",errsize);
            return -1;
        }
        VMInstr *ins=&vm_prog[vm->prog_len];
        kmemset(ins,0,sizeof(*ins));

        if(kstrcmp(TT[0],"push")==0&&NTT>=3){
            int seg=seg_id(TT[1]); if(seg<0) continue;
            int idx=katoi(TT[2]);
            static const VMOpcode po[]={OP_PUSH_CONST,OP_PUSH_LOCAL,OP_PUSH_ARG,
                OP_PUSH_THIS,OP_PUSH_THAT,OP_PUSH_TEMP,OP_PUSH_POINTER,OP_PUSH_STATIC};
            ins->op=(VMOpcode)po[seg]; ins->arg=(int16_t)idx;
            if(seg==7){
                if(idx < 0) continue;
                ins->static_id=(uint16_t)(file_static_base+idx);
                if(idx > file_static_max) file_static_max = idx;
            }
            vm->prog_len++;
        } else if(kstrcmp(TT[0],"pop")==0&&NTT>=3){
            int seg=seg_id(TT[1]); if(seg<=0) continue;
            int idx=katoi(TT[2]);
            static const VMOpcode pp[]={(VMOpcode)0,OP_POP_LOCAL,OP_POP_ARG,
                OP_POP_THIS,OP_POP_THAT,OP_POP_TEMP,OP_POP_POINTER,OP_POP_STATIC};
            ins->op=(VMOpcode)pp[seg]; ins->arg=(int16_t)idx;
            if(seg==7){
                if(idx < 0) continue;
                ins->static_id=(uint16_t)(file_static_base+idx);
                if(idx > file_static_max) file_static_max = idx;
            }
            vm->prog_len++;
        } else if(kstrcmp(TT[0],"add")==0){ins->op=OP_ADD;vm->prog_len++;}
        else if(kstrcmp(TT[0],"sub")==0){ins->op=OP_SUB;vm->prog_len++;}
        else if(kstrcmp(TT[0],"neg")==0){ins->op=OP_NEG;vm->prog_len++;}
        else if(kstrcmp(TT[0],"eq") ==0){ins->op=OP_EQ; vm->prog_len++;}
        else if(kstrcmp(TT[0],"gt") ==0){ins->op=OP_GT; vm->prog_len++;}
        else if(kstrcmp(TT[0],"lt") ==0){ins->op=OP_LT; vm->prog_len++;}
        else if(kstrcmp(TT[0],"and")==0){ins->op=OP_AND;vm->prog_len++;}
        else if(kstrcmp(TT[0],"or") ==0){ins->op=OP_OR; vm->prog_len++;}
        else if(kstrcmp(TT[0],"not")==0){ins->op=OP_NOT;vm->prog_len++;}
        else if(kstrcmp(TT[0],"label")==0&&NTT>=2){
            char full[VM_MAX_NAME]; ksprintf(full,"%s$%s",cur_func,TT[1]);
            label_def(full,vm->prog_len);
        } else if(kstrcmp(TT[0],"goto")==0&&NTT>=2){
            char full[VM_MAX_NAME]; ksprintf(full,"%s$%s",cur_func,TT[1]);
            ins->op=OP_GOTO;
            int t=label_find(full); ins->target=(t>=0)?(int32_t)t:-1;
            if(t<0&&npatches<VM_MAX_PATCHES){
                kstrncpy(patch_tbl[npatches].name,full,VM_MAX_NAME);
                patch_tbl[npatches].instr=vm->prog_len; npatches++;
            }
            vm->prog_len++;
        } else if(kstrcmp(TT[0],"if-goto")==0&&NTT>=2){
            char full[VM_MAX_NAME]; ksprintf(full,"%s$%s",cur_func,TT[1]);
            ins->op=OP_IF_GOTO;
            int t=label_find(full); ins->target=(t>=0)?(int32_t)t:-1;
            if(t<0&&npatches<VM_MAX_PATCHES){
                kstrncpy(patch_tbl[npatches].name,full,VM_MAX_NAME);
                patch_tbl[npatches].instr=vm->prog_len; npatches++;
            }
            vm->prog_len++;
        } else if(kstrcmp(TT[0],"function")==0&&NTT>=3){
            kstrncpy(cur_func,TT[1],VM_MAX_NAME);
            ins->op=OP_FUNCTION; ins->arg=(int16_t)katoi(TT[2]);
            if(vm->nfuncs<VM_MAX_FUNCS){
                kstrncpy(vm_funcs[vm->nfuncs].name,TT[1],VM_MAX_NAME);
                vm_funcs[vm->nfuncs].entry=vm->prog_len; vm->nfuncs++;
            }
            label_def(TT[1],vm->prog_len);
            vm->prog_len++;
        } else if(kstrcmp(TT[0],"call")==0&&NTT>=3){
            ins->op=OP_CALL; ins->arg=(int16_t)katoi(TT[2]);
            int t=label_find(TT[1]);
            if(t>=0){ins->target=(int32_t)t;}
            else{
                ins->target=-1;
                if(is_os_func(TT[1])){
                    if(nos_calls>=VM_MAX_OS_CALLS){
                        if(errbuf) kstrncpy(errbuf,"Trop d'appels OS (augmente VM_MAX_OS_CALLS)",errsize);
                        return -1;
                    }
                    os_call_register(vm->prog_len,TT[1]);
                }else{
                    if(npatches>=VM_MAX_PATCHES){
                        if(errbuf) kstrncpy(errbuf,"Trop de patches (calls forward)",errsize);
                        return -1;
                    }
                    kstrncpy(patch_tbl[npatches].name,TT[1],VM_MAX_NAME);
                    patch_tbl[npatches].instr=vm->prog_len; npatches++;
                }
            }
            vm->prog_len++;
        } else if(kstrcmp(TT[0],"return")==0){
            ins->op=OP_RETURN; vm->prog_len++;
        }
    }
    if(file_static_max >= 0){
        uint32_t next_off = (uint32_t)g_static_offset + (uint32_t)file_static_max + 1u;
        if(HACK_STATIC_BASE + next_off >= HACK_RAM_SIZE){
            if(errbuf) kstrncpy(errbuf,"Zone static Hack saturée",errsize);
            return -1;
        }
        g_static_offset = (uint16_t)next_off;
    }
    (void)errbuf;(void)errsize;
    return 0;
}

static int resolve_patches(void) {
    int unresolved = 0;
    for (int i=0;i<npatches;i++){
        int t=label_find(patch_tbl[i].name);
        if(t>=0) vm_prog[patch_tbl[i].instr].target=(int32_t)t;
        else unresolved++;
    }
    return unresolved;
}

/* ════════════════════════════════════════════════════════════════
 * SECTION 2 : OS Hack natif
 * ════════════════════════════════════════════════════════════════ */
#define SP    (vm_ram[0])
#define LCL   (vm_ram[1])
#define ARG   (vm_ram[2])
#define THS   (vm_ram[3])
#define THT   (vm_ram[4])
#define RAM(i)(vm_ram[(uint16_t)(i)])

static inline void    vm_push(int16_t v){vm_ram[vm_ram[0]]=(int16_t)v;vm_ram[0]++;}
static inline int16_t vm_pop(void){vm_ram[0]--;return vm_ram[vm_ram[0]];}

static int16_t heap_ptr=HACK_HEAP_BASE;
static int16_t hack_alloc(int16_t s){
    int16_t b=heap_ptr;
    if(b+s+1>=HACK_SCREEN_BASE)return 0;
    RAM(b)=s;heap_ptr=(int16_t)(b+s+1);return(int16_t)(b+1);
}
static void hack_free(int16_t p){(void)p;}

static int16_t hack_mul(int16_t a,int16_t b){
    int neg=(a<0)^(b<0);
    int16_t ua=(int16_t)(a<0?-a:a),ub=(int16_t)(b<0?-b:b);
    int32_t r=0;
    for(int i=0;i<16;i++) if((ub>>i)&1) r+=(int32_t)ua<<i;
    return neg?(int16_t)-r:(int16_t)r;
}
static int16_t hack_div(int16_t a,int16_t b){
    if(!b)return 0;
    int neg=(a<0)^(b<0);
    int16_t ua=(int16_t)(a<0?-a:a),ub=(int16_t)(b<0?-b:b),q=0;
    for(int i=15;i>=0;i--)
        if((int32_t)ub<<i<=(int32_t)ua){ua=(int16_t)(ua-(int16_t)(ub<<i));q|=(int16_t)(1<<i);}
    return neg?(int16_t)-q:q;
}
static int16_t hack_sqrt(int16_t n){
    if(n<=0)return 0;
    int16_t r=0;
    for(int i=7;i>=0;i--){int16_t t=(int16_t)(r+(1<<i));if(hack_mul(t,t)<=n)r=t;}
    return r;
}

static int out_row=0,out_col=0,screen_mode=0;
static int g_screen_dirty=0;
static uint32_t g_vid_writes=0;
static uint32_t g_poke_calls=0;
static uint32_t g_draw_calls=0;
static int16_t  g_last_poke_addr=0;
static uint32_t g_last_flush_ms=0;
static uint32_t g_os_calls=0;
static uint32_t g_os_misses=0;
static int g_dirty_first=-1, g_dirty_last=-1;
static int g_exit_by_esc=0;
#define SYSWAIT_DIV 2u
#define VM_IO_POLL_MASK    0x3Fu   /* poll clavier toutes les 64 instructions */
#define VM_FLUSH_POLL_MASK 0x1FFu  /* test flush toutes les 512 instructions */
#define VM_FLUSH_MIN_MS    25u     /* limite le cout du blit (~40 fps max) */
#define C_OUT VGA_COLOR(VGA_BLACK,VGA_WHITE)

/* ── Save System (runtime VM, sans recompilation Jack) ─────────────── */
#define SAVE_TEAM_MAX    6
#define SAVE_POKEDEX_MAX 12
#define SAVE_ITEMS_MAX   4
#define SAVE_PICKUP_MAX  20

/* Game field offsets */
#define GAME_F_MAPMGR      0
#define GAME_F_PLAYER      1
#define GAME_F_POKEDEX     3
#define GAME_F_INVENTORY  10
#define GAME_F_PICKUPS    12
#define GAME_F_PREV_MAP   15
#define GAME_F_BATTLE_CNT 16
#define GAME_F_LIMITED    17
#define GAME_F_CHEN       18
#define GAME_F_NOAD       19

/* Player field offsets */
#define PLAYER_F_X      0
#define PLAYER_F_Y      1
#define PLAYER_F_DIR    2
#define PLAYER_F_TEAM   4
#define PLAYER_F_MONEY  6
#define PLAYER_F_STORY  7

/* Other field offsets */
#define MAPMGR_F_CURRMAP   1
#define TEAM_F_ARRAY       0
#define TEAM_F_COUNT       1
#define TEAM_F_ACTIVE      3
#define POKEMON_F_TYPE     0
#define POKEMON_F_LEVEL    1
#define POKEMON_F_HP       2
#define POKEMON_F_MAXHP    3
#define POKEMON_F_ATK      4
#define POKEMON_F_DEF      5
#define POKEMON_F_MOVES    6
#define POKEMON_F_MOVECNT  7
#define MONEY_F_AMOUNT     0
#define INVENTORY_F_ITEMS  0
#define ITEM_F_QTY         1
#define POKEDEX_F_FLAGS    0
#define POKEDEX_F_COUNT    2
#define PICKUPMGR_F_ITEMS  0
#define PICKUPMGR_F_COUNT  1
#define PICKUP_F_COLLECTED 5

typedef struct {
    int     valid;
    int16_t map_id;
    int16_t player_x, player_y, player_dir;
    int16_t story_progress;
    int16_t money;
    int16_t previous_map;
    int16_t battle_count;
    int16_t limited_vision;
    int16_t chen_defeated;
    int16_t noadkoko_defeated;
    int16_t inv_qty[SAVE_ITEMS_MAX];
    int16_t pokedex_flags[SAVE_POKEDEX_MAX];
    int16_t team_active;
    int16_t team_present[SAVE_TEAM_MAX];
    int16_t team_type[SAVE_TEAM_MAX];
    int16_t team_level[SAVE_TEAM_MAX];
    int16_t team_hp[SAVE_TEAM_MAX];
    int16_t pickup_count;
    int16_t pickup_collected[SAVE_PICKUP_MAX];
} VMSaveState;

static VMSaveState g_save;
static int16_t     g_game_obj=0;
static int32_t     g_game_run_entry=-1;
static uint8_t     g_f5_latch=0;
static uint8_t     g_f9_latch=0;

static inline int vm_addr_ok(int16_t p, int need_words){
    int32_t a=(int32_t)p;
    int32_t e=a+(int32_t)need_words;
    return (a>=HACK_HEAP_BASE) && (e<HACK_SCREEN_BASE);
}

static inline int16_t vm_clamp_i16(int16_t v,int16_t lo,int16_t hi){
    if(v<lo) return lo;
    if(v>hi) return hi;
    return v;
}

static int16_t vm_new_move(int16_t id,int16_t power,int16_t type){
    int16_t m=hack_alloc(3);
    if(!m) return 0;
    RAM(m+0)=id;
    RAM(m+1)=power;
    RAM(m+2)=type;
    return m;
}

static void vm_pokemon_base_stats(int16_t type,int16_t level,int16_t *max_hp,int16_t *atk,int16_t *def){
    int16_t mh=40+level,a=50+level,d=45+level;
    switch(type){
    case 0:  mh=39+level;  a=52+level;  d=43+level;  break;
    case 1:  mh=44+level;  a=48+level;  d=65+level;  break;
    case 2:  mh=60+level;  a=60+level;  d=60+level;  break;
    case 3:  mh=35+level;  a=55+level;  d=40+level;  break;
    case 4:  mh=40+level;  a=45+level;  d=40+level;  break;
    case 5:  mh=30+level;  a=56+level;  d=35+level;  break;
    case 6:  mh=50+level;  a=75+level;  d=85+level;  break;
    case 7:  mh=30+level;  a=45+level;  d=55+level;  break;
    case 8:  mh=70+level;  a=70+level;  d=80+level;  break;
    case 9:  mh=50+level;  a=42+level;  d=43+level;  break;
    case 10: mh=60+level;  a=90+level;  d=55+level;  break;
    case 11: mh=250+level; a=80+level;  d=100+level; break;
    default: break;
    }
    *max_hp=mh; *atk=a; *def=d;
}

static int16_t vm_new_pokemon(int16_t type,int16_t level,int16_t hp_saved){
    int16_t p=hack_alloc(8);
    int16_t mv=0,mc=0,mh=0,atk=0,def=0;
    if(!p) return 0;
    mv=hack_alloc(4);
    if(!mv) return 0;

    RAM(mv+0)=0; RAM(mv+1)=0; RAM(mv+2)=0; RAM(mv+3)=0;
    vm_pokemon_base_stats(type,level,&mh,&atk,&def);
    if(hp_saved<0) hp_saved=0;
    if(hp_saved>mh) hp_saved=mh;

    RAM(p+POKEMON_F_TYPE)=type;
    RAM(p+POKEMON_F_LEVEL)=level;
    RAM(p+POKEMON_F_HP)=hp_saved;
    RAM(p+POKEMON_F_MAXHP)=mh;
    RAM(p+POKEMON_F_ATK)=atk;
    RAM(p+POKEMON_F_DEF)=def;
    RAM(p+POKEMON_F_MOVES)=mv;

    switch(type){
    case 0:  RAM(mv+0)=vm_new_move(0,40,0); RAM(mv+1)=vm_new_move(1,40,1); mc=2; break;
    case 1:  RAM(mv+0)=vm_new_move(2,35,0); RAM(mv+1)=vm_new_move(3,40,2); mc=2; break;
    case 2:  RAM(mv+0)=vm_new_move(4,35,0); RAM(mv+1)=vm_new_move(5,45,3); mc=2; break;
    case 3:  RAM(mv+0)=vm_new_move(6,40,0); RAM(mv+1)=vm_new_move(7,40,4); mc=2; break;
    case 4:  RAM(mv+0)=vm_new_move(8,35,0); RAM(mv+1)=vm_new_move(9,60,5); mc=2; break;
    case 5:  RAM(mv+0)=vm_new_move(6,40,0); RAM(mv+1)=vm_new_move(12,45,0); mc=2; break;
    case 6:  RAM(mv+0)=vm_new_move(0,40,7); RAM(mv+1)=vm_new_move(14,50,7); mc=2; break;
    case 7:  RAM(mv+0)=vm_new_move(3,40,2); RAM(mv+1)=vm_new_move(15,55,2); mc=2; break;
    case 8:  RAM(mv+0)=vm_new_move(1,80,1); RAM(mv+1)=vm_new_move(12,90,0); mc=2; break;
    case 9:  RAM(mv+0)=vm_new_move(4,30,0); RAM(mv+1)=vm_new_move(14,50,3); mc=2; break;
    case 10: RAM(mv+0)=vm_new_move(0,50,0); RAM(mv+1)=vm_new_move(14,80,3); mc=2; break;
    case 11: RAM(mv+0)=vm_new_move(0,50,0); RAM(mv+1)=vm_new_move(14,80,3); mc=2; break;
    default: RAM(mv+0)=vm_new_move(6,35,0); mc=1; break;
    }
    RAM(p+POKEMON_F_MOVECNT)=mc;
    return p;
}

static void vm_save_capture(void){
    int16_t game=g_game_obj;
    int16_t mapmgr,player,pokedex,inventory,pickups;
    int16_t team,money,arr;
    int16_t cnt;
    int i;

    if(!vm_addr_ok(game,20)) return;

    mapmgr   = RAM(game+GAME_F_MAPMGR);
    player   = RAM(game+GAME_F_PLAYER);
    pokedex  = RAM(game+GAME_F_POKEDEX);
    inventory= RAM(game+GAME_F_INVENTORY);
    pickups  = RAM(game+GAME_F_PICKUPS);
    if(!vm_addr_ok(player,8)) return;

    g_save.valid=0;
    g_save.map_id = vm_addr_ok(mapmgr,2) ? RAM(mapmgr+MAPMGR_F_CURRMAP) : 5;
    g_save.player_x = RAM(player+PLAYER_F_X);
    g_save.player_y = RAM(player+PLAYER_F_Y);
    g_save.player_dir = RAM(player+PLAYER_F_DIR);
    g_save.story_progress = RAM(player+PLAYER_F_STORY);
    g_save.previous_map = RAM(game+GAME_F_PREV_MAP);
    g_save.battle_count = RAM(game+GAME_F_BATTLE_CNT);
    g_save.limited_vision = RAM(game+GAME_F_LIMITED);
    g_save.chen_defeated = RAM(game+GAME_F_CHEN);
    g_save.noadkoko_defeated = RAM(game+GAME_F_NOAD);

    money = RAM(player+PLAYER_F_MONEY);
    g_save.money = vm_addr_ok(money,1) ? RAM(money+MONEY_F_AMOUNT) : 300;

    for(i=0;i<SAVE_ITEMS_MAX;i++) g_save.inv_qty[i]=0;
    if(vm_addr_ok(inventory,2)){
        arr=RAM(inventory+INVENTORY_F_ITEMS);
        for(i=0;i<SAVE_ITEMS_MAX;i++){
            int16_t it = vm_addr_ok(arr,i+1) ? RAM(arr+i) : 0;
            if(vm_addr_ok(it,2)) g_save.inv_qty[i]=RAM(it+ITEM_F_QTY);
        }
    }

    for(i=0;i<SAVE_POKEDEX_MAX;i++) g_save.pokedex_flags[i]=0;
    if(vm_addr_ok(pokedex,3)){
        arr=RAM(pokedex+POKEDEX_F_FLAGS);
        for(i=0;i<SAVE_POKEDEX_MAX;i++){
            if(vm_addr_ok(arr,i+1)) g_save.pokedex_flags[i]=RAM(arr+i);
        }
    }

    team = RAM(player+PLAYER_F_TEAM);
    g_save.team_active=0;
    for(i=0;i<SAVE_TEAM_MAX;i++){
        g_save.team_present[i]=0;
        g_save.team_type[i]=0;
        g_save.team_level[i]=5;
        g_save.team_hp[i]=10;
    }
    if(vm_addr_ok(team,4)){
        arr=RAM(team+TEAM_F_ARRAY);
        g_save.team_active=RAM(team+TEAM_F_ACTIVE);
        for(i=0;i<SAVE_TEAM_MAX;i++){
            int16_t poke = vm_addr_ok(arr,i+1) ? RAM(arr+i) : 0;
            if(vm_addr_ok(poke,4)){
                g_save.team_present[i]=1;
                g_save.team_type[i]=RAM(poke+POKEMON_F_TYPE);
                g_save.team_level[i]=RAM(poke+POKEMON_F_LEVEL);
                g_save.team_hp[i]=RAM(poke+POKEMON_F_HP);
            }
        }
    }

    g_save.pickup_count=0;
    for(i=0;i<SAVE_PICKUP_MAX;i++) g_save.pickup_collected[i]=0;
    if(vm_addr_ok(pickups,3)){
        arr=RAM(pickups+PICKUPMGR_F_ITEMS);
        cnt=RAM(pickups+PICKUPMGR_F_COUNT);
        if(cnt<0) cnt=0;
        if(cnt>SAVE_PICKUP_MAX) cnt=SAVE_PICKUP_MAX;
        g_save.pickup_count=cnt;
        for(i=0;i<cnt;i++){
            int16_t p = vm_addr_ok(arr,i+1) ? RAM(arr+i) : 0;
            if(vm_addr_ok(p,6)) g_save.pickup_collected[i]=RAM(p+PICKUP_F_COLLECTED);
        }
    }

    g_save.valid=1;
}

static void vm_save_apply(int16_t game){
    int16_t mapmgr,player,pokedex,inventory,pickups;
    int16_t team,money,arr;
    int16_t restored=0,active;
    int i;

    if(!g_save.valid) return;
    if(!vm_addr_ok(game,20)) return;

    mapmgr   = RAM(game+GAME_F_MAPMGR);
    player   = RAM(game+GAME_F_PLAYER);
    pokedex  = RAM(game+GAME_F_POKEDEX);
    inventory= RAM(game+GAME_F_INVENTORY);
    pickups  = RAM(game+GAME_F_PICKUPS);
    if(!vm_addr_ok(player,8)) return;

    if(vm_addr_ok(mapmgr,2)){
        int16_t mid=vm_clamp_i16(g_save.map_id,0,8);
        RAM(mapmgr+MAPMGR_F_CURRMAP)=mid;
    }

    RAM(player+PLAYER_F_X)=g_save.player_x;
    RAM(player+PLAYER_F_Y)=g_save.player_y;
    RAM(player+PLAYER_F_DIR)=g_save.player_dir;
    RAM(player+PLAYER_F_STORY)=g_save.story_progress;
    RAM(game+GAME_F_PREV_MAP)=g_save.previous_map;
    RAM(game+GAME_F_BATTLE_CNT)=g_save.battle_count;
    RAM(game+GAME_F_LIMITED)=g_save.limited_vision;
    RAM(game+GAME_F_CHEN)=g_save.chen_defeated;
    RAM(game+GAME_F_NOAD)=g_save.noadkoko_defeated;

    money=RAM(player+PLAYER_F_MONEY);
    if(vm_addr_ok(money,1)) RAM(money+MONEY_F_AMOUNT)=g_save.money;

    if(vm_addr_ok(inventory,2)){
        arr=RAM(inventory+INVENTORY_F_ITEMS);
        for(i=0;i<SAVE_ITEMS_MAX;i++){
            int16_t it = vm_addr_ok(arr,i+1) ? RAM(arr+i) : 0;
            if(vm_addr_ok(it,2)) RAM(it+ITEM_F_QTY)=g_save.inv_qty[i];
        }
    }

    if(vm_addr_ok(pokedex,3)){
        int16_t cap=0;
        arr=RAM(pokedex+POKEDEX_F_FLAGS);
        for(i=0;i<SAVE_POKEDEX_MAX;i++){
            int16_t v=g_save.pokedex_flags[i]? (int16_t)-1 : 0;
            if(vm_addr_ok(arr,i+1)) RAM(arr+i)=v;
            if(v) cap++;
        }
        RAM(pokedex+POKEDEX_F_COUNT)=cap;
    }

    team=RAM(player+PLAYER_F_TEAM);
    if(vm_addr_ok(team,4)){
        arr=RAM(team+TEAM_F_ARRAY);
        restored=0;
        for(i=0;i<SAVE_TEAM_MAX;i++){
            if(!vm_addr_ok(arr,i+1)) break;
            if(g_save.team_present[i]){
                int16_t oldp=RAM(arr+i);
                int16_t np=0;
                if(vm_addr_ok(oldp,8) &&
                   RAM(oldp+POKEMON_F_TYPE)==g_save.team_type[i] &&
                   RAM(oldp+POKEMON_F_LEVEL)==g_save.team_level[i]){
                    int16_t mh=RAM(oldp+POKEMON_F_MAXHP);
                    int16_t hp=g_save.team_hp[i];
                    if(mh<1) mh=1;
                    if(hp<0) hp=0;
                    if(hp>mh) hp=mh;
                    RAM(oldp+POKEMON_F_HP)=hp;
                    np=oldp;
                }else{
                    np=vm_new_pokemon(g_save.team_type[i],g_save.team_level[i],g_save.team_hp[i]);
                }
                RAM(arr+i)=np;
                if(np) restored++;
            }else{
                RAM(arr+i)=0;
            }
        }
        RAM(team+TEAM_F_COUNT)=restored;
        if(restored<1) restored=1;
        active=vm_clamp_i16(g_save.team_active,0,(int16_t)(restored-1));
        RAM(team+TEAM_F_ACTIVE)=active;
    }

    if(vm_addr_ok(pickups,3)){
        int16_t cnt=RAM(pickups+PICKUPMGR_F_COUNT);
        if(cnt<0) cnt=0;
        if(cnt>SAVE_PICKUP_MAX) cnt=SAVE_PICKUP_MAX;
        arr=RAM(pickups+PICKUPMGR_F_ITEMS);
        for(i=0;i<cnt;i++){
            int16_t p = vm_addr_ok(arr,i+1) ? RAM(arr+i) : 0;
            if(vm_addr_ok(p,6)) RAM(p+PICKUP_F_COLLECTED)=g_save.pickup_collected[i];
        }
    }

    RAM(game+5)=0;   /* gameState */
    RAM(game+6)=-1;  /* running=true */
}

#define HACK_TEXT_COLS 64
#define HACK_TEXT_ROWS 23
#define HACK_CHAR_W    8
#define HACK_CHAR_H    11
#define HACK_PIX_W     512
#define HACK_PIX_H     256

static inline int out_gfx_mode(void){
    return screen_mode || gfx_is_graphics();
}

static inline void mark_screen_word(int wi){
    if((unsigned)wi>=8192u) return;
    if(g_dirty_first<0 || wi<g_dirty_first) g_dirty_first=wi;
    if(g_dirty_last<0  || wi>g_dirty_last)  g_dirty_last=wi;
    screen_mode=1;
    g_screen_dirty=1;
}
static inline void mark_screen_addr(int16_t ad){
    mark_screen_word((int)ad - HACK_SCREEN_BASE);
}
static inline void mark_screen_full(void){
    g_dirty_first=0;
    g_dirty_last=8191;
    screen_mode=1;
    g_screen_dirty=1;
}

static inline void hack_set_px_bw(int x,int y,int black){
    if((unsigned)x>=HACK_PIX_W || (unsigned)y>=HACK_PIX_H) return;
    int wi = y*32 + (x>>4);
    uint16_t m = (uint16_t)(1u << (x & 15));
    uint16_t old = (uint16_t)vm_ram[HACK_SCREEN_BASE + wi];
    uint16_t w = old;
    if(black) w |= m; else w &= (uint16_t)~m;
    if(w!=old){
        vm_ram[HACK_SCREEN_BASE + wi] = (int16_t)w;
        mark_screen_word(wi);
    }
}

static void hack_char_draw(int row,int col,int c){
    if(row<0 || row>=HACK_TEXT_ROWS || col<0 || col>=HACK_TEXT_COLS) return;
    int x0 = col * HACK_CHAR_W;
    int y0 = row * HACK_CHAR_H;

    /* Clear full character cell (white). */
    for(int y=0; y<HACK_CHAR_H; y++){
        for(int x=0; x<HACK_CHAR_W; x++){
            hack_set_px_bw(x0+x,y0+y,0);
        }
    }

    uint8_t uc = (uint8_t)c;
    const uint8_t *g = hack_font5x8[uc];
    /* glcdfont: 5 columns, each byte is 8 vertical bits (LSB=top). */
    for(int gx=0; gx<5; gx++){
        uint8_t bits = g[gx];
        for(int gy=0; gy<8; gy++){
            if((bits>>gy)&1){
                hack_set_px_bw(x0+1+gx,y0+1+gy,1);
            }
        }
    }

}

static void out_scroll(void){
    if(out_gfx_mode()){
        out_row=HACK_TEXT_ROWS-1;
        return;
    }
    vga_scroll_region(0,22);out_row=22;
}
static void out_nl(void){out_col=0;out_row++;if(out_row>=23)out_scroll();}
static void out_char(int c){
    if(c=='\n'||c==128){out_nl();return;}
    if(c=='\b'||c==129){
        if(out_col>0){
            out_col--;
            if(out_gfx_mode()) hack_char_draw(out_row,out_col,' ');
            else vga_put_char(out_col,out_row,' ',C_OUT);
        }
        return;
    }
    if(out_gfx_mode()){
        if(out_col>=HACK_TEXT_COLS) out_nl();
        if(c<32 || c>126) c='?';
        hack_char_draw(out_row,out_col,c);
        out_col++;
        return;
    }
    if(out_col>=80)out_nl();
    vga_put_char(out_col++,out_row,(char)c,C_OUT);
}
static void out_str(int16_t sp){
    if(sp<=0||sp>=HACK_SCREEN_BASE-2)return;
    int16_t len=RAM(sp+1);
    for(int16_t i=0;i<len;i++){
        int16_t c=RAM(sp+2+i);
        if(c=='\\'&&i+1<len){
            int16_t n=RAM(sp+2+i+1);
            if(n=='n'){out_nl();i++;continue;}
            if(n=='t'){out_char(' ');out_char(' ');out_char(' ');out_char(' ');i++;continue;}
        }
        out_char(c);
    }
}
static void out_int(int16_t n){char b[8];kitoa(n,b);for(int i=0;b[i];i++)out_char((int)b[i]);}

static int16_t str_new(int16_t c){int16_t p=hack_alloc((int16_t)(c+2));if(!p)return 0;RAM(p)=c;RAM(p+1)=0;return p;}
static void    str_app(int16_t s,int16_t c){if(s>0&&RAM(s+1)<RAM(s)){RAM(s+2+RAM(s+1))=c;RAM(s+1)++;}}
static int16_t str_len(int16_t s){return s>0?RAM(s+1):0;}
static int16_t str_chr(int16_t s,int16_t i){return s>0?RAM(s+2+i):0;}
static void    str_set(int16_t s,int16_t i,int16_t c){if(s>0)RAM(s+2+i)=c;}
static int16_t azerty_digit(int16_t c){
    if(c>='0'&&c<='9') return (int16_t)(c-'0');
    switch(c){
    case '&':  return 1;
    case 0xE9: return 2; /* é */
    case '"':  return 3;
    case '\'': return 4;
    case '(':  return 5;
    case '-':  return 6;
    case 0xE8: return 7; /* è */
    case '_':  return 8;
    case 0xE7: return 9; /* ç */
    case 0xE0: return 0; /* à */
    default:   return -1;
    }
}
static int16_t str_int(int16_t s){
    if(!s)return 0;
    int16_t len=RAM(s+1);int n=0,neg=0,st=0;
    if(len>0&&RAM(s+2)=='-'){neg=1;st=1;}
    for(int16_t i=st;i<len;i++){
        int16_t d=azerty_digit(RAM(s+2+i));
        if(d<0)break;
        n=n*10+d;
    }
    return(int16_t)(neg?-n:n);
}
static void str_seti(int16_t s,int16_t n){RAM(s+1)=0;char b[8];kitoa(n,b);for(int i=0;b[i];i++)str_app(s,(int16_t)b[i]);}

static int16_t key_to_hack(uint8_t k){
    if(k>=' '&&k<127){
        /* Canonique VM: rangée numérique AZERTY -> chiffres */
        switch(k){
        case '&':  return '1';
        case '"':  return '3';
        case '\'': return '4';
        case '(':  return '5';
        case '-':  return '6';
        case '_':  return '8';
        default:   return k;
        }
    }
    switch(k){
    case 0xE9: return '2'; /* é (AZERTY rangée chiffres) */
    case 0xE8: return '7'; /* è */
    case 0xE7: return '9'; /* ç */
    case 0xE0: return '0'; /* à */
    case KEY_ENTER:     return 128;
    case KEY_BACKSPACE: return 129;
    case KEY_LEFT:      return 130;
    case KEY_UP:        return 131;
    case KEY_RIGHT:     return 132;
    case KEY_DOWN:      return 133;
    case KEY_HOME:      return 134;
    case KEY_END:       return 135;
    case KEY_PGUP:      return 136;
    case KEY_PGDN:      return 137;
    case KEY_INSERT:    return 138;
    case KEY_DELETE:    return 139;
    case KEY_ESCAPE:    return 140;
    default:            return 0;
    }
}
static int16_t raw_scancode_fallback(uint8_t raw){
    /* Chiffres rangée haute (Set 1) */
    if(raw>=0x02 && raw<=0x0B){
        static const char d[] = "1234567890";
        return d[raw-0x02];
    }
    /* Numpad (Set 1) */
    switch(raw){
    case 0x47: return '7';
    case 0x48: return '8';
    case 0x49: return '9';
    case 0x4B: return '4';
    case 0x4C: return '5';
    case 0x4D: return '6';
    case 0x4F: return '1';
    case 0x50: return '2';
    case 0x51: return '3';
    case 0x52: return '0';
    case 0x53: return '.';
    case 0x1C: return 128; /* Enter */
    case 0x0E: return 129; /* Backspace */
    case 0x39: return ' ';
    default:   return 0;
    }
}
static int16_t readint_key_norm(int16_t k){
    if(k>='0'&&k<='9') return k;
    /* AZERTY sans Shift sur la rangée numérique */
    switch(k){
    case '&':  return '1';
    case '-':  return '6';
    case '"':  return '3';
    case '\'': return '4';
    case '(':  return '5';
    case '_':  return '8';
    default:   return 0;
    }
}
static void kbd_poll(void){
    uint8_t raw = keyboard_current_key();
    if(raw==0 && keyboard_available()) raw=keyboard_getkey();

    if(raw==KEY_F5){
        if(!g_f5_latch) vm_save_capture();
        g_f5_latch=1;
        RAM(HACK_KBD_ADDR)=0;
        return;
    }
    if(raw==KEY_F9){
        if(!g_f9_latch) vm_save_apply(g_game_obj);
        g_f9_latch=1;
        RAM(HACK_KBD_ADDR)=0;
        return;
    }
    g_f5_latch=0;
    g_f9_latch=0;

    if(raw){
        int16_t k=key_to_hack(raw);
        if(!k && raw>=32 && raw<160) k=raw;
        if(!k) k=raw_scancode_fallback(raw);
        RAM(HACK_KBD_ADDR)=k;
        return;
    }
    RAM(HACK_KBD_ADDR)=0;
}

/* Rendu écran : bit=0=BLANC, bit=1=NOIR (spec Nand2Tetris) */
static void screen_flush(void){
    if(!screen_mode && !g_screen_dirty)return;
    if(screen_mode){
        if(!gfx_is_graphics()){
            gfx_set_mode13();
            gfx_clear(0);
            mark_screen_full();
        }
        gfx_blit_hack_screen(vm_ram + HACK_SCREEN_BASE, g_dirty_first, g_dirty_last);
        g_dirty_first=-1;
        g_dirty_last=-1;
        g_screen_dirty=0;
        return;
    }
    for(int cy=0;cy<24;cy++){
        for(int cx=0;cx<80;cx++){
            int x0=(cx*512)/80,   x1=((cx+1)*512)/80;
            int y0=(cy*256)/24,   y1=((cy+1)*256)/24;
            int cnt=0,total=0;
            for(int hy=y0;hy<y1;hy++){
                for(int hx=x0;hx<x1;hx++){
                    int wi=hy*32+hx/16, b=hx&15;
                    if((vm_ram[HACK_SCREEN_BASE+wi]>>b)&1) cnt++;
                    total++;
                }
            }
            int lvl = (total>0) ? ((cnt*16)/total) : 0; /* 0..16 */
            char ch; uint8_t col;
            if(lvl<=0){       ch=' ';       col=VGA_COLOR(VGA_WHITE,VGA_WHITE); }
            else if(lvl<=2){  ch=(char)0xB0;col=VGA_COLOR(VGA_WHITE,VGA_BLACK); } /* light shade */
            else if(lvl<=5){  ch=(char)0xB1;col=VGA_COLOR(VGA_WHITE,VGA_BLACK); } /* medium shade */
            else if(lvl<=9){  ch=(char)0xB2;col=VGA_COLOR(VGA_WHITE,VGA_BLACK); } /* dark shade */
            else {            ch=(char)0xDB;col=VGA_COLOR(VGA_WHITE,VGA_BLACK); } /* full block */
            vga_put_char(cx,cy,ch,col);
        }
    }
    g_screen_dirty=0;
}

static void screen_px(int16_t x,int16_t y,int16_t c){
    if((unsigned)x>=512||(unsigned)y>=256)return;
    int wi=y*32+x/16,b=x&15;
    uint16_t old=(uint16_t)vm_ram[HACK_SCREEN_BASE+wi];
    uint16_t w=old;
    if(c) w|= (uint16_t)(1u<<b);
    else  w&=(uint16_t)~(1u<<b);
    if(w!=old){
        vm_ram[HACK_SCREEN_BASE+wi]=(int16_t)w;
        mark_screen_word(wi);
    }
}
static void screen_rect(int16_t x1,int16_t y1,int16_t x2,int16_t y2,int16_t c){
    for(int16_t y=y1;y<=y2;y++)for(int16_t x=x1;x<=x2;x++)screen_px(x,y,c);
}

static int16_t os_dispatch(const char *name,int16_t nargs){
    int16_t a0=nargs>0?RAM(ARG):0, a1=nargs>1?RAM(ARG+1):0,
            a2=nargs>2?RAM(ARG+2):0, a3=nargs>3?RAM(ARG+3):0;
    (void)a3;
    if(kstrcmp(name,"Math.init")==0)     return 0;
    if(kstrcmp(name,"Math.abs")==0)      return(int16_t)(a0<0?-a0:a0);
    if(kstrcmp(name,"Math.multiply")==0) return hack_mul(a0,a1);
    if(kstrcmp(name,"Math.divide")==0)   return hack_div(a0,a1);
    if(kstrcmp(name,"Math.sqrt")==0)     return hack_sqrt(a0);
    if(kstrcmp(name,"Math.max")==0)      return(int16_t)(a0>a1?a0:a1);
    if(kstrcmp(name,"Math.min")==0)      return(int16_t)(a0<a1?a0:a1);
    if(kstrcmp(name,"Memory.init")==0)   return 0;
    if(kstrcmp(name,"Memory.peek")==0)   return RAM(a0);
    if(kstrcmp(name,"Memory.poke")==0){
        int16_t old=RAM(a0);
        RAM(a0)=a1;
        g_poke_calls++;
        g_last_poke_addr=a0;
        if(a0>=HACK_SCREEN_BASE && a0<HACK_KBD_ADDR && old!=a1){mark_screen_addr(a0);g_vid_writes++;}
        return 0;
    }
    if(kstrcmp(name,"Memory.alloc")==0)  return hack_alloc(a0);
    if(kstrcmp(name,"Memory.deAlloc")==0){hack_free(a0);return 0;}
    if(kstrcmp(name,"Array.new")==0)     return hack_alloc(a0);
    if(kstrcmp(name,"Array.dispose")==0){hack_free(a0);return 0;}
    if(kstrcmp(name,"String.new")==0)         return str_new(a0);
    if(kstrcmp(name,"String.dispose")==0)     {hack_free(a0);return 0;}
    if(kstrcmp(name,"String.length")==0)      return str_len(a0);
    if(kstrcmp(name,"String.charAt")==0)      return str_chr(a0,a1);
    if(kstrcmp(name,"String.setCharAt")==0)   {str_set(a0,a1,a2);return 0;}
    if(kstrcmp(name,"String.appendChar")==0)  {str_app(a0,a1);return a0;}
    if(kstrcmp(name,"String.eraseLastChar")==0){if(a0>0&&RAM(a0+1)>0)RAM(a0+1)--;return 0;}
    if(kstrcmp(name,"String.intValue")==0)    return str_int(a0);
    if(kstrcmp(name,"String.setInt")==0)      {str_seti(a0,a1);return 0;}
    if(kstrcmp(name,"String.backSpace")==0)   return 129;
    if(kstrcmp(name,"String.doubleQuote")==0) return '"';
    if(kstrcmp(name,"String.newLine")==0)     return '\n';
    if(kstrcmp(name,"Output.init")==0)       {out_row=0;out_col=0;return 0;}
    if(kstrcmp(name,"Output.moveCursor")==0) {
        int r=(int)a0,c=(int)a1;
        if(out_gfx_mode()){
            if(r<0) r=0;
            if(r>=HACK_TEXT_ROWS) r=HACK_TEXT_ROWS-1;
            if(c<0) c=0;
            if(c>=HACK_TEXT_COLS) c=HACK_TEXT_COLS-1;
        }
        out_row=r; out_col=c; return 0;
    }
    if(kstrcmp(name,"Output.printChar")==0)  {out_char(a0);return 0;}
    if(kstrcmp(name,"Output.printString")==0){out_str(a0);return 0;}
    if(kstrcmp(name,"Output.printInt")==0)   {out_int(a0);return 0;}
    if(kstrcmp(name,"Output.println")==0)    {out_nl();return 0;}
    if(kstrcmp(name,"Output.backSpace")==0)  {out_char(129);return 0;}
    if(kstrcmp(name,"Screen.init")==0){
        kmemset(vm_ram+HACK_SCREEN_BASE,0,8192*2);g_draw_calls++;mark_screen_full();return 0;}
    if(kstrcmp(name,"Screen.clearScreen")==0){
        kmemset(vm_ram+HACK_SCREEN_BASE,0,8192*2);g_draw_calls++;mark_screen_full();screen_flush();return 0;}
    if(kstrcmp(name,"Screen.setColor")==0)      {RAM(13)=a0;screen_mode=1;return 0;}
    if(kstrcmp(name,"Screen.drawPixel")==0)     {screen_mode=1;g_screen_dirty=1;g_draw_calls++;screen_px(a0,a1,RAM(13));return 0;}
    if(kstrcmp(name,"Screen.drawLine")==0){
        screen_mode=1; g_draw_calls++;
        int16_t x1=a0,y1=a1,x2=a2,y2=a3;
        int16_t dx=(int16_t)(x2-x1),dy=(int16_t)(y2-y1);
        int16_t sx=(int16_t)(dx>0?1:-1),sy=(int16_t)(dy>0?1:-1);
        dx=(int16_t)(dx<0?-dx:dx);dy=(int16_t)(dy<0?-dy:dy);
        int16_t err=(int16_t)(dx-dy);
        for(;;){screen_px(x1,y1,RAM(13));if(x1==x2&&y1==y2)break;
            int16_t e2=(int16_t)(2*err);
            if(e2>-dy){err=(int16_t)(err-dy);x1=(int16_t)(x1+sx);}
            if(e2< dx){err=(int16_t)(err+dx);y1=(int16_t)(y1+sy);}}
        return 0;}
    if(kstrcmp(name,"Screen.drawRectangle")==0){screen_mode=1;g_screen_dirty=1;g_draw_calls++;screen_rect(a0,a1,a2,a3,RAM(13));return 0;}
    if(kstrcmp(name,"Screen.drawCircle")==0){
        screen_mode=1; g_screen_dirty=1; g_draw_calls++;
        int16_t cx=a0,cy=a1,r=a2;
        for(int16_t dy=(int16_t)-r;dy<=r;dy++){
            int16_t dx=(int16_t)hack_sqrt((int16_t)(hack_mul(r,r)-hack_mul(dy,dy)));
            screen_rect((int16_t)(cx-dx),(int16_t)(cy+dy),(int16_t)(cx+dx),(int16_t)(cy+dy),RAM(13));}
        return 0;}
    if(kstrcmp(name,"Keyboard.init")==0)     return 0;
    if(kstrcmp(name,"Keyboard.keyPressed")==0){kbd_poll();return RAM(HACK_KBD_ADDR);}
    if(kstrcmp(name,"Keyboard.readChar")==0){
        while(!keyboard_available()){for(volatile int i=0;i<50000;i++);}
        uint8_t raw=keyboard_getkey();
        int16_t k=key_to_hack(raw);
        if(!k && raw>=32 && raw<160) k=raw;
        if(!k) k=raw_scancode_fallback(raw);
        return k;
    }
    if(kstrcmp(name,"Keyboard.readLine")==0){
        out_str(a0);int16_t s=str_new(64);
        for(;;){while(!keyboard_available()){for(volatile int i=0;i<50000;i++);}
            uint8_t raw=keyboard_getkey();
            int16_t k=key_to_hack(raw);
            if(!k && raw>=32 && raw<160) k=raw;
            if(!k) k=raw_scancode_fallback(raw);
            if(k==128)break;
            if(k==129){if(str_len(s)>0){RAM(s+1)--;out_char(129);}}
            else{
                if(k==0) continue;
                int16_t m=readint_key_norm(k);
                if(m)k=m;
                str_app(s,k);out_char(k);
            }}
        out_nl();return s;}
    if(kstrcmp(name,"Keyboard.readInt")==0){
        int16_t s=(int16_t)os_dispatch("Keyboard.readLine",1);
        return str_int(s);
    }
    if(kstrcmp(name,"Sys.init")==0){
        os_dispatch("Memory.init",0); os_dispatch("Math.init",0);
        os_dispatch("Screen.init",0); os_dispatch("Output.init",0);
        os_dispatch("Keyboard.init",0);
        return -8888;}
    if(kstrcmp(name,"Sys.halt")==0)  return -9999;
    if(kstrcmp(name,"Sys.error")==0){
        char m[32];ksprintf(m,"Sys.error(%d)",(int)a0);
        vga_print_at(0,24,m,VGA_COLOR(VGA_RED,VGA_WHITE));return -9999;}
    if(kstrcmp(name,"Sys.wait")==0){
        uint32_t delay=(uint32_t)(a0<0?0:a0);
        if(delay>0){
            delay=(delay+SYSWAIT_DIV-1u)/SYSWAIT_DIV; /* speed-up gameplay waits */
            if(delay==0) delay=1;
        }
        uint32_t u=timer_ms()+delay;
        while(timer_ms()<u){
            kbd_poll();
            __asm__ volatile("hlt");
        }
        return 0;}
    return 0;
}

/* ════════════════════════════════════════════════════════════════
 * SECTION 3 : Boucle d'exécution avec trace debug (ligne 23)
 * ════════════════════════════════════════════════════════════════ */
static VMState *g_vm = 0;
static int  g_dbg_draw = 0;
static int  g_dbg_os   = 0;
static char g_dbg_last[48];

static int16_t os_traced(const char *name,int16_t nargs){
    g_dbg_os++;
    kstrncpy(g_dbg_last,name,47);
    if(name[0]=='S'&&name[1]=='c') g_dbg_draw++;
    if((g_dbg_os&0x7F)==0){
        char s[80];
        ksprintf(s,"OS#%d draw#%d SP=%d | %s",g_dbg_os,g_dbg_draw,(int)SP,g_dbg_last);
        vga_clear_line(23,VGA_COLOR(VGA_BLACK,VGA_BLACK));
        vga_print_at(0,23,s,VGA_COLOR(VGA_BLACK,VGA_YELLOW));
    }
    return os_dispatch(name,nargs);
}

#define EXEC_BODY(OS_CALL) \
    while(vm->running&&vm->pc>=0&&vm->pc<vm->prog_len){ \
        VMInstr *ins=&vm_prog[vm->pc++]; \
        switch(ins->op){ \
        case OP_PUSH_CONST:   vm_push(ins->arg);break; \
        case OP_PUSH_LOCAL:   vm_push(RAM(LCL+ins->arg));break; \
        case OP_PUSH_ARG:     vm_push(RAM(ARG+ins->arg));break; \
        case OP_PUSH_THIS:    vm_push(RAM(THS+ins->arg));break; \
        case OP_PUSH_THAT:    vm_push(RAM(THT+ins->arg));break; \
        case OP_PUSH_TEMP:    vm_push(RAM(5+ins->arg));break; \
        case OP_PUSH_POINTER: vm_push(ins->arg==0?THS:THT);break; \
        case OP_PUSH_STATIC:  vm_push(RAM(HACK_STATIC_BASE+ins->static_id));break; \
        case OP_POP_LOCAL:    RAM(LCL+ins->arg)=vm_pop();break; \
        case OP_POP_ARG:      RAM(ARG+ins->arg)=vm_pop();break; \
        case OP_POP_THIS:     {int16_t ad=(int16_t)(THS+ins->arg);int16_t v=vm_pop();int16_t o=RAM(ad);RAM(ad)=v;if(ad>=HACK_SCREEN_BASE&&ad<HACK_KBD_ADDR&&o!=v){mark_screen_addr(ad);g_vid_writes++;}}break; \
        case OP_POP_THAT:     {int16_t ad=(int16_t)(THT+ins->arg);int16_t v=vm_pop();int16_t o=RAM(ad);RAM(ad)=v;if(ad>=HACK_SCREEN_BASE&&ad<HACK_KBD_ADDR&&o!=v){mark_screen_addr(ad);g_vid_writes++;}}break; \
        case OP_POP_TEMP:     RAM(5+ins->arg)=vm_pop();break; \
        case OP_POP_POINTER:  {int16_t v=vm_pop();if(ins->arg==0)THS=v;else THT=v;}break; \
        case OP_POP_STATIC:   RAM(HACK_STATIC_BASE+ins->static_id)=vm_pop();break; \
        case OP_ADD:{int16_t b=vm_pop(),a=vm_pop();vm_push((int16_t)(a+b));}break; \
        case OP_SUB:{int16_t b=vm_pop(),a=vm_pop();vm_push((int16_t)(a-b));}break; \
        case OP_NEG:{vm_push((int16_t)-vm_pop());}break; \
        case OP_EQ: {int16_t b=vm_pop(),a=vm_pop();vm_push(a==b?(int16_t)-1:0);}break; \
        case OP_GT: {int16_t b=vm_pop(),a=vm_pop();vm_push(a>b?(int16_t)-1:0);}break; \
        case OP_LT: {int16_t b=vm_pop(),a=vm_pop();vm_push(a<b?(int16_t)-1:0);}break; \
        case OP_AND:{int16_t b=vm_pop(),a=vm_pop();vm_push((int16_t)(a&b));}break; \
        case OP_OR: {int16_t b=vm_pop(),a=vm_pop();vm_push((int16_t)(a|b));}break; \
        case OP_NOT:{vm_push((int16_t)~vm_pop());}break; \
        case OP_LABEL:break; \
        case OP_GOTO:    if(ins->target>=0){vm->pc=ins->target;}break; \
        case OP_IF_GOTO: {int16_t v=vm_pop();if(v&&ins->target>=0){vm->pc=ins->target;}}break; \
        case OP_FUNCTION:for(int i=0;i<ins->arg;i++)vm_push(0);break; \
        case OP_CALL:{ \
            int32_t tgt=ins->target;int16_t na=ins->arg; \
            int32_t spc=(int32_t)(ins-vm_prog); \
            if(tgt<0){ \
                g_os_calls++; \
                const char *fn=os_call_find(spc); \
                if(!fn){g_os_misses++;vm_push(0);break;} \
                int16_t sv=SP, oldARG=ARG; ARG=(int16_t)(sv-na); \
                int16_t ret=OS_CALL(fn,na); \
                SP=(int16_t)(sv-na); \
                ARG=oldARG; \
                if(ret==-9999){vm->running=0;break;} \
                if(ret==-8888){ \
                    int32_t mt=vm->main_entry; \
                    if(mt>=0){int16_t s0=SP,rpc=(int16_t)(vm->prog_len-1); \
                        vm_push(rpc);vm_push(LCL);vm_push(ARG);vm_push(THS);vm_push(THT); \
                        ARG=s0;LCL=SP;vm->pc=mt; \
                    }else{vm->running=0;} \
                    break; \
                } \
                vm_push(ret); \
            }else{ \
                if(g_game_run_entry>=0 && tgt==g_game_run_entry && na>0){ \
                    g_game_obj=RAM((int16_t)(SP-na)); \
                    if(g_save.valid) vm_save_apply(g_game_obj); \
                } \
                vm_push((int16_t)vm->pc); \
                vm_push(LCL);vm_push(ARG);vm_push(THS);vm_push(THT); \
                ARG=(int16_t)(SP-na-5);LCL=SP;vm->pc=tgt; \
            } \
            break; \
        } \
        case OP_RETURN:{ \
            int16_t fr=LCL,ra=RAM(fr-5),rv=vm_pop(); \
            SP=(int16_t)ARG;vm_push(rv); \
            THT=RAM(fr-1);THS=RAM(fr-2);ARG=RAM(fr-3);LCL=RAM(fr-4); \
            vm->pc=(int32_t)(uint16_t)ra; \
            if(vm->pc>=vm->prog_len){vm->running=0;} \
            break; \
        } \
        default:break; \
        } \
        cycle++; \
        if((cycle&VM_IO_POLL_MASK)==0){ \
            kbd_poll(); \
            if(RAM(HACK_KBD_ADDR)==140){vm_save_capture();g_exit_by_esc=1;vm->running=0;} \
        } \
        if((cycle&VM_FLUSH_POLL_MASK)==0){ \
            uint32_t _now=timer_ms(); \
            if((screen_mode||g_screen_dirty)&&(_now-g_last_flush_ms>=VM_FLUSH_MIN_MS)){screen_flush();g_last_flush_ms=_now;} \
        } \
        if((cycle&0x1FFF)==0 && !gfx_is_graphics()){ \
            char _dbg[80]; \
            ksprintf(_dbg,"pc=%d sp=%d vw=%u mc=%u dr=%u os=%u miss=%u", (int)vm->pc, (int)SP, (unsigned)g_vid_writes, (unsigned)g_poke_calls, (unsigned)g_draw_calls, (unsigned)g_os_calls, (unsigned)g_os_misses); \
            vga_clear_line(24, VGA_COLOR(VGA_DARK_GREY, VGA_WHITE)); \
            vga_print_at(0,24,_dbg,VGA_COLOR(VGA_DARK_GREY,VGA_WHITE)); \
        } \
    } \
    if(screen_mode||g_screen_dirty){screen_flush();}

static void vm_exec_release(VMState *vm){
    uint32_t cycle=0;
    EXEC_BODY(os_dispatch)
}

__attribute__((unused)) static void vm_exec_debug(VMState *vm){
    g_vm=vm; g_dbg_draw=0; g_dbg_os=0;
    kmemset(g_dbg_last,0,sizeof(g_dbg_last));
    uint32_t cycle=0;
    EXEC_BODY(os_traced)

    /* ── Diagnostic fin d'exécution ─────────────────────────────── */
    {
        uint8_t cy=VGA_COLOR(VGA_BLACK,VGA_YELLOW);
        uint8_t cr=VGA_COLOR(VGA_BLACK,VGA_LIGHT_RED);
        uint8_t ci=VGA_COLOR(VGA_BLACK,VGA_LIGHT_CYAN);
        char s[80];

        /* Ligne 22 : stats générales */
        ksprintf(s,"FIN VM: OS#%d draw#%d pc=%d SP=%d heap=%d",
                 g_dbg_os,g_dbg_draw,(int)vm->pc,(int)SP,(int)heap_ptr);
        vga_clear_line(22,VGA_COLOR(VGA_BLACK,VGA_BLACK));
        vga_print_at(0,22,s,g_dbg_os>0?cy:cr);

        /* Ligne 21 : dernier OS call */
        ksprintf(s,"last_os=%s | main@%d instrs=%d",
                 g_dbg_last,(int)vm->main_entry,(int)vm->prog_len);
        vga_clear_line(21,VGA_COLOR(VGA_BLACK,VGA_BLACK));
        vga_print_at(0,21,s,ci);

        /* Si crash rapide (<128 OS calls) : afficher l état complet */
        if(g_dbg_os < 128) {
            vga_clear();
            vga_print_at(0,0,"=== CRASH RAPIDE (<128 OS calls) ===",cr);
            ksprintf(s,"OS#%d draw#%d last=%s",g_dbg_os,g_dbg_draw,g_dbg_last);
            vga_print_at(0,1,s,cr);
            ksprintf(s,"pc=%d prog_len=%d main_entry=%d",
                     (int)vm->pc,(int)vm->prog_len,(int)vm->main_entry);
            vga_print_at(0,2,s,cr);
            ksprintf(s,"SP=%d LCL=%d ARG=%d THIS=%d THAT=%d",
                     (int)SP,(int)LCL,(int)ARG,(int)THS,(int)THT);
            vga_print_at(0,3,s,ci);
            ksprintf(s,"heap_ptr=%d (max=%d) free=%d words",
                     (int)heap_ptr,HACK_SCREEN_BASE,HACK_SCREEN_BASE-(int)heap_ptr);
            vga_print_at(0,4,s,ci);
            /* Instruction courante */
            if(vm->pc>0&&vm->pc<vm->prog_len){
                VMInstr *ii=&vm_prog[vm->pc];
                ksprintf(s,"next_instr: op=%d arg=%d target=%d static=%d",
                         (int)ii->op,(int)ii->arg,(int)ii->target,(int)ii->static_id);
                vga_print_at(0,5,s,VGA_COLOR(VGA_BLACK,VGA_YELLOW));
            }
            /* Labels autour de pc */
            int shown=0;
            for(int li=0;li<nlabels&&shown<8;li++){
                if(label_tbl[li].idx>=(int32_t)(vm->pc>5?vm->pc-5:0)
                   &&label_tbl[li].idx<=(int32_t)(vm->pc+5)){
                    ksprintf(s,"  lbl[%d]@%d=%s",li,(int)label_tbl[li].idx,label_tbl[li].name);
                    vga_print_at(0,6+shown,s,VGA_COLOR(VGA_BLACK,VGA_WHITE)); shown++;
                }
            }
        }
    }
}

/* ════════════════════════════════════════════════════════════════
 * SECTION 4 : API publique
 * ════════════════════════════════════════════════════════════════ */

static void bootstrap(VMState *vm){
    if(vm->prog_len>=VM_MAX_INSTRS-2)return;
    int bpc=vm->prog_len;
    VMInstr *c1=&vm_prog[vm->prog_len++]; kmemset(c1,0,sizeof(*c1));
    c1->op=OP_CALL;c1->arg=0;c1->target=-1; os_call_register(bpc,"Sys.init");
    VMInstr *c2=&vm_prog[vm->prog_len++]; kmemset(c2,0,sizeof(*c2));
    c2->op=OP_CALL;c2->arg=0;c2->target=-1; os_call_register(bpc+1,"Sys.halt");
    SP=HACK_STACK_BASE;ARG=HACK_STACK_BASE;LCL=HACK_STACK_BASE;
    vm->pc=bpc;
}

void vm_init(VMState *vm){
    kmemset(vm,0,sizeof(*vm));
    kmemset(vm_prog, 0,sizeof(vm_prog));
    kmemset(vm_funcs,0,sizeof(vm_funcs));
    kmemset(vm_ram,  0,sizeof(vm_ram));
    kmemset(os_calls,0,sizeof(os_calls));
    kmemset(label_tbl,0,sizeof(label_tbl));
    kmemset(patch_tbl,0,sizeof(patch_tbl));
    nos_calls=0;nlabels=0;npatches=0;g_static_offset=0;
    heap_ptr=HACK_HEAP_BASE;SP=HACK_STACK_BASE;
    out_row=0;out_col=0;screen_mode=0;g_screen_dirty=0;g_vid_writes=0;g_poke_calls=0;g_draw_calls=0;g_last_poke_addr=0;g_last_flush_ms=0;g_os_calls=0;g_os_misses=0;g_dirty_first=-1;g_dirty_last=-1;
}

int vm_load(VMState *vm,const char **files,int nfiles,char *errbuf,uint32_t errsize){
    for(int i=0;i<nfiles;i++){
        const char *fname=files[i];
        uint32_t fsz=0;
        const char *data=vm_store_find(fname,&fsz);
        if(!data){RamFSNode *n=ramfs_find(fname);if(n){data=ramfs_data(n);fsz=n->size;}}
        if(!data){
            if(errbuf){kstrncpy(errbuf,"VM introuvable: ",errsize);
                uint32_t l=kstrlen(errbuf);kstrncpy(errbuf+l,fname,errsize-l);}
            return -1;
        }
        if(load_vm_data(vm,fname,data,errbuf,errsize)<0)return -1;
    }
    if(resolve_patches()>0){
        if(errbuf) kstrncpy(errbuf,"References non resolues entre fonctions VM",errsize);
        return -1;
    }
    vm->main_entry=label_find("Main.main");
    bootstrap(vm);
    return 0;
}

int vm_load_store(VMState *vm,char *errbuf,uint32_t errsize){
    char list[2048]; vm_store_list(list,sizeof(list));
    char lname[64];int li=0;const char *p=list;
    while(*p){
        if(*p=='\n'){
            lname[li]='\0';int l=li;
            if(l>3&&lname[l-3]=='.'&&lname[l-2]=='v'&&lname[l-1]=='m'){
                int vmdir=(l>6&&lname[l-4]=='d'&&lname[l-5]=='i'&&lname[l-6]=='r');
                if(!vmdir){
                    uint32_t fsz=0;const char *data=vm_store_find(lname,&fsz);
                    if(!data){RamFSNode *n=ramfs_find(lname);if(n){data=ramfs_data(n);}}
                    if(data){if(load_vm_data(vm,lname,data,errbuf,errsize)<0)return -1;}
                }
            }
            li=0;
        }else if(li<63){lname[li++]=*p;}
        p++;
    }
    if(resolve_patches()>0){
        if(errbuf) kstrncpy(errbuf,"References non resolues entre fonctions VM",errsize);
        return -1;
    }
    vm->main_entry=label_find("Main.main");
    bootstrap(vm);
    return 0;
}

static int32_t vm_find_func_entry(VMState *vm,const char *name){
    for(int32_t i=0;i<vm->nfuncs;i++){
        if(kstrcmp(vm_funcs[i].name,name)==0) return vm_funcs[i].entry;
    }
    return -1;
}

void vm_run(VMState *vm){
    uint8_t c_ok=VGA_COLOR(VGA_BLACK,VGA_LIGHT_GREEN);
    uint8_t c_err=VGA_COLOR(VGA_BLACK,VGA_LIGHT_RED);
    uint8_t c_bar=VGA_COLOR(VGA_DARK_GREY,VGA_WHITE);
    char dbg[80];

    vga_print_at(0,24,"  Hack VM  |  ESC quitter  |  F5 save  |  F9 load",c_bar);

    if(vm->main_entry<0){
        vga_print_at(0,0,"ERREUR: Main.main introuvable!",c_err);
        ksprintf(dbg,"prog_len=%d nlabels=%d",(int)vm->prog_len,(int)nlabels);
        vga_print_at(0,1,dbg,c_err);
        int shown=0;
        for(int li=0;li<nlabels&&shown<8;li++){
            ksprintf(dbg,"  [%d] %s",li,label_tbl[li].name);
            vga_print_at(0,2+shown,dbg,VGA_COLOR(VGA_BLACK,VGA_LIGHT_CYAN));shown++;
        }
        vga_print_at(0,23,"Appuyez sur une touche...",c_err);
        while(!keyboard_available()){for(volatile int i=0;i<50000;i++);}
        keyboard_getkey();vm->running=0;return;
    }

    ksprintf(dbg,"Hack VM: %d instrs | Main@%d | lbl=%d",
             (int)vm->prog_len,(int)vm->main_entry,(int)nlabels);
    vga_print_at(0,0,dbg,c_ok);
    for(volatile int i=0;i<1200000;i++);

    g_game_run_entry=vm_find_func_entry(vm,"Game.run");
    g_game_obj=0;
    g_exit_by_esc=0;
    vm->running=1;
    vm_exec_release(vm);

    /* Restauration texte inconditionnelle: protege contre un etat
       graphique incoherent apres sortie rapide (ESC). */
    gfx_restore_text_mode();
    vga_init();

    if(g_exit_by_esc) return;

    vga_print_at(0,24,"  Programme termine. Appuyez sur une touche...",
                 VGA_COLOR(VGA_BLACK,VGA_LIGHT_GREEN));
    while(!keyboard_available()){for(volatile int i=0;i<50000;i++);}
    keyboard_getkey();
}

void vm_destroy(VMState *vm){(void)vm;}
