/* driver/keyboard.c — ALOS  Clavier PS/2  layout AZERTY */
#include "keyboard.h"
#include "../kernel/boot/earlydiag.h"

#ifndef ALOS_HW_SAFE
#define ALOS_HW_SAFE 0
#endif

#if ALOS_HW_SAFE
#include "usb_hid_kbd.h"
#endif

static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile ("inb %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0,%1"::"a"(v),"Nd"(port));
}
static inline void io_wait(void) { outb(0x80, 0x00); }

#define KBC_STATUS_OBF              0x01
#define KBC_STATUS_IBF              0x02
#define KBC_STATUS_AUX              0x20
#define KBC_CMD_READ_CONFIG         0x20
#define KBC_CMD_WRITE_CONFIG        0x60
#define KBC_CMD_DISABLE_FIRST_PORT  0xAD
#define KBC_CMD_ENABLE_FIRST_PORT   0xAE
#define KBC_CONFIG_IRQ1             0x01
#define KBC_CONFIG_FIRST_CLOCK_OFF  0x10
#define KBC_CONFIG_TRANSLATION      0x40
#define KBD_CMD_ENABLE_SCANNING     0xF4
#define KBD_ACK                     0xFA
#define KBD_RESEND                  0xFE

static int kbc_wait_input_clear(void) {
    for (int i = 0; i < 200000; ++i) {
        uint8_t status = inb(KBD_STATUS_PORT);
        if (status == 0xFF) return 0;
        if ((status & KBC_STATUS_IBF) == 0) return 1;
    }
    return 0;
}

static int kbc_wait_output_full(void) {
    for (int i = 0; i < 200000; ++i) {
        uint8_t status = inb(KBD_STATUS_PORT);
        if (status == 0xFF) return 0;
        if (status & KBC_STATUS_OBF) return 1;
    }
    return 0;
}

static void kbc_flush_output(void) {
    for (int i = 0; i < 64; ++i) {
        uint8_t status = inb(KBD_STATUS_PORT);
        if (status == 0xFF || (status & KBC_STATUS_OBF) == 0) break;
        (void)inb(KBD_DATA_PORT);
    }
}

static int kbc_write_command(uint8_t cmd) {
    if (!kbc_wait_input_clear()) return 0;
    outb(KBD_STATUS_PORT, cmd);
    return 1;
}

static int kbc_write_data(uint8_t value) {
    if (!kbc_wait_input_clear()) return 0;
    outb(KBD_DATA_PORT, value);
    return 1;
}

static int kbc_read_data(uint8_t *out) {
    if (!out || !kbc_wait_output_full()) return 0;
    *out = inb(KBD_DATA_PORT);
    return 1;
}

static int ps2_keyboard_send(uint8_t cmd) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t reply = 0;
        if (!kbc_write_data(cmd)) return 0;
        if (!kbc_read_data(&reply)) return 0;
        if (reply == KBD_ACK) return 1;
        if (reply != KBD_RESEND) return 0;
    }
    return 0;
}

static void ps2_keyboard_enable(void) {
    uint8_t config = 0;

    (void)kbc_write_command(KBC_CMD_DISABLE_FIRST_PORT);
    kbc_flush_output();

    if (kbc_write_command(KBC_CMD_READ_CONFIG) && kbc_read_data(&config)) {
        config |= (uint8_t)(KBC_CONFIG_IRQ1 | KBC_CONFIG_TRANSLATION);
        config &= (uint8_t)~KBC_CONFIG_FIRST_CLOCK_OFF;
        if (kbc_write_command(KBC_CMD_WRITE_CONFIG)) {
            (void)kbc_write_data(config);
        }
    }

    (void)kbc_write_command(KBC_CMD_ENABLE_FIRST_PORT);
    (void)ps2_keyboard_send(KBD_CMD_ENABLE_SCANNING);
}

static void ps2_keyboard_enable_legacy(void) {
    uint8_t config = 0;

    if (kbc_write_command(KBC_CMD_READ_CONFIG) && kbc_read_data(&config)) {
        config |= (uint8_t)(KBC_CONFIG_IRQ1 | KBC_CONFIG_TRANSLATION);
        config &= (uint8_t)~KBC_CONFIG_FIRST_CLOCK_OFF;
        if (kbc_write_command(KBC_CMD_WRITE_CONFIG)) {
            (void)kbc_write_data(config);
        }
    }

    (void)kbc_write_command(KBC_CMD_ENABLE_FIRST_PORT);
    (void)ps2_keyboard_send(KBD_CMD_ENABLE_SCANNING);
}

