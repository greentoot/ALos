/* kernel/exec/jackc.c - Minimal Jack -> VM compiler (Nand2Tetris style) */
#include "jackc.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include <stdint.h>

#define JACKC_MAX_TOKEN   96
#define JACKC_MAX_NAME    64
#define JACKC_MAX_TYPE    64
#define JACKC_MAX_SYMS    256

enum {
    TK_EOF = 0,
    TK_IDENT,
    TK_INT,
    TK_STR,
    TK_SYM,
    TK_KW
};

enum {
    SYM_STATIC = 1,
    SYM_FIELD  = 2,
    SYM_ARG    = 3,
    SYM_VAR    = 4
};

enum {
    SUB_FUNCTION = 0,
    SUB_CONSTRUCTOR = 1,
    SUB_METHOD = 2
};

typedef struct {
    int  kind;
    char text[JACKC_MAX_TOKEN];
    int  ival;
    char sym;
    int  line;
    int  col;
} Token;

typedef struct {
    char     name[JACKC_MAX_NAME];
    char     type[JACKC_MAX_TYPE];
    uint8_t  kind;
    uint16_t index;
    uint8_t  used;
} JackSym;

typedef struct {
    const char *src_name;
    const char *src;
    uint32_t    len;
    uint32_t    pos;
    int         line;
    int         col;
    Token       tok;

    char       *out;
    uint32_t    out_cap;
    uint32_t    out_len;

    char       *errbuf;
    uint32_t    errcap;
    int         failed;

    char     cur_class[JACKC_MAX_NAME];
    int      label_id;
    int      cur_sub_kind;

    JackSym   class_syms[JACKC_MAX_SYMS];
    JackSym   sub_syms[JACKC_MAX_SYMS];
    uint16_t  nclass;
    uint16_t  nsub;
    uint16_t  c_static;
    uint16_t  c_field;
    uint16_t  s_arg;
    uint16_t  s_var;
} JackC;

static int is_alpha(char c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_');
}

static int is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static int is_alnum_(char c) {
    return is_alpha(c) || is_digit(c);
}

static int is_symbol(char c) {
    const char *s = "{}()[].,;+-*/&|<>=~";
    while (*s) {
        if (*s == c) return 1;
        s++;
    }
    return 0;
}

static int is_keyword(const char *s) {
    static const char *kw[] = {
        "class","constructor","function","method","field","static","var",
        "int","char","boolean","void","true","false","null","this",
        "let","do","if","else","while","return", 0
    };
    for (int i = 0; kw[i]; i++) if (kstrcmp(s, kw[i]) == 0) return 1;
    return 0;
}

static void set_error_raw(JackC *c, const char *msg, int line, int col) {
    if (c->failed) return;
    c->failed = 1;
    if (c->errbuf && c->errcap) {
        ksprintf(c->errbuf, "%s:%d:%d: %s", c->src_name ? c->src_name : "<jack>", line, col, msg);
    }
}

static void set_error(JackC *c, const char *msg) {
    int l = c->tok.line ? c->tok.line : c->line;
    int p = c->tok.col  ? c->tok.col  : c->col;
    set_error_raw(c, msg, l, p);
}

static int emit_line(JackC *c, const char *line) {
    uint32_t n = kstrlen(line);
    if (c->failed) return -1;
    if (c->out_len + n + 2 >= c->out_cap) {
        set_error(c, "Sortie VM trop grande");
        return -1;
    }
    kmemcpy(c->out + c->out_len, line, n);
    c->out_len += n;
    c->out[c->out_len++] = '\n';
    c->out[c->out_len] = '\0';
    return 0;
}

static int emit_push(JackC *c, const char *seg, int idx) {
    char b[96];
    ksprintf(b, "push %s %d", seg, idx);
    return emit_line(c, b);
}

static int emit_pop(JackC *c, const char *seg, int idx) {
    char b[96];
    ksprintf(b, "pop %s %d", seg, idx);
    return emit_line(c, b);
}

static int emit_call(JackC *c, const char *name, int nargs) {
    char b[128];
    ksprintf(b, "call %s %d", name, nargs);
    return emit_line(c, b);
}

static int emit_label(JackC *c, const char *name) {
    char b[128];
    ksprintf(b, "label %s", name);
    return emit_line(c, b);
}

static int emit_goto(JackC *c, const char *name) {
    char b[128];
    ksprintf(b, "goto %s", name);
    return emit_line(c, b);
}

