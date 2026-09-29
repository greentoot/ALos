/**
 * kernel/gdt.h - Table de descripteurs globaux (GDT) + TSS minimal.
 *
 * Jusqu'a cette session, la GDT etait du code mort (le noyau tournait sur
 * les segments plats fournis par GRUB via multiboot2, voir kernel_entry.asm
 * ou le chargement de GDT est gate derriere ALOS_HW_SAFE toujours defini).
 * Ce module charge une vraie GDT avec des segments ring 3, prerequis pour
 * lancer des taches en mode utilisateur (voir kernel/process/task.c).
 *
 * IMPORTANT - ordre d'initialisation : gdt_init() DOIT etre appele avant
 * keyboard_init() (driver/keyboard.c) dans kernel_main(). keyboard_init()
 * capture le %cs courant pour construire toutes les portes de l'IDT ; si la
 * GDT est chargee apres coup, ces portes referenceraient un selecteur
 * perime. En chargeant notre propre GDT en premier avec le code noyau au
 * meme selecteur (0x08) que ce que GRUB fournit deja, tout le reste du code
 * existant (task.c, idt gates) reste valide sans modification.
 */
#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <stdint.h>

/* Selecteurs (indice GDT * 8). Ajouter | 3 (RPL=3) pour un usage ring 3. */
#define GDT_SEL_KCODE 0x08
#define GDT_SEL_KDATA 0x10
#define GDT_SEL_UCODE 0x18   /* utiliser GDT_SEL_UCODE|3 = 0x1B en ring 3 */
#define GDT_SEL_UDATA 0x20   /* utiliser GDT_SEL_UDATA|3 = 0x23 en ring 3 */
#define GDT_SEL_TSS   0x28

/* Construit et charge la GDT + le TSS (lgdt puis ltr). A appeler une seule
 * fois, tres tot dans kernel_main(), avant keyboard_init(). */
void gdt_init(void);

/* Met a jour esp0 dans le TSS : la pile que le CPU utilisera automatiquement
 * si une tache ring 3 est interrompue (IRQ/exception) ou fait un syscall
 * (int 0x80). Doit etre appelee a chaque changement de contexte vers une
 * tache (voir kernel/process/scheduler.c), avec le sommet de la pile noyau
 * de la tache elue. Sans ca, une interruption survenant en ring 3 utilise
 * une pile noyau perimee -> corruption memoire quasi certaine. */
void tss_set_kernel_stack(uint32_t esp0);

#endif