static int keyboard_read_ps2_scancode(uint8_t *out) {
    uint8_t status = inb(KBD_STATUS_PORT);
    uint8_t data;

    if (status == 0xFF) return 0;
    if ((status & KBC_STATUS_OBF) == 0) return 0;

    data = inb(KBD_DATA_PORT);
    if (status & KBC_STATUS_AUX) return -1;

    if (out) *out = data;
    return 1;
}

/* ─── IDT ────────────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t offset_low; uint16_t selector;
    uint8_t  zero;       uint8_t  type_attr;
    uint16_t offset_high;
} IDTEntry;
typedef struct __attribute__((packed)) { uint16_t limit; uint32_t base; } IDTPointer;

#define IDT_ENTRIES 49
static IDTEntry idt[IDT_ENTRIES];
IDTPointer      idt_ptr;
static uint16_t g_idt_selector = 0x08;
extern void idt_load(void);

extern void _exc0(void);  extern void _exc1(void);  extern void _exc2(void);
extern void _exc3(void);  extern void _exc4(void);  extern void _exc5(void);
extern void _exc6(void);  extern void _exc7(void);  extern void _exc8(void);
extern void _exc9(void);  extern void _exc10(void); extern void _exc11(void);
extern void _exc12(void); extern void _exc13(void); extern void _exc14(void);
extern void _exc15(void); extern void _exc16(void); extern void _exc17(void);
extern void _exc18(void); extern void _exc19(void); extern void _exc20(void);
extern void _exc21(void); extern void _exc22(void); extern void _exc23(void);
extern void _exc24(void); extern void _exc25(void); extern void _exc26(void);
extern void _exc27(void); extern void _exc28(void); extern void _exc29(void);
extern void _exc30(void); extern void _exc31(void);
extern void irq0_stub(void);
extern void irq1_stub(void);
extern void irq12_stub(void);
extern void int80_stub(void);

static void idt_set_gate(uint8_t n, uint32_t handler) {
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = g_idt_selector;
    idt[n].zero        = 0;
    idt[n].type_attr   = 0x8E;
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

static uint16_t read_code_segment_selector(void) {
    uint16_t cs = 0x08;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    return cs ? cs : 0x08;
}

/* ─── Panique CPU ─────────────────────────────────────────────────────────── */
void cpu_exception_handler(uint32_t num) {
    earlydiag_panic_num("CPU EXCEPTION", num);
    __asm__ volatile ("cli");
    while (1) __asm__ volatile ("hlt");
}

/* ─── PIC remap ───────────────────────────────────────────────────────────── */
static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();   /* IRQ0-7 -> vecteurs 32-39 */
    outb(0xA1, 0x28); io_wait();   /* IRQ8-15 -> vecteurs 40-47 */
    outb(0x21, 0x04); io_wait();   /* esclave sur IRQ2 */
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xF8);              /* IRQ0, IRQ1 et cascade IRQ2 */
    outb(0xA1, 0xEF);              /* IRQ12 souris */
}

/* ══════════════════════════════════════════════════════════════════════════
 * LAYOUT AZERTY  (clavier français standard)
 *
 * Rangée numérique sans shift :  & é " ' ( - è _ ç à
 * Rangée numérique avec shift  :  1 2 3 4 5 6 7 8 9 0
 * Touches spéciales AZERTY :
 *   sc 0x10 = a      sc 0x11 = z     sc 0x18 = o     sc 0x19 = p
 *   sc 0x1E = q      sc 0x1F = s     sc 0x30 = b     sc 0x32 = m
 *   sc 0x2C = w      (position QWERTY: z→w, q→a, a→q, etc.)
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * Table scancodes → caractère, sans modificateur (AltGr non géré ici).
 * Scancodes Set 1 (standard PS/2).
 *
 * Position physique → caractère AZERTY :
 *  02:& 03:é 04:" 05:' 06:( 07:- 08:è 09:_ 0A:ç 0B:à 0C:) 0D:=
 *  10:a 11:z 12:e 13:r 14:t 15:y 16:u 17:i 18:o 19:p
 *  1E:q 1F:s 20:d 21:f 22:g 23:h 24:j 25:k 26:l 27:m 28:ù
 *  2C:w 2D:x 2E:c 2F:v 30:b 31:n 32:, 33:; 34:: 35:!
 */