static int emit_if_goto(JackC *c, const char *name) {
    char b[128];
    ksprintf(b, "if-goto %s", name);
    return emit_line(c, b);
}

static void next_char(JackC *c) {
    if (c->pos >= c->len) return;
    if (c->src[c->pos] == '\n') {
        c->line++;
        c->col = 1;
    } else {
        c->col++;
    }
    c->pos++;
}

static char cur_char(JackC *c) {
    if (c->pos >= c->len) return '\0';
    return c->src[c->pos];
}

static char peek_char(JackC *c) {
    if (c->pos + 1 >= c->len) return '\0';
    return c->src[c->pos + 1];
}

static void skip_ws_comments(JackC *c) {
    while (c->pos < c->len && !c->failed) {
        char ch = cur_char(c);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            next_char(c);
            continue;
        }
        if (ch == '/' && peek_char(c) == '/') {
            while (c->pos < c->len && cur_char(c) != '\n') next_char(c);
            continue;
        }
        if (ch == '/' && peek_char(c) == '*') {
            next_char(c); next_char(c);
            while (c->pos < c->len) {
                if (cur_char(c) == '*' && peek_char(c) == '/') {
                    next_char(c); next_char(c);
                    break;
                }
                next_char(c);
            }
            continue;
        }
        break;
    }
}

static Token next_token(JackC *c) {
    Token t;
    kmemset(&t, 0, sizeof(t));
    t.kind = TK_EOF;
    t.line = c->line;
    t.col  = c->col;

    skip_ws_comments(c);
    if (c->failed || c->pos >= c->len) {
        t.kind = TK_EOF;
        t.line = c->line;
        t.col  = c->col;
        return t;
    }

    char ch = cur_char(c);
    t.line = c->line;
    t.col  = c->col;

    if (is_symbol(ch)) {
        t.kind = TK_SYM;
        t.sym = ch;
        t.text[0] = ch;
        t.text[1] = '\0';
        next_char(c);
        return t;
    }

    if (is_digit(ch)) {
        int v = 0;
        t.kind = TK_INT;
        while (is_digit(cur_char(c))) {
            v = (v * 10) + (cur_char(c) - '0');
            next_char(c);
        }
        t.ival = v;
        ksprintf(t.text, "%d", v);
        return t;
    }

    if (ch == '"') {
        uint32_t n = 0;
        t.kind = TK_STR;
        next_char(c); /* " */
        while (c->pos < c->len && cur_char(c) != '"' && !c->failed) {
            char k = cur_char(c);
            if (n + 1 < sizeof(t.text)) t.text[n++] = k;
            next_char(c);
        }
        t.text[n] = '\0';
        if (cur_char(c) != '"') {
            set_error_raw(c, "String non terminee", t.line, t.col);
            return t;
        }
        next_char(c); /* closing " */
        return t;
    }

    if (is_alpha(ch)) {
        uint32_t n = 0;
        t.kind = TK_IDENT;
        while (is_alnum_(cur_char(c))) {
            if (n + 1 < sizeof(t.text)) t.text[n++] = cur_char(c);
            next_char(c);
        }
        t.text[n] = '\0';
        if (is_keyword(t.text)) t.kind = TK_KW;
        return t;
    }

    set_error_raw(c, "Caractere invalide dans source Jack", t.line, t.col);
    return t;
}

static void advance(JackC *c) {
    if (c->failed) return;
    c->tok = next_token(c);
}

static int tok_is_kw(JackC *c, const char *s) {
    return c->tok.kind == TK_KW && kstrcmp(c->tok.text, s) == 0;
}

static int tok_is_sym(JackC *c, char s) {
    return c->tok.kind == TK_SYM && c->tok.sym == s;
}

static int tok_is_ident(JackC *c) {
    return c->tok.kind == TK_IDENT;
}

static void expect_sym(JackC *c, char s) {
    char m[80];
    if (c->failed) return;
    if (!tok_is_sym(c, s)) {
        ksprintf(m, "Symbole attendu: '%c'", s);
        set_error(c, m);
        return;
    }
    advance(c);
}

static void expect_kw(JackC *c, const char *kw) {
    char m[96];
    if (c->failed) return;
    if (!tok_is_kw(c, kw)) {
        ksprintf(m, "Mot-cle attendu: %s", kw);
        set_error(c, m);
        return;
    }
    advance(c);
}

