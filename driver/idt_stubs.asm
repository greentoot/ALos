[BITS 32]

; Fonctions C
extern cpu_exception_handler
extern keyboard_irq_handler
extern mouse_irq_handler
extern timer_irq_handler
extern syscall_handler
extern do_context_switch

; Exports exceptions 0-31
global _exc0,  _exc1,  _exc2,  _exc3,  _exc4,  _exc5,  _exc6,  _exc7
global _exc8,  _exc9,  _exc10, _exc11, _exc12, _exc13, _exc14, _exc15
global _exc16, _exc17, _exc18, _exc19, _exc20, _exc21, _exc22, _exc23
global _exc24, _exc25, _exc26, _exc27, _exc28, _exc29, _exc30, _exc31
global irq0_stub, irq1_stub, irq12_stub, int80_stub
global task_resume_point
global idt_load
extern idt_ptr

section .text

; ─── Macros exceptions ─────────────────────────────────────────────────────
%macro STUB_NOERR 1
_exc%1:
    pusha
    push dword %1
    call cpu_exception_handler
    add esp, 4
    popa
    iret
%endmacro

%macro STUB_ERR 1
_exc%1:
    add esp, 4      ; retire l'error code CPU
    pusha
    push dword %1
    call cpu_exception_handler
    add esp, 4
    popa
    iret
%endmacro

STUB_NOERR  0   ; #DE Divide Error
STUB_NOERR  1   ; #DB Debug
STUB_NOERR  2   ;     NMI
STUB_NOERR  3   ; #BP Breakpoint
STUB_NOERR  4   ; #OF Overflow
STUB_NOERR  5   ; #BR Bound Range
STUB_NOERR  6   ; #UD Invalid Opcode
STUB_NOERR  7   ; #NM Device Not Available
STUB_ERR    8   ; #DF Double Fault
STUB_NOERR  9   ;     Coproc Segment Overrun
STUB_ERR    10  ; #TS Invalid TSS
STUB_ERR    11  ; #NP Segment Not Present
STUB_ERR    12  ; #SS Stack Fault
STUB_ERR    13  ; #GP General Protection
STUB_ERR    14  ; #PF Page Fault
STUB_NOERR  15
STUB_NOERR  16  ; #MF x87 FPU
STUB_ERR    17  ; #AC Alignment Check
STUB_NOERR  18  ; #MC Machine Check
STUB_NOERR  19  ; #XF SIMD
STUB_NOERR  20
STUB_NOERR  21
STUB_NOERR  22
STUB_NOERR  23
STUB_NOERR  24
STUB_NOERR  25
STUB_NOERR  26
STUB_NOERR  27
STUB_NOERR  28
STUB_NOERR  29
STUB_NOERR  30
STUB_NOERR  31

; ─── IRQ0 : timer + context switch ─────────────────────────────────────────
irq0_stub:
    pusha
    ; Appeler le driver timer (incrémente _ticks)
    call timer_irq_handler
    ; EOI PIC maître
    mov al, 0x20
    out 0x20, al
    ; Context switch : passe esp courant, récupère le nouveau
    call do_context_switch
    ; do_context_switch a déjà changé esp (mov esp,eax ; ret) : le "ret"
    ; saute ici, que la tâche choisie soit déjà en cours (retour normal
    ; depuis une IRQ précédente) ou toute neuve. Une tâche neuve doit donc
    ; avoir, tout en haut de sa pile initiale (voir task.c:setup_stack),
    ; l'adresse de CETTE etiquette — sinon le "ret" saute dans le vide.
task_resume_point:
    popa
    iret

; ─── IRQ1 : clavier ────────────────────────────────────────────────────────
irq1_stub:
    pusha
    call keyboard_irq_handler
    popa
    iret

irq12_stub:
    pusha
    call mouse_irq_handler
    popa
    iret

; ─── INT 0x80 : syscall ────────────────────────────────────────────────────
; Convention : eax=num, ebx=arg1, ecx=arg2, edx=arg3
; Retour dans eax
int80_stub:
    pusha
    push edx        ; arg3
    push ecx        ; arg2
    push ebx        ; arg1
    push eax        ; num
    call syscall_handler
    add esp, 16
    ; Mettre le résultat dans eax du frame pusha (offset +28 depuis esp)
    mov [esp+28], eax
    popa
    iret

; ─── idt_load ──────────────────────────────────────────────────────────────
idt_load:
    lidt [idt_ptr]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