static const uint8_t sc_normal[128] = {
/*00*/  0,
/*01*/  KEY_ESCAPE,
/*02*/  '&',    /* 1 */
/*03*/  0xE9,   /* é  — on mappe sur 'e' faute de code étendu */
/*04*/  '"',
/*05*/  '\'',
/*06*/  '(',
/*07*/  '-',
/*08*/  0xE8,   /* è → 'e' */
/*09*/  '_',
/*0A*/  0xE7,   /* ç → 'c' */
/*0B*/  0xE0,   /* à → 'a' */
/*0C*/  ')',
/*0D*/  '=',
/*0E*/  KEY_BACKSPACE,
/*0F*/  KEY_TAB,
/*10*/  'a',
/*11*/  'z',
/*12*/  'e',
/*13*/  'r',
/*14*/  't',
/*15*/  'y',
/*16*/  'u',
/*17*/  'i',
/*18*/  'o',
/*19*/  'p',
/*1A*/  '^',    /* ^ (touche morte en vrai, ici caractère direct) */
/*1B*/  '$',
/*1C*/  KEY_ENTER,
/*1D*/  0,      /* CTRL gauche */
/*1E*/  'q',
/*1F*/  's',
/*20*/  'd',
/*21*/  'f',
/*22*/  'g',
/*23*/  'h',
/*24*/  'j',
/*25*/  'k',
/*26*/  'l',
/*27*/  'm',
/*28*/  0xF9,   /* ù */
/*29*/  '*',    /* ² (touche grave / astérisque) */
/*2A*/  0,      /* SHIFT gauche */
/*2B*/  '`',
/*2C*/  'w',
/*2D*/  'x',
/*2E*/  'c',
/*2F*/  'v',
/*30*/  'b',
/*31*/  'n',
/*32*/  ',',
/*33*/  ';',
/*34*/  ':',
/*35*/  '!',
/*36*/  0,      /* SHIFT droit */
/*37*/  '*',
/*38*/  0,      /* ALT gauche */
/*39*/  ' ',
/*3A*/  0,      /* CAPS LOCK */
/*3B*/  KEY_F1,  /*3C*/KEY_F2, /*3D*/KEY_F3, /*3E*/KEY_F4, /*3F*/KEY_F5,
/*40*/  KEY_F6,  /*41*/KEY_F7, /*42*/KEY_F8, /*43*/KEY_F9, /*44*/KEY_F10,
/*45*/  0,      /* NUMLOCK */
/*46*/  0,      /* SCROLLLOCK */
/*47*/  KEY_HOME,
/*48*/  KEY_UP,
/*49*/  KEY_PGUP,
/*4A*/  '-',
/*4B*/  KEY_LEFT,
/*4C*/  '5',
/*4D*/  KEY_RIGHT,
/*4E*/  '+',
/*4F*/  KEY_END,
/*50*/  KEY_DOWN,
/*51*/  KEY_PGDN,
/*52*/  KEY_INSERT,
/*53*/  KEY_DELETE,
/*54*/  0,0,0,
/*57*/  KEY_F11,
/*58*/  KEY_F12,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,
};

/* Table SHIFT — rangée numérique : 1 2 3 4 5 6 7 8 9 0 ° + */
static const uint8_t sc_shifted[128] = {
/*00*/  0,
/*01*/  KEY_ESCAPE,
/*02*/  '1',
/*03*/  '2',
/*04*/  '3',
/*05*/  '4',
/*06*/  '5',
/*07*/  '6',
/*08*/  '7',
/*09*/  '8',
/*0A*/  '9',
/*0B*/  '0',
/*0C*/  0xB0,   /* ° */
/*0D*/  '+',
/*0E*/  KEY_BACKSPACE,
/*0F*/  KEY_TAB,
/*10*/  'A',
/*11*/  'Z',
/*12*/  'E',
/*13*/  'R',
/*14*/  'T',
/*15*/  'Y',
/*16*/  'U',
/*17*/  'I',
/*18*/  'O',
/*19*/  'P',
/*1A*/  0xA8,   /* ¨ */
/*1B*/  '#',
/*1C*/  KEY_ENTER,
/*1D*/  0,
/*1E*/  'Q',
/*1F*/  'S',
/*20*/  'D',
/*21*/  'F',
/*22*/  'G',
/*23*/  'H',
/*24*/  'J',
/*25*/  'K',
/*26*/  'L',
/*27*/  'M',
/*28*/  '%',
/*29*/  0,
/*2A*/  0,      /* SHIFT gauche */
/*2B*/  '>',
/*2C*/  'W',
/*2D*/  'X',
/*2E*/  'C',
/*2F*/  'V',
/*30*/  'B',
/*31*/  'N',
/*32*/  '?',
/*33*/  '.',
/*34*/  '/',
/*35*/  0xA7,   /* § */
/*36*/  0,      /* SHIFT droit */
/*37*/  '*',
/*38*/  0,
/*39*/  ' ',
/*3A*/  0,
/*3B*/  KEY_F1,  /*3C*/KEY_F2, /*3D*/KEY_F3, /*3E*/KEY_F4, /*3F*/KEY_F5,
/*40*/  KEY_F6,  /*41*/KEY_F7, /*42*/KEY_F8, /*43*/KEY_F9, /*44*/KEY_F10,
/*45*/  0, /*46*/ 0,
/*47*/  KEY_HOME,
/*48*/  KEY_UP,
/*49*/  KEY_PGUP,
/*4A*/  '-',
/*4B*/  KEY_LEFT,
/*4C*/  '5',
/*4D*/  KEY_RIGHT,
/*4E*/  '+',
/*4F*/  KEY_END,
/*50*/  KEY_DOWN,
/*51*/  KEY_PGDN,
/*52*/  KEY_INSERT,
/*53*/  KEY_DELETE,
/*54*/  0,0,0,
/*57*/  KEY_F11,
/*58*/  KEY_F12,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,
};