static void expect_ident(JackC *c, char *out, uint32_t outsz) {
    if (c->failed) return;
    if (!tok_is_ident(c)) {
        set_error(c, "Identifiant attendu");
        return;
    }
    if (out && outsz) {
        kstrncpy(out, c->tok.text, outsz - 1);
        out[outsz - 1] = '\0';
    }
    advance(c);
}

static int parse_type(JackC *c, char *out, uint32_t outsz, int allow_void) {
    if (c->failed) return -1;
    if (c->tok.kind == TK_IDENT ||
        tok_is_kw(c, "int") || tok_is_kw(c, "char") || tok_is_kw(c, "boolean") ||
        (allow_void && tok_is_kw(c, "void"))) {
        if (out && outsz) {
            kstrncpy(out, c->tok.text, outsz - 1);
            out[outsz - 1] = '\0';
        }
        advance(c);
        return 0;
    }
    set_error(c, "Type attendu");
    return -1;
}

static void sub_symbols_reset(JackC *c) {
    kmemset(c->sub_syms, 0, sizeof(c->sub_syms));
    c->nsub = 0;
    c->s_arg = 0;
    c->s_var = 0;
}

static int sym_define_class(JackC *c, uint8_t kind, const char *type, const char *name) {
    JackSym *s;
    if (c->nclass >= JACKC_MAX_SYMS) {
        set_error(c, "Table de symboles classe pleine");
        return -1;
    }
    s = &c->class_syms[c->nclass++];
    kmemset(s, 0, sizeof(*s));
    s->used = 1;
    s->kind = kind;
    kstrncpy(s->name, name, JACKC_MAX_NAME - 1);
    kstrncpy(s->type, type, JACKC_MAX_TYPE - 1);
    if (kind == SYM_STATIC) s->index = c->c_static++;
    else s->index = c->c_field++;
    return 0;
}

static int sym_define_sub(JackC *c, uint8_t kind, const char *type, const char *name) {
    JackSym *s;
    if (c->nsub >= JACKC_MAX_SYMS) {
        set_error(c, "Table de symboles subroutine pleine");
        return -1;
    }
    s = &c->sub_syms[c->nsub++];
    kmemset(s, 0, sizeof(*s));
    s->used = 1;
    s->kind = kind;
    kstrncpy(s->name, name, JACKC_MAX_NAME - 1);
    kstrncpy(s->type, type, JACKC_MAX_TYPE - 1);
    if (kind == SYM_ARG) s->index = c->s_arg++;
    else s->index = c->s_var++;
    return 0;
}

static JackSym *sym_find(JackC *c, const char *name) {
    for (int i = (int)c->nsub - 1; i >= 0; i--) {
        if (c->sub_syms[i].used && kstrcmp(c->sub_syms[i].name, name) == 0) return &c->sub_syms[i];
    }
    for (int i = (int)c->nclass - 1; i >= 0; i--) {
        if (c->class_syms[i].used && kstrcmp(c->class_syms[i].name, name) == 0) return &c->class_syms[i];
    }
    return 0;
}

static const char *seg_of_kind(uint8_t kind) {
    if (kind == SYM_STATIC) return "static";
    if (kind == SYM_FIELD)  return "this";
    if (kind == SYM_ARG)    return "argument";
    if (kind == SYM_VAR)    return "local";
    return 0;
}

static void make_label(JackC *c, const char *prefix, char *out, uint32_t outsz) {
    ksprintf(out, "%s%d", prefix, c->label_id++);
    if (outsz) out[outsz - 1] = '\0';
}

static void compile_expression(JackC *c);
static void compile_statements(JackC *c);

static void emit_binary_op(JackC *c, char op) {
    switch (op) {
        case '+': emit_line(c, "add"); break;
        case '-': emit_line(c, "sub"); break;
        case '&': emit_line(c, "and"); break;
        case '|': emit_line(c, "or"); break;
        case '<': emit_line(c, "lt"); break;
        case '>': emit_line(c, "gt"); break;
        case '=': emit_line(c, "eq"); break;
        case '*': emit_call(c, "Math.multiply", 2); break;
        case '/': emit_call(c, "Math.divide", 2); break;
        default: set_error(c, "Operateur binaire non supporte"); break;
    }
}

static int compile_expression_list(JackC *c) {
    int nargs = 0;
    if (tok_is_sym(c, ')')) return 0;
    while (!c->failed) {
        compile_expression(c);
        nargs++;
        if (tok_is_sym(c, ',')) {
            advance(c);
            continue;
        }
        break;
    }
    return nargs;
}

