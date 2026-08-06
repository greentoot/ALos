# ALOS

ALOS est un noyau x86 (i386) écrit from scratch en C et assembleur, bootable via GRUB (Multiboot2) et testable sous QEMU. C'est un projet de type "hobby OS" : il n'y a pas d'espace utilisateur isolé, tout tourne en ring 0.

Ce dossier contient la version épurée du noyau, sans émulateurs, sans ROMs, sans BIOS DS/GBA et sans contenu de jeu embarqué.

## Fonctionnalités actuelles

- **Boot** : chargement Multiboot2 via GRUB, parsing des infos de boot, détection de framebuffer, diagnostics de démarrage. Un mode "bare-metal strict" peut refuser de démarrer si un hyperviseur est détecté ; une variante "hw-safe" existe en parallèle de l'ISO standard.
- **Mémoire** : gestionnaire de mémoire physique par bitmap (`pmm`, jusqu'à 1 Gio) et un tas noyau (`heap`, 8 Mio) avec `kmalloc`/`kfree` et statistiques d'usage.
- **Processus** : table de tâches (8 max), ordonnanceur à tick avec sommeil/réveil (`task_sleep`), changement de contexte en assembleur.
- **Fichiers** : système de fichiers en RAM (`ramfs`, 256 nœuds max) exposé via une couche VFS (fd-based, `open`/`read`/`write`/`close`).
- **Appels système** : gérés via `int 0x80` (write, exit, getpid, sleep, open, read).
- **Mini-exécuteur ASM** : un assembleur/interpréteur x86 simplifié embarqué (`asmexec_run`) capable d'assembler un sous-ensemble d'instructions i386 (mov, add, jmp, cmp, appels système, etc.) et de lancer le résultat comme une tâche.
- **Shell type Unix** : `help`, `clear`, `uname`, `uptime`, `mem`, `ps`, `usb`, `pwd`, `cd`, `ls`/`dir`, `cat`, `mkdir`, `rm`, `touch`, `echo`, opérant sur le ramfs.
- **Pilotes** : VGA texte/graphique, couche graphique générique, port série, clavier et souris PS/2, timer (PIT), disque ATA, audio, USB (probe, contrôleur xHCI, clavier HID).
- **Installeur** : module capable d'écrire une table de partitions MBR sur une cible ATA, utilisable comme module GRUB séparé (`ALOS Installer`).

## Stade d'avancement

Projet en développement actif, pas encore prêt pour un usage réel. Fonctionnalités connues comme manquantes ou incomplètes (roadmap, voir `complet_list`) :

- Pas de pagination (memory management encore en adressage physique direct).
- Pas de véritable système de fichiers persistant (le ramfs est volatile, perdu au reboot).
- Pas de chargeur ELF (seul le mini-exécuteur ASM permet de lancer du code dynamiquement).
- Pas de séparation mode utilisateur / noyau (tout s'exécute en ring 0).
- Pas de pile réseau.
- Pas d'optimisations SIMD.

## Construire et lancer

Fichier ISO généré : `alos-linux-like.iso` (variante graphique/complète) et `alos-hw-safe.iso` (variante prudente pour matériel réel).

Régénérer l'ISO depuis WSL ou Linux :

```bash
cd <chemin-vers-ce-dossier>
make iso-linux-like
```

Lancer avec QEMU :

```bash
qemu-system-i386 -m 512 -vga std -cdrom alos-linux-like.iso -boot d
```