/* Touches étendues (préfixe 0xE0) */
static uint8_t extended_keycode(uint8_t sc) {
    switch (sc) {
        case 0x48: return KEY_UP;     case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;   case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;   case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;   case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INSERT; case 0x53: return KEY_DELETE;
        case 0x1C: return KEY_ENTER;
        default:   return KEY_NONE;
    }
}

/* ─── État interne ─────────────────────────────────────────────────────────── */
static uint8_t   modifiers = 0;
static int       extended  = 0;
static KbdBuffer kbd_buf;
static uint8_t   key_active[256];
static uint8_t   key_from_sc[128];
static uint8_t   key_from_ext[128];
static uint8_t   last_key_down = KEY_NONE;
static int       ps2_controller_present = 0;

static void buf_push(uint8_t key) {
    if (kbd_buf.count >= KBD_BUFFER_SIZE) return;
    kbd_buf.buf[kbd_buf.tail] = key;
    kbd_buf.tail = (uint16_t)((kbd_buf.tail + 1) % KBD_BUFFER_SIZE);
    kbd_buf.count++;
}
static uint8_t buf_pop(void) {
    if (kbd_buf.count == 0) return KEY_NONE;
    uint8_t k = kbd_buf.buf[kbd_buf.head];
    kbd_buf.head = (uint16_t)((kbd_buf.head + 1) % KBD_BUFFER_SIZE);
    kbd_buf.count--;
    return k;
}

static uint8_t map_scancode_key(uint8_t code){
    int shift = (modifiers & MOD_SHIFT) != 0;

    /* CAPS LOCK : inverse shift uniquement pour lettres */
    if (modifiers & MOD_CAPS) {
        uint8_t base = sc_normal[code];
        if ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z')) {
            shift = !shift;
        }
    }

    uint8_t key = shift ? sc_shifted[code] : sc_normal[code];

    /* Numpad digits when NumLock active */
    if (modifiers & MOD_NUMLOCK) {
        switch (code) {
            case 0x47: key = '7'; break;
            case 0x48: key = '8'; break;
            case 0x49: key = '9'; break;
            case 0x4B: key = '4'; break;
            case 0x4C: key = '5'; break;
            case 0x4D: key = '6'; break;
            case 0x4F: key = '1'; break;
            case 0x50: key = '2'; break;
            case 0x51: key = '3'; break;
            case 0x52: key = '0'; break;
            case 0x53: key = '.'; break;
        }
    }

    switch (key) {
        case 0xE9: break;              /* é */
        case 0xE8: break;              /* è */
        case 0xEA: key = 'e'; break;   /* ê */
        case 0xE0: break;              /* à */
        case 0xE2: key = 'a'; break;   /* â */
        case 0xE7: break;              /* ç */
        case 0xF9: key = 'u'; break;   /* ù */
        case 0xFB: key = 'u'; break;   /* û */
        case 0xEE: key = 'i'; break;   /* î */
        case 0xF4: key = 'o'; break;   /* ô */
        case 0xB0: key = 'd'; break;   /* ° */
        case 0xA7: key = 's'; break;   /* § */
        case 0xA8: key = '"'; break;   /* ¨ */
    }
    return key;
}