static void compile_subcall_after_first(JackC *c, const char *first) {
    char subname[JACKC_MAX_NAME];
    char callname[2 * JACKC_MAX_NAME + 2];
    int nargs = 0;

    if (tok_is_sym(c, '(')) {
        /* method on current object: Class.first(...) */
        advance(c); /* ( */
        emit_push(c, "pointer", 0);
        nargs = compile_expression_list(c) + 1;
        expect_sym(c, ')');
        ksprintf(callname, "%s.%s", c->cur_class, first);
        emit_call(c, callname, nargs);
        return;
    }

    if (tok_is_sym(c, '.')) {
        JackSym *obj = 0;
        advance(c); /* . */
        expect_ident(c, subname, sizeof(subname));
        expect_sym(c, '(');
        obj = sym_find(c, first);
        if (obj) {
            emit_push(c, seg_of_kind(obj->kind), obj->index);
            nargs = compile_expression_list(c) + 1;
            ksprintf(callname, "%s.%s", obj->type, subname);
        } else {
            nargs = compile_expression_list(c);
            ksprintf(callname, "%s.%s", first, subname);
        }
        expect_sym(c, ')');
        emit_call(c, callname, nargs);
        return;
    }

    set_error(c, "Appel de subroutine invalide");
}

static void compile_term(JackC *c) {
    if (c->failed) return;

    if (c->tok.kind == TK_INT) {
        emit_push(c, "constant", c->tok.ival);
        advance(c);
        return;
    }

    if (c->tok.kind == TK_STR) {
        uint32_t n = kstrlen(c->tok.text);
        emit_push(c, "constant", (int)n);
        emit_call(c, "String.new", 1);
        for (uint32_t i = 0; i < n; i++) {
            emit_push(c, "constant", (unsigned char)c->tok.text[i]);
            emit_call(c, "String.appendChar", 2);
        }
        advance(c);
        return;
    }

    if (c->tok.kind == TK_KW) {
        if (kstrcmp(c->tok.text, "true") == 0) {
            emit_push(c, "constant", 0);
            emit_line(c, "not");
            advance(c);
            return;
        }
        if (kstrcmp(c->tok.text, "false") == 0 || kstrcmp(c->tok.text, "null") == 0) {
            emit_push(c, "constant", 0);
            advance(c);
            return;
        }
        if (kstrcmp(c->tok.text, "this") == 0) {
            emit_push(c, "pointer", 0);
            advance(c);
            return;
        }
    }

    if (tok_is_sym(c, '(')) {
        advance(c);
        compile_expression(c);
        expect_sym(c, ')');
        return;
    }

    if (tok_is_sym(c, '-') || tok_is_sym(c, '~')) {
        char op = c->tok.sym;
        advance(c);
        compile_term(c);
        if (op == '-') emit_line(c, "neg");
        else emit_line(c, "not");
        return;
    }

    if (tok_is_ident(c)) {
        char name[JACKC_MAX_NAME];
        JackSym *s = 0;
        kstrncpy(name, c->tok.text, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        advance(c);

        if (tok_is_sym(c, '[')) {
            s = sym_find(c, name);
            if (!s) { set_error(c, "Variable de tableau inconnue"); return; }
            emit_push(c, seg_of_kind(s->kind), s->index);
            advance(c); /* [ */
            compile_expression(c);
            expect_sym(c, ']');
            emit_line(c, "add");
            emit_pop(c, "pointer", 1);
            emit_push(c, "that", 0);
            return;
        }

        if (tok_is_sym(c, '(') || tok_is_sym(c, '.')) {
            compile_subcall_after_first(c, name);
            return;
        }

        s = sym_find(c, name);
        if (!s) { set_error(c, "Variable inconnue"); return; }
        emit_push(c, seg_of_kind(s->kind), s->index);
        return;
    }

    set_error(c, "Term Jack invalide");
}

static int is_expr_op(JackC *c) {
    if (c->tok.kind != TK_SYM) return 0;
    return c->tok.sym == '+' || c->tok.sym == '-' || c->tok.sym == '*' ||
           c->tok.sym == '/' || c->tok.sym == '&' || c->tok.sym == '|' ||
           c->tok.sym == '<' || c->tok.sym == '>' || c->tok.sym == '=';
}

static void compile_expression(JackC *c) {
    compile_term(c);
    while (!c->failed && is_expr_op(c)) {
        char op = c->tok.sym;
        advance(c);
        compile_term(c);
        emit_binary_op(c, op);
    }
}

static void compile_let(JackC *c) {
    char var[JACKC_MAX_NAME];
    JackSym *s = 0;
    int is_array = 0;

    expect_kw(c, "let");
    expect_ident(c, var, sizeof(var));
    s = sym_find(c, var);
    if (!s) { set_error(c, "Variable let inconnue"); return; }

    if (tok_is_sym(c, '[')) {
        is_array = 1;
        emit_push(c, seg_of_kind(s->kind), s->index);
        advance(c);
        compile_expression(c);
        expect_sym(c, ']');
        emit_line(c, "add");
    }

    expect_sym(c, '=');
    compile_expression(c);
    expect_sym(c, ';');

    if (is_array) {
        emit_pop(c, "temp", 0);
        emit_pop(c, "pointer", 1);
        emit_push(c, "temp", 0);
        emit_pop(c, "that", 0);
    } else {
        emit_pop(c, seg_of_kind(s->kind), s->index);
    }
}

static void compile_do(JackC *c) {
    char first[JACKC_MAX_NAME];
    expect_kw(c, "do");
    expect_ident(c, first, sizeof(first));
    compile_subcall_after_first(c, first);
    expect_sym(c, ';');
    emit_pop(c, "temp", 0);
}

static void compile_return(JackC *c) {
    expect_kw(c, "return");
    if (tok_is_sym(c, ';')) {
        emit_push(c, "constant", 0);
        advance(c);
    } else {
        compile_expression(c);
        expect_sym(c, ';');
    }
    emit_line(c, "return");
}

static void compile_while(JackC *c) {
    char l_exp[64], l_end[64];
    make_label(c, "WHILE_EXP_", l_exp, sizeof(l_exp));
    make_label(c, "WHILE_END_", l_end, sizeof(l_end));

    expect_kw(c, "while");
    emit_label(c, l_exp);
    expect_sym(c, '(');
    compile_expression(c);
    expect_sym(c, ')');
    emit_line(c, "not");
    emit_if_goto(c, l_end);
    expect_sym(c, '{');
    compile_statements(c);
    expect_sym(c, '}');
    emit_goto(c, l_exp);
    emit_label(c, l_end);
}

static void compile_if(JackC *c) {
    char l_true[64], l_false[64], l_end[64];
    int has_else = 0;
    make_label(c, "IF_TRUE_", l_true, sizeof(l_true));
    make_label(c, "IF_FALSE_", l_false, sizeof(l_false));
    make_label(c, "IF_END_", l_end, sizeof(l_end));

    expect_kw(c, "if");
    expect_sym(c, '(');
    compile_expression(c);
    expect_sym(c, ')');
    emit_if_goto(c, l_true);
    emit_goto(c, l_false);
    emit_label(c, l_true);
    expect_sym(c, '{');
    compile_statements(c);
    expect_sym(c, '}');
    if (tok_is_kw(c, "else")) {
        has_else = 1;
        emit_goto(c, l_end);
    }
    emit_label(c, l_false);
    if (has_else) {
        advance(c); /* else */
        expect_sym(c, '{');
        compile_statements(c);
        expect_sym(c, '}');
        emit_label(c, l_end);
    }
}

static void compile_statements(JackC *c) {
    while (!c->failed) {
        if (tok_is_kw(c, "let"))       { compile_let(c); continue; }
        if (tok_is_kw(c, "if"))        { compile_if(c); continue; }
        if (tok_is_kw(c, "while"))     { compile_while(c); continue; }
        if (tok_is_kw(c, "do"))        { compile_do(c); continue; }
        if (tok_is_kw(c, "return"))    { compile_return(c); continue; }
        break;
    }
}

static void compile_class_var_dec(JackC *c) {
    uint8_t kind = tok_is_kw(c, "static") ? SYM_STATIC : SYM_FIELD;
    char type[JACKC_MAX_TYPE];
    char name[JACKC_MAX_NAME];

    advance(c); /* static|field */
    parse_type(c, type, sizeof(type), 0);
    expect_ident(c, name, sizeof(name));
    sym_define_class(c, kind, type, name);
    while (tok_is_sym(c, ',')) {
        advance(c);
        expect_ident(c, name, sizeof(name));
        sym_define_class(c, kind, type, name);
    }
    expect_sym(c, ';');
}

static void compile_var_dec(JackC *c) {
    char type[JACKC_MAX_TYPE];
    char name[JACKC_MAX_NAME];
    expect_kw(c, "var");
    parse_type(c, type, sizeof(type), 0);
    expect_ident(c, name, sizeof(name));
    sym_define_sub(c, SYM_VAR, type, name);
    while (tok_is_sym(c, ',')) {
        advance(c);
        expect_ident(c, name, sizeof(name));
        sym_define_sub(c, SYM_VAR, type, name);
    }
    expect_sym(c, ';');
}

static void compile_parameter_list(JackC *c) {
    char type[JACKC_MAX_TYPE];
    char name[JACKC_MAX_NAME];
    if (tok_is_sym(c, ')')) return;
    while (!c->failed) {
        parse_type(c, type, sizeof(type), 0);
        expect_ident(c, name, sizeof(name));
        sym_define_sub(c, SYM_ARG, type, name);
        if (tok_is_sym(c, ',')) { advance(c); continue; }
        break;
    }
}

static void compile_subroutine(JackC *c) {
    char ret_type[JACKC_MAX_TYPE];
    char sub_name[JACKC_MAX_NAME];
    char fn_name[2 * JACKC_MAX_NAME + 2];
    char b[128];

    if (tok_is_kw(c, "constructor")) c->cur_sub_kind = SUB_CONSTRUCTOR;
    else if (tok_is_kw(c, "method")) c->cur_sub_kind = SUB_METHOD;
    else c->cur_sub_kind = SUB_FUNCTION;
    advance(c); /* constructor|function|method */

    parse_type(c, ret_type, sizeof(ret_type), 1);
    (void)ret_type;
    expect_ident(c, sub_name, sizeof(sub_name));

    sub_symbols_reset(c);
    if (c->cur_sub_kind == SUB_METHOD) {
        sym_define_sub(c, SYM_ARG, c->cur_class, "this");
    }

    expect_sym(c, '(');
    compile_parameter_list(c);
    expect_sym(c, ')');
    expect_sym(c, '{');

    while (tok_is_kw(c, "var")) {
        compile_var_dec(c);
    }

    ksprintf(fn_name, "%s.%s", c->cur_class, sub_name);
    ksprintf(b, "function %s %u", fn_name, (unsigned)c->s_var);
    emit_line(c, b);

    if (c->cur_sub_kind == SUB_CONSTRUCTOR) {
        emit_push(c, "constant", c->c_field);
        emit_call(c, "Memory.alloc", 1);
        emit_pop(c, "pointer", 0);
    } else if (c->cur_sub_kind == SUB_METHOD) {
        emit_push(c, "argument", 0);
        emit_pop(c, "pointer", 0);
    }

    compile_statements(c);
    expect_sym(c, '}');
}

static void compile_class(JackC *c) {
    char class_name[JACKC_MAX_NAME];

    expect_kw(c, "class");
    expect_ident(c, class_name, sizeof(class_name));
    kstrncpy(c->cur_class, class_name, sizeof(c->cur_class) - 1);
    c->cur_class[sizeof(c->cur_class) - 1] = '\0';

    expect_sym(c, '{');

    while (tok_is_kw(c, "static") || tok_is_kw(c, "field")) {
        compile_class_var_dec(c);
    }
    while (tok_is_kw(c, "constructor") || tok_is_kw(c, "function") || tok_is_kw(c, "method")) {
        compile_subroutine(c);
    }

    expect_sym(c, '}');
    if (!c->failed && c->tok.kind != TK_EOF) {
        set_error(c, "Tokens en trop apres la classe");
    }
}

int jackc_compile(const char *src_name,
                  const char *src_code,
                  char *out_vm,
                  uint32_t out_cap,
                  uint32_t *out_len,
                  char *errbuf,
                  uint32_t errcap) {
    JackC c;
    if (!src_code || !out_vm || out_cap < 2) return -1;

    kmemset(&c, 0, sizeof(c));
    c.src_name = src_name ? src_name : "<jack>";
    c.src = src_code;
    c.len = kstrlen(src_code);
    c.pos = 0;
    c.line = 1;
    c.col = 1;
    c.out = out_vm;
    c.out_cap = out_cap;
    c.out_len = 0;
    c.errbuf = errbuf;
    c.errcap = errcap;
    c.failed = 0;
    c.label_id = 0;
    c.out[0] = '\0';
    if (errbuf && errcap) errbuf[0] = '\0';

    kmemset(c.class_syms, 0, sizeof(c.class_syms));
    c.nclass = 0;
    c.c_field = 0;
    c.c_static = 0;

    advance(&c);
    compile_class(&c);

    if (c.failed) return -1;

    if (out_len) *out_len = c.out_len;
    return 0;
}
