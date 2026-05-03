[BITS 16]
[ORG 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov si, msg_boot
    call print16

    in al, 0x92
    or al, 2
    out 0x92, al

    mov ah, 0x02
    mov al, 30
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov bx, 0x1000
    int 0x13
    jc disk_error

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:protected_mode

print16:
    mov ah, 0x0E
.p:
    lodsb
    test al, al
    jz .r
    int 0x10
    jmp .p
.r:
    ret

gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF,0
    db 0,10011010b,11001111b,0
gdt_data:
    dw 0xFFFF,0
    db 0,10010010b,11001111b,0
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

[BITS 32]
protected_mode:
    cli
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x90000

    mov esi, msg_protected
    mov edi, 0xB8000
    mov ah, 0x0A
.p32:
    lodsb
    test al, al
    jz .k
    mov [edi], ax
    add edi, 2
    jmp .p32
.k:
    mov eax, 0x1000
    jmp eax

msg_boot db "[ALOS] Boot...",13,10,0
msg_protected db "[ALOS] PM OK - Kernel...",0
msg_error db "Disk error",13,10,0

disk_error:
    mov si, msg_error
    call print16
    jmp $

times 510-($-$$) db 0
dw 0xAA55