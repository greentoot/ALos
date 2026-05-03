; kernel/process/switch_ctx.asm
[BITS 32]
global do_context_switch
extern scheduler_tick

section .text
; Appelé depuis irq0_stub après pusha.
; À cet instant esp pointe vers le frame complet pusha+iret.
; On passe cet esp à scheduler_tick, qui retourne le nouvel esp.
do_context_switch:
    push    esp             ; arg : esp courant (frame pusha+iret)
    call    scheduler_tick
    add     esp, 4          ; nettoyer l'argument
    ; eax = nouvel esp (peut être identique si 1 seule tâche)
    mov     esp, eax        ; basculer vers le stack de la tâche choisie
    ret                     ; retourne dans irq0_stub → popa + iret

section .note.GNU-stack noalloc noexec nowrite progbits