static void keyboard_mark_key(uint8_t key, int pressed, int push_to_buffer) {
    if (key == KEY_NONE) return;
    if (pressed) {
        if (!key_active[key]) {
            key_active[key] = 1;
            last_key_down = key;
            if (push_to_buffer) buf_push(key);
        }
    } else {
        key_active[key] = 0;
        if (last_key_down == key) last_key_down = KEY_NONE;
    }
}

static void keyboard_process_scancode(uint8_t sc) {
    if (sc == 0xE0) {
        extended = 1;
        return;
    }

    {
        int released = (sc & 0x80) != 0;
        uint8_t code = sc & 0x7F;

        if (extended) {
            extended = 0;
            {
                uint8_t k = extended_keycode(code);
                if (released) {
                    uint8_t prev = key_from_ext[code];
                    if (prev != KEY_NONE) key_active[prev] = 0;
                    key_from_ext[code] = KEY_NONE;
                } else if (k != KEY_NONE) {
                    key_from_ext[code] = k;
                    key_active[k] = 1;
                    last_key_down = k;
                    buf_push(k);
                }
            }
            return;
        }

        switch (code) {
            case 0x2A: case 0x36:
                if (released) modifiers &= (uint8_t)~MOD_SHIFT; else modifiers |= MOD_SHIFT;
                return;
            case 0x1D:
                if (released) modifiers &= (uint8_t)~MOD_CTRL; else modifiers |= MOD_CTRL;
                return;
            case 0x38:
                if (released) modifiers &= (uint8_t)~MOD_ALT; else modifiers |= MOD_ALT;
                return;
            case 0x3A:
                if (!released) modifiers ^= MOD_CAPS;
                return;
            case 0x45:
                if (!released) modifiers ^= MOD_NUMLOCK;
                return;
        }

        if (code >= 128) return;

        if (released) {
            uint8_t prev = key_from_sc[code];
            if (prev != KEY_NONE) key_active[prev] = 0;
            key_from_sc[code] = KEY_NONE;
            return;
        }

        {
            uint8_t key = map_scancode_key(code);
            if (key != KEY_NONE) {
                key_from_sc[code] = key;
                key_active[key] = 1;
                last_key_down = key;
                buf_push(key);
            }
        }
    }
}

/* ─── IRQ1 handler ──────────────────────────────────────────────────────────── */
void keyboard_irq_handler(void) {
    uint8_t sc = 0;
    if (keyboard_read_ps2_scancode(&sc) > 0) {
        keyboard_process_scancode(sc);
    }
    outb(PIC1_CMD, PIC_EOI);
}

void keyboard_poll(void) {
    int guard = 64;
    while (guard-- > 0) {
        uint8_t sc = 0;
        int read = keyboard_read_ps2_scancode(&sc);
        if (read == 0) break;
        if (read > 0) keyboard_process_scancode(sc);
    }
}

