OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS {
    . = 0x100000;

    .multiboot ALIGN(4)  : { *(.multiboot) }
    .multiboot2 ALIGN(8) : { *(.multiboot2) }
    .text      ALIGN(4)  : { *(.text*)    }
    .rodata    ALIGN(4)  : { *(.rodata*)  }
    .data      ALIGN(4)  : { *(.data*)    }
    .bss       ALIGN(4)  : { *(COMMON) *(.bss*) }

    /* Symbole kernel_end : adresse immédiatement après le kernel */
    kernel_end = .;
}
