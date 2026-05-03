[BITS 32]
global _start
extern kernel_main

MULTIBOOT_MAGIC    equ 0x1BADB002
MULTIBOOT_FLAGS    equ 0x00000003
MULTIBOOT_CHECKSUM equ -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

MULTIBOOT2_MAGIC    equ 0xE85250D6
MULTIBOOT2_BOOTLOADER_MAGIC equ 0x36D76289
MULTIBOOT2_ARCH     equ 0
MULTIBOOT2_HEADER_LEN equ (mb2_header_end - mb2_header_start)
MULTIBOOT2_CHECKSUM equ -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_HEADER_LEN)

section .multiboot
align 4
    dd MULTIBOOT_MAGIC
    dd MULTIBOOT_FLAGS
    dd MULTIBOOT_CHECKSUM

section .multiboot2
align 8
mb2_header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd MULTIBOOT2_HEADER_LEN
    dd MULTIBOOT2_CHECKSUM

    dw 5                  ; framebuffer tag
    dw 0                  ; optional
    dd 20                 ; size
    dd 0                  ; width auto
    dd 0                  ; height auto
    dd 0                  ; depth auto
    dd 0                  ; padding to keep next tag 8-byte aligned

    dw 0                  ; end tag
    dw 0
    dd 8
mb2_header_end:

section .bss
align 16
stack_bottom:
    resb 262144          ; 32 Ko stack kernel (augmenté vs original)
stack_top:

section .text
_start:
    cli
    cmp eax, MULTIBOOT2_BOOTLOADER_MAGIC
    jne .skip_early_stamp
    push dword 0
    push ebx
    call early_mb2_stamp_band
    add esp, 8
.skip_early_stamp:
    mov esp, stack_top
    cmp eax, MULTIBOOT2_BOOTLOADER_MAGIC
    jne .skip_stack_stamp
    push dword 32
    push ebx
    call early_mb2_stamp_band
    add esp, 8
.skip_stack_stamp:
%ifndef ALOS_HW_SAFE
    lgdt [gdt_descriptor]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush
%endif
.flush:
    cmp eax, MULTIBOOT2_BOOTLOADER_MAGIC
    jne .skip_gdt_stamp
    push dword 64
    push ebx
    call early_mb2_stamp_band
    add esp, 8
.skip_gdt_stamp:
    cmp eax, MULTIBOOT2_BOOTLOADER_MAGIC
    jne .skip_call_stamp
    push dword 96
    push ebx
    call early_mb2_stamp_band
    add esp, 8
.skip_call_stamp:
    push ebx                ; multiboot_info_addr
    push eax                ; multiboot_magic
    call kernel_main
    add esp, 8
.hang:
    cli
    hlt
    jmp .hang

; Marqueur ultra-precoce:
; si GRUB nous a bien transmis un framebuffer Multiboot2 exploitable en 32-bit,
; on remplit les premieres lignes en blanc pour distinguer "kernel lance mais
; diag C non visible" de "aucun handoff video exploitable".
early_mb2_stamp_band:
    push ebp
    mov ebp, esp
    pushad

    mov esi, [ebp + 8]         ; mb2 info addr
    test esi, esi
    jz .done

    mov ecx, [esi]             ; total_size
    cmp ecx, 16
    jb .done
    mov edx, 8                 ; offset premier tag

.tag_loop:
    cmp edx, ecx
    jae .done
    lea eax, [esi + edx]
    mov ebx, [eax + 4]         ; tag size
    cmp ebx, 8
    jb .done
    mov edi, [eax]             ; tag type
    cmp edi, 0
    je .done
    cmp edi, 8                 ; framebuffer tag
    je .fb_tag
    add ebx, 7
    and ebx, 0FFFFFFF8h
    add edx, ebx
    jmp .tag_loop

.fb_tag:
    cmp dword [eax + 12], 0    ; high 32 bits of framebuffer_addr
    jne .done
    mov edi, [eax + 8]         ; low 32 bits of framebuffer_addr
    test edi, edi
    jz .done

    cmp byte [eax + 29], 1     ; RGB framebuffer
    je .stamp_rgb
    cmp byte [eax + 29], 2     ; text framebuffer
    je .stamp_text
    jmp .done

.stamp_rgb:
    mov ecx, [eax + 16]        ; pitch
    test ecx, ecx
    jz .done
    mov ebx, [ebp + 12]        ; y offset in scanlines
    imul ebx, ecx
    add edi, ebx
    shl ecx, 4                 ; 16 scanlines blanches
    mov al, 0FFh
    rep stosb
    jmp .done

.stamp_text:
    mov ebx, [ebp + 12]
    shr ebx, 4                 ; approx row index
    imul ebx, ebx, 160         ; 80 cols * 2 bytes
    add edi, ebx
    mov word [edi + 0],  0x0F41 ; A
    mov word [edi + 2],  0x0F4C ; L
    mov word [edi + 4],  0x0F4F ; O
    mov word [edi + 6],  0x0F53 ; S
    mov word [edi + 8],  0x0F20 ; ' '
    mov word [edi + 10], 0x0F42 ; B
    mov word [edi + 12], 0x0F41 ; A
    mov word [edi + 14], 0x0F4E ; N
    mov word [edi + 16], 0x0F44 ; D

.done:
    popad
    pop ebp
    ret

section .data
align 8
gdt_start:
    dq 0x0000000000000000           ; null descriptor
    dw 0xFFFF, 0x0000               ; code segment
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000               ; data segment
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

section .note.GNU-stack noalloc noexec nowrite progbits