/* ─── Init ──────────────────────────────────────────────────────────────────── */
void keyboard_init(void) {
    __asm__ volatile ("cli");

    ps2_controller_present = (inb(KBD_STATUS_PORT) != 0xFF);

    /* Vider le buffer PS/2 avec timeout borne, puis activer le clavier. */
    kbc_flush_output();
#if ALOS_HW_SAFE
    /*
     * En mode HW_SAFE on preserve l'emulation USB legacy du BIOS/UEFI:
     * certains firmwares exposent le clavier USB via le 8042 tant que l'OS
     * ne reprogramme pas le controleur PS/2.
     */
    if (ps2_controller_present) ps2_keyboard_enable_legacy();
    else ps2_keyboard_enable();
#else
    ps2_keyboard_enable();
#endif
    kbd_buf.head = kbd_buf.tail = kbd_buf.count = 0;
    modifiers = 0; extended = 0;
    for (int i = 0; i < 256; i++) key_active[i] = 0;
    for (int i = 0; i < 128; i++) { key_from_sc[i] = KEY_NONE; key_from_ext[i] = KEY_NONE; }
    last_key_down = KEY_NONE;
    g_idt_selector = read_code_segment_selector();

    /* Exceptions CPU 0-31 */
    idt_set_gate(0,(uint32_t)_exc0);   idt_set_gate(1,(uint32_t)_exc1);
    idt_set_gate(2,(uint32_t)_exc2);   idt_set_gate(3,(uint32_t)_exc3);
    idt_set_gate(4,(uint32_t)_exc4);   idt_set_gate(5,(uint32_t)_exc5);
    idt_set_gate(6,(uint32_t)_exc6);   idt_set_gate(7,(uint32_t)_exc7);
    idt_set_gate(8,(uint32_t)_exc8);   idt_set_gate(9,(uint32_t)_exc9);
    idt_set_gate(10,(uint32_t)_exc10); idt_set_gate(11,(uint32_t)_exc11);
    idt_set_gate(12,(uint32_t)_exc12); idt_set_gate(13,(uint32_t)_exc13);
    idt_set_gate(14,(uint32_t)_exc14); idt_set_gate(15,(uint32_t)_exc15);
    idt_set_gate(16,(uint32_t)_exc16); idt_set_gate(17,(uint32_t)_exc17);
    idt_set_gate(18,(uint32_t)_exc18); idt_set_gate(19,(uint32_t)_exc19);
    idt_set_gate(20,(uint32_t)_exc20); idt_set_gate(21,(uint32_t)_exc21);
    idt_set_gate(22,(uint32_t)_exc22); idt_set_gate(23,(uint32_t)_exc23);
    idt_set_gate(24,(uint32_t)_exc24); idt_set_gate(25,(uint32_t)_exc25);
    idt_set_gate(26,(uint32_t)_exc26); idt_set_gate(27,(uint32_t)_exc27);
    idt_set_gate(28,(uint32_t)_exc28); idt_set_gate(29,(uint32_t)_exc29);
    idt_set_gate(30,(uint32_t)_exc30); idt_set_gate(31,(uint32_t)_exc31);

    /* IRQ0 timer -> 32, IRQ1 clavier -> 33, IRQ12 souris -> 44 */
    idt_set_gate(32, (uint32_t)irq0_stub);
    idt_set_gate(33, (uint32_t)irq1_stub);
    idt_set_gate(44, (uint32_t)irq12_stub);

    /* INT 0x80 syscall → 48,  DPL=3 (accessible userland) */
    idt[48].offset_low  = (uint32_t)int80_stub & 0xFFFF;
    idt[48].selector    = g_idt_selector;
    idt[48].zero        = 0;
    idt[48].type_attr   = 0xEE;
    idt[48].offset_high = ((uint32_t)int80_stub >> 16) & 0xFFFF;

    pic_remap();

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;
    idt_load();

    __asm__ volatile ("sti");
}

/* ─── API publique ───────────────────────────────────────────────────────────── */
int     keyboard_available(void) {
#if ALOS_HW_SAFE
    usb_hid_kbd_poll();
#endif
    keyboard_poll();
    return kbd_buf.count > 0;
}
uint8_t keyboard_getkey(void) {
    while (!keyboard_available()) {
        __asm__ volatile("pause");
    }
    return buf_pop();
}
void    keyboard_clear_buffer(void) {
    kbd_buf.head = 0;
    kbd_buf.tail = 0;
    kbd_buf.count = 0;
    last_key_down = KEY_NONE;
}
uint8_t keyboard_modifiers(void) { return modifiers; }
int     keyboard_ps2_present(void) { return ps2_controller_present; }
uint8_t keyboard_current_key(void) {
    if (last_key_down != KEY_NONE && key_active[last_key_down]) return last_key_down;

    /* Priorite aux touches de mouvement */
    if (key_active[KEY_UP])    return KEY_UP;
    if (key_active[KEY_DOWN])  return KEY_DOWN;
    if (key_active[KEY_LEFT])  return KEY_LEFT;
    if (key_active[KEY_RIGHT]) return KEY_RIGHT;

    /* Puis scan general */
    for (int k = 1; k < 256; k++) if (key_active[k]) return (uint8_t)k;
    return KEY_NONE;
}

void keyboard_inject_press(uint8_t key) {
    keyboard_mark_key(key, 1, 1);
}

void keyboard_inject_release(uint8_t key) {
    keyboard_mark_key(key, 0, 0);
}

void keyboard_inject_tap(uint8_t key) {
    keyboard_mark_key(key, 1, 1);
    keyboard_mark_key(key, 0, 0);
}

void keyboard_set_external_modifiers(uint8_t mods) {
    uint8_t preserved = (uint8_t)(modifiers & (uint8_t)~(MOD_SHIFT | MOD_CTRL | MOD_ALT));
    modifiers = (uint8_t)(preserved | (mods & (MOD_SHIFT | MOD_CTRL | MOD_ALT)));
}
