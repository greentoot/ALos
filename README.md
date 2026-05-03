# ALOS v0.5 - README technique complet

## 1) Vision du projet

ALOS est un mini système d'exploitation x86 (32 bits, mode protégé) orienté:

- apprentissage bas niveau (boot, interruptions, mémoire, scheduler)
- shell interactif
- VM Hack/Nand2Tetris intégrée
- exécution de jeux Jack embarqués (ex: `jackpokemonV`)

Le projet est freestanding (pas de libc système), compilé en ELF i386 et lancé principalement sous QEMU/VirtualBox.

## 2) Fonctionnalités actuelles

- Boot Multiboot via GRUB (ISO/disque bootable)
- Noyau 32-bit, IDT, exceptions CPU, IRQ timer+clavier
- Scheduler préemptif simple (100 Hz)
- PMM bitmap + heap kernel statique
- RamFS hiérarchique Linux-like (`/bin`, `/home`, `/usr`, ...)
- VFS minimal (`stdin/stdout/stderr` + RamFS)
- Shell `kshell` avec commandes fichiers/process/système
- Driver clavier PS/2 AZERTY
- Driver texte VGA + driver graphique (mode Hack 512x256 rendu adaptatif)
- Interpréteur Hack VM (Nand2Tetris) avec OS natif C (`Math`, `Screen`, `Keyboard`, ...)
- Support multi-jeux VM embarqués (`tools/embed_vm.py`)
- Éditeur ASM, exécution ASM, éditeur Jack et compilateur Jack->VM intégrés
- Système de sauvegarde runtime VM (F5/F9)
- Persistance disque (snapshot RamFS mutable + sauvegardes GBA)

## 3) Démarrage rapide

### 3.1 Build ELF + run QEMU

```bash
make all
make run
```

`make run` attache automatiquement `alos_persist.img` (partition type `0xA0`).
Toutes les modifications RamFS (fichiers/dossiers édités) et les sauvegardes GBA
sont restaurées au redémarrage.

Pour repartir de zéro:

```bash
make persist-reset
```

### 3.2 Build ISO bootable GRUB

```bash
make iso
make run-iso
```

### 3.3 Build image disque bootable GRUB

```bash
make disk
make run-disk
```

### 3.4 Mode strict bare-metal

Refuse le boot si hyperviseur détecté:

```bash
make strict
```

### 3.5 Embarquer des jeux VM (mono ou multi-jeux)

```bash
python tools/embed_vm.py ./jackpokemonV
python tools/embed_vm.py Pokemon=./jackpokemonV Guess=../games/JackGuess
make run
```

Dans le shell:

```text
vmls
jack Pokemon
```

## 4) Arborescence globale

```text
Iaos/
├─ Makefile
├─ aios.bin
├─ aios.elf
├─ complet_list
├─ kernel.c
├─ kernel_entry.asm
├─ linker.ld
├─ README.md
├─ bootloader/
│  ├─ boot.asm
│  └─ boot.bin
├─ driver/
│  ├─ gfx.c
│  ├─ gfx.h
│  ├─ idt_stubs.asm
│  ├─ keyboard.c
│  ├─ keyboard.h
│  ├─ timer.c
│  ├─ timer.h
│  ├─ vga.c
│  └─ vga.h
├─ grub/
│  └─ grub.cfg
├─ kernel/
│  ├─ kernel.bin
│  ├─ shell.c
│  ├─ shell.h
│  ├─ tty.c
│  ├─ tty.h
│  ├─ boot/
│  │  ├─ bootinfo.c
│  │  └─ bootinfo.h
│  ├─ exec/
│  │  ├─ asm_exec.c
│  │  ├─ asm_exec.h
│  │  ├─ jackc.c
│  │  └─ jackc.h
│  ├─ fs/
│  │  ├─ ramfs.c
│  │  ├─ ramfs.h
│  │  ├─ vfs.c
│  │  └─ vfs.h
│  ├─ jack/
│  │  ├─ font5x8.h
│  │  ├─ jack_data.c
│  │  ├─ jack_data.h
│  │  ├─ jack_mount.c
│  │  ├─ vm_interp.c
│  │  ├─ vm_interp.h
│  │  ├─ vm_store.c
│  │  └─ vm_store.h
│  ├─ lib/
│  │  ├─ kprintf.c
│  │  ├─ kprintf.h
│  │  ├─ string.c
│  │  └─ string.h
│  ├─ memory/
│  │  ├─ heap.c
│  │  ├─ heap.h
│  │  ├─ pmm.c
│  │  └─ pmm.h
│  ├─ process/
│  │  ├─ scheduler.c
│  │  ├─ scheduler.h
│  │  ├─ switch_ctx.asm
│  │  ├─ task.c
│  │  └─ task.h
│  └─ syscall/
│     ├─ syscall.c
│     └─ syscall.h
├─ tools/
│  ├─ build_disk_image.sh
│  ├─ build_iso.sh
│  ├─ build_persist_image.sh
│  └─ embed_vm.py
└─ jackpokemonV/
   ├─ Battle.jack / Battle.vm
   ├─ Game.jack / Game.vm
   ├─ Inventory.jack / Inventory.vm
   ├─ Item.jack / Item.vm
   ├─ ItemMenu.jack / ItemMenu.vm
   ├─ ItemPickup.jack / ItemPickup.vm
   ├─ ItemPickupManager.jack / ItemPickupManager.vm
   ├─ Main.jack / Main.vm
   ├─ Map.jack / Map.vm
   ├─ MapManager.jack / MapManager.vm
   ├─ Money.jack / Money.vm
   ├─ Move.jack / Move.vm
   ├─ Player.jack / Player.vm
   ├─ Pnj.jack / Pnj.vm
   ├─ PnjManager.jack / PnjManager.vm
   ├─ Pokedex.jack / Pokedex.vm
   ├─ Pokemon.jack / Pokemon.vm
   ├─ Random.jack / Random.vm
   ├─ Shop.jack / Shop.vm
   ├─ SpawnZone.jack / SpawnZone.vm
   ├─ Sprites.jack / Sprites.vm
   ├─ StarterMenu.jack / StarterMenu.vm
   ├─ Strings.jack / Strings.vm
   ├─ Team.jack / Team.vm
   ├─ TeamMenu.jack / TeamMenu.vm
   └─ TransitionEffect.jack / TransitionEffect.vm
```

## 5) Détail fichier par fichier

## 5.1 Racine

| Fichier | Rôle |
|---|---|
| `Makefile` | Build central: compile ELF, run QEMU, créer ISO/disque bootables, mode strict bare-metal, pipeline `embed`. |
| `aios.bin` | Ancien binaire raw de boot (artefact historique). |
| `aios.elf` | Ancien ELF noyau (artefact historique). |
| `complet_list` | Liste TODO globale (paging, ELF loader, drivers disque, user/kernel, réseau...). |
| `kernel.c` | Entrée C du noyau: parse bootinfo, init mémoire, FS, scheduler, timer, clavier, shell. |
| `kernel_entry.asm` | Point d’entrée Multiboot `_start`, setup segments/stack/GDT, jump `kernel_main`. |
| `linker.ld` | Script de link ELF i386 (placement sections + symbole `kernel_end`). |
| `README.md` | Documentation technique (ce fichier). |

## 5.2 `bootloader/`

| Fichier | Rôle |
|---|---|
| `bootloader/boot.asm` | Bootloader BIOS 16-bit legacy: lecture disque fixe + passage mode protégé + jump kernel (voie historique, non principale). |
| `bootloader/boot.bin` | Binaire 512B boot sector généré depuis `boot.asm`. |

## 5.3 `driver/`

| Fichier | Rôle |
|---|---|
| `driver/gfx.h` | API rendu graphique Hack (capture/restauration texte, mode graphique, blit écran 1bpp). |
| `driver/gfx.c` | Driver graphique VGA/VBE: modes banked, viewport centré, scaling x1/x2/x3, flush écran Hack 512x256. |
| `driver/idt_stubs.asm` | Stubs ASM exceptions/IRQ/INT80 + trampoline context switch. |
| `driver/keyboard.h` | API clavier + keycodes spéciaux + modificateurs + buffer ring. |
| `driver/keyboard.c` | Driver PS/2 AZERTY: scancodes, états touches, IRQ1, IDT/PIC setup, INT 0x80 gate. |
| `driver/timer.h` | API timer PIT (ticks/ms/IRQ). |
| `driver/timer.c` | Driver PIT 8253 (100 Hz). |
| `driver/ata.h` | API ATA PIO (presence, identify, read/write secteurs). |
| `driver/ata.c` | Driver ATA primary master (LBA28, lecture/écriture secteur). |
| `driver/vga.h` | API mode texte VGA 80x25 (put char/print/scroll/clear). |
| `driver/vga.c` | Implémentation VGA texte avec shadow buffer pour limiter writes VRAM. |

## 5.4 `grub/`

| Fichier | Rôle |
|---|---|
| `grub/grub.cfg` | Configuration GRUB Multiboot (`multiboot /boot/alos.elf`). |

## 5.5 `kernel/` (niveau principal)

| Fichier | Rôle |
|---|---|
| `kernel/kernel.bin` | Artefact binaire noyau brut (historique/compat). |
| `kernel/shell.h` | Interface du shell principal. |
| `kernel/shell.c` | Shell interactif: commandes fichiers, process, VM, Jack, ASM; boucle CLI; bannière boot. |
| `kernel/tty.h` | API terminal utilisateur (zone scroll + ligne commande). |
| `kernel/tty.c` | TTY plein écran style Linux: prompt, saisie, scroll, séparateur, couleurs. |

## 5.6 `kernel/boot/`

| Fichier | Rôle |
|---|---|
| `kernel/boot/bootinfo.h` | Structure `BootInfo` (mémoire, framebuffer, hyperviseur). |
| `kernel/boot/bootinfo.c` | Parse multiboot v1 + probe CPUID hyperviseur + expose état plateforme. |

## 5.7 `kernel/exec/`

| Fichier | Rôle |
|---|---|
| `kernel/exec/asm_exec.h` | API micro-assembleur i386 embarqué. |
| `kernel/exec/asm_exec.c` | Assembler+launcher de code ASM (labels, jumps, création tâche). |
| `kernel/exec/jackc.h` | API compilateur Jack -> VM. |
| `kernel/exec/jackc.c` | Compilateur Jack minimal (tokenizer, parser, symbols, génération VM). |

## 5.8 `kernel/fs/`

| Fichier | Rôle |
|---|---|
| `kernel/fs/ramfs.h` | API RamFS hiérarchique, types nœuds, codes erreur, résolution chemin. |
| `kernel/fs/ramfs.c` | Implémentation RamFS: chemins absolus/relatifs, dossiers, fichiers RO/RW, listing, suppression. |
| `kernel/fs/persist.h` | API persistance (`persist_init`, `persist_flush`, `persist_mark_dirty`). |
| `kernel/fs/persist.c` | Snapshot disque: sérialisation RamFS mutable + saves GBA, CRC32, restore boot. |
| `kernel/fs/vfs.h` | Interface VFS + table fd. |
| `kernel/fs/vfs.c` | VFS minimal: `stdin/stdout/stderr`, open/read/write/close sur RamFS. |

## 5.9 `kernel/jack/`

| Fichier | Rôle |
|---|---|
| `kernel/jack/font5x8.h` | Font bitmap 5x8 utilisée pour rendu texte en mode graphique Hack. |
| `kernel/jack/jack_data.h` | Déclarations des tables VM embarquées (`JackDataEntry`). |
| `kernel/jack/jack_data.c` | Grosse table générée `.rodata` contenant fichiers VM embarqués et manifests `.vmdir`. |
| `kernel/jack/jack_mount.c` | Montage du store VM embarqué au boot (`jack_data_table -> vm_store`). |
| `kernel/jack/vm_store.h` | API registre des fichiers `.vm` embarqués. |
| `kernel/jack/vm_store.c` | Store VM en mémoire (add/find/list/stats). |
| `kernel/jack/vm_interp.h` | Structures/opcodes/limites VM Hack. |
| `kernel/jack/vm_interp.c` | Cœur interpréteur VM Hack: parser VM, exécution, OS natif C, rendu, clavier, save/load runtime. |

## 5.10 `kernel/lib/`

| Fichier | Rôle |
|---|---|
| `kernel/lib/string.h` | API utilitaires mémoire/strings/conversions. |
| `kernel/lib/string.c` | Implémentation `kmemcpy`, `kstrcmp`, `katoi`, `kitoa`, etc. |
| `kernel/lib/kprintf.h` | API `kprintf`/`ksprintf`. |
| `kernel/lib/kprintf.c` | Formatage simple (`%d %u %x %s %c`) vers TTY ou buffer. |

## 5.11 `kernel/memory/`

| Fichier | Rôle |
|---|---|
| `kernel/memory/pmm.h` | API PMM pages physiques 4K. |
| `kernel/memory/pmm.c` | PMM bitmap + stratégie next-fit. |
| `kernel/memory/heap.h` | API heap kernel + stats fragmentation. |
| `kernel/memory/heap.c` | Heap statique 384KiB, blocs chaînés, split/coalesce. |

## 5.12 `kernel/process/`

| Fichier | Rôle |
|---|---|
| `kernel/process/task.h` | Structures tâches, états, API create/kill/sleep. |
| `kernel/process/task.c` | Gestion table tâches + init stack initiale + sleep coopératif. |
| `kernel/process/scheduler.h` | API scheduler tick. |
| `kernel/process/scheduler.c` | Round-robin préemptif simple + réveil des sleepers. |
| `kernel/process/switch_ctx.asm` | Bascule de contexte ESP via `scheduler_tick`. |

## 5.13 `kernel/syscall/`

| Fichier | Rôle |
|---|---|
| `kernel/syscall/syscall.h` | Numéros syscall et handler INT 0x80. |
| `kernel/syscall/syscall.c` | Dispatch syscall (`write`, `exit`, `sleep`, `open`, `read`, etc.). |

## 5.14 `tools/`

| Fichier | Rôle |
|---|---|
| `tools/embed_vm.py` | Générateur des assets VM embarqués (mono/multi-jeux, namespacing, manifests `.vmdir`). |
| `tools/build_iso.sh` | Construction ISO GRUB bootable (`grub-mkrescue`). |
| `tools/build_disk_image.sh` | Construction image disque bootable BIOS+GRUB (`dd/parted/losetup/grub-install`). |

## 5.15 `jackpokemonV/` (sources jeu)

Notes:

- Chaque `*.jack` est un module source.
- Chaque `*.vm` est son bytecode compilé (Nand2Tetris VM) utilisé pour l’exécution.

### 5.15.1 Logique gameplay

| Fichier | Rôle |
|---|---|
| `jackpokemonV/Main.jack` | Point d’entrée Jack, lance le jeu. |
| `jackpokemonV/Main.vm` | Bytecode compilé de `Main.jack`. |
| `jackpokemonV/Game.jack` | Boucle principale, déplacement, interactions, transitions, déclenchement combats/menu. |
| `jackpokemonV/Game.vm` | Bytecode compilé de `Game.jack`. |
| `jackpokemonV/Battle.jack` | Système de combat (UI, dégâts, switch Pokémon, rendu sprites combat). |
| `jackpokemonV/Battle.vm` | Bytecode compilé de `Battle.jack`. |
| `jackpokemonV/TransitionEffect.jack` | Effets visuels (iris, flash, transition combat). |
| `jackpokemonV/TransitionEffect.vm` | Bytecode compilé de `TransitionEffect.jack`. |
| `jackpokemonV/Random.jack` | RNG utilitaire (`next`, `randRange`, `chance`). |
| `jackpokemonV/Random.vm` | Bytecode compilé de `Random.jack`. |
| `jackpokemonV/SpawnZone.jack` | Tables/spawns Pokémon sauvages selon map/terrain. |
| `jackpokemonV/SpawnZone.vm` | Bytecode compilé de `SpawnZone.jack`. |

### 5.15.2 Entités et données Pokémon

| Fichier | Rôle |
|---|---|
| `jackpokemonV/Pokemon.jack` | Modèle Pokémon (stats, moves, HP, XP-like logique locale). |
| `jackpokemonV/Pokemon.vm` | Bytecode compilé de `Pokemon.jack`. |
| `jackpokemonV/Move.jack` | Modèle attaque/capacité (id/power/type). |
| `jackpokemonV/Move.vm` | Bytecode compilé de `Move.jack`. |
| `jackpokemonV/Team.jack` | Gestion équipe joueur (slots, actif, heal, checks vivants). |
| `jackpokemonV/Team.vm` | Bytecode compilé de `Team.jack`. |
| `jackpokemonV/Pokedex.jack` | État Pokédex capturé + affichage liste/noms. |
| `jackpokemonV/Pokedex.vm` | Bytecode compilé de `Pokedex.jack`. |

### 5.15.3 Joueur, PNJ, monde

| Fichier | Rôle |
|---|---|
| `jackpokemonV/Player.jack` | État joueur (position, direction, équipe, argent, progression). |
| `jackpokemonV/Player.vm` | Bytecode compilé de `Player.jack`. |
| `jackpokemonV/Pnj.jack` | Entité PNJ/dresseur (dialogues, équipe trainer, interactions). |
| `jackpokemonV/Pnj.vm` | Bytecode compilé de `Pnj.jack`. |
| `jackpokemonV/PnjManager.jack` | Gestion collection PNJ, collisions/interactions/champ de vision. |
| `jackpokemonV/PnjManager.vm` | Bytecode compilé de `PnjManager.jack`. |
| `jackpokemonV/Map.jack` | Définition des maps, tiles, portails, collisions et layouts. |
| `jackpokemonV/Map.vm` | Bytecode compilé de `Map.jack`. |
| `jackpokemonV/MapManager.jack` | Map courante + switch map + draw map active. |
| `jackpokemonV/MapManager.vm` | Bytecode compilé de `MapManager.jack`. |

### 5.15.4 Inventaire, économie, shop

| Fichier | Rôle |
|---|---|
| `jackpokemonV/Item.jack` | Modèle item + quantité. |
| `jackpokemonV/Item.vm` | Bytecode compilé de `Item.jack`. |
| `jackpokemonV/Inventory.jack` | Inventaire principal + add/use/display. |
| `jackpokemonV/Inventory.vm` | Bytecode compilé de `Inventory.jack`. |
| `jackpokemonV/ItemMenu.jack` | UI menu objets + cible Pokémon + heal. |
| `jackpokemonV/ItemMenu.vm` | Bytecode compilé de `ItemMenu.jack`. |
| `jackpokemonV/Money.jack` | Modèle argent (gain/dépense/canAfford). |
| `jackpokemonV/Money.vm` | Bytecode compilé de `Money.jack`. |
| `jackpokemonV/Shop.jack` | UI boutique + achat d’objets. |
| `jackpokemonV/Shop.vm` | Bytecode compilé de `Shop.jack`. |
| `jackpokemonV/ItemPickup.jack` | Objet ramassable sur map (position/type/qty/map). |
| `jackpokemonV/ItemPickup.vm` | Bytecode compilé de `ItemPickup.jack`. |
| `jackpokemonV/ItemPickupManager.jack` | Gestion globale des pickups par map. |
| `jackpokemonV/ItemPickupManager.vm` | Bytecode compilé de `ItemPickupManager.jack`. |

### 5.15.5 UI/menus/affichage

| Fichier | Rôle |
|---|---|
| `jackpokemonV/StarterMenu.jack` | Sélection starter et UI associée. |
| `jackpokemonV/StarterMenu.vm` | Bytecode compilé de `StarterMenu.jack`. |
| `jackpokemonV/TeamMenu.jack` | UI équipe détaillée (slots, HP bars, sélection). |
| `jackpokemonV/TeamMenu.vm` | Bytecode compilé de `TeamMenu.jack`. |
| `jackpokemonV/Sprites.jack` | Fonctions de dessin sprite/tile (très volumineux). |
| `jackpokemonV/Sprites.vm` | Bytecode compilé de `Sprites.jack`. |
| `jackpokemonV/Strings.jack` | Ressources texte massives (dialogues/noms/chaînes). |
| `jackpokemonV/Strings.vm` | Bytecode compilé de `Strings.jack`. |

## 6) Flux d’exécution (du boot au jeu)

1. `kernel_entry.asm`:
- setup stack + GDT
- appelle `kernel_main`

2. `kernel.c`:
- parse multiboot (`bootinfo_parse`)
- configure résolution préférée (si framebuffer connu)
- capture mode texte (`gfx_capture_text_mode`)
- init mémoire (`pmm_init`, `heap_init`)
- init FS (`ramfs_init`, `jack_data_mount`, `vfs_init`)
- init tâches/scheduler/timer/clavier
- lance `shell_run`

3. `shell.c`:
- boucle CLI
- commande `jack` -> charge VM files -> `vm_run`

4. `vm_interp.c`:
- parse bytecode `.vm`
- bootstrap `Sys.init`
- dispatch OS natif C (`Screen.*`, `Output.*`, `Keyboard.*`, `Math.*`, etc.)
- rendu écran Hack via `driver/gfx.c`

5. sortie VM:
- retour forcé texte (`gfx_restore_text_mode`, `vga_init`, `tty_init`)
- retour shell

## 7) Commandes shell disponibles

### 7.1 Navigation/FS

- `pwd`
- `cd [dir]`
- `ls [dir]`
- `mkdir <dir>`
- `touch <fichier>`
- `cat <fichier>`
- `write <fichier> <texte>`
- `rm <path>`

### 7.2 ASM intégré

- `run <f.asm>`: assemble+exécute
- `asm [f.asm]`: éditeur interactif ASM

### 7.3 Jack/VM

- `jackedit [f.jack]`: éditeur Jack interactif
- `jackc <in.jack> [out.vm]`: compilation Jack->VM
- `vmls`: liste jeux/manifests/fichiers VM
- `jack <jeu>` ou `jack <f1.vm> [f2.vm ...]`: exécution VM

### 7.4 Process/système

- `ps`
- `kill <pid>`
- `free`
- `heap`
- `uname`
- `uptime`
- `clear`
- `echo ...`
- `newtask [nom]`
- `help`

## 8) Graphiques et adaptation écran

Le rendu VM Hack est 512x256 1bpp. Le driver:

- tente une résolution VBE adaptée (préférence bootloader puis fallbacks)
- choisit automatiquement un facteur d’échelle `x1/x2/x3`
- centre le viewport Hack dans la surface active
- utilise des dirty ranges de mots écran pour limiter le coût de blit

Conséquence:

- l’OS exploite mieux les fenêtres QEMU/VM de tailles variées
- le rendu reste pixel-perfect côté contenu Hack

## 9) Système de sauvegarde VM intégré

Dans la VM runtime:

- `F5`: capture état (position, map, équipe, inventaire, pokédex, pickups, flags scénario)
- `F9`: réapplique l’état sur les objets runtime
- `ESC`: sortie VM propre et retour shell

Le système est en RAM uniquement (pas persistance disque réelle pour le moment).

## 10) Points vulnérables / risques techniques

## 10.1 Sécurité / isolation

Risque élevé:

- Pas de séparation user/kernel réelle: tout tourne en ring0.
- `asm_exec` exécute du code machine natif injecté.
- INT 0x80 exposé sans sandbox mémoire.
- Absence de paging/MMU: corruption mémoire globale possible.

Fichiers clés:

- `kernel/exec/asm_exec.c`
- `kernel/syscall/syscall.c`
- `driver/idt_stubs.asm`
- `kernel/process/*`

## 10.2 Robustesse mémoire

Risque élevé:

- PMM prend un plafond mémoire global sans exploiter finement les zones réservées mmap.
- Heap kernel simple, sans canary/guard pages.
- VM RAM manipule des adresses directes avec checks partiels.
- `Memory.peek/poke` VM peut écrire en dehors de régions logiques sûres si mal utilisé.

Fichiers clés:

- `kernel/memory/pmm.c`
- `kernel/memory/heap.c`
- `kernel/jack/vm_interp.c`

## 10.3 Concurrence / scheduling

Risque moyen:

- Scheduler très simple, sans priorité ni accounting.
- `task_sleep` en boucle `hlt` coopérative.
- Pas de verrous noyau, risque data races si futures extensions multipoints.

Fichiers clés:

- `kernel/process/scheduler.c`
- `kernel/process/task.c`

## 10.4 FS / persistance

Risque moyen:

- RamFS volatile uniquement.
- Pas de permissions, pas d’UID/GID, pas de liens, pas de journalisation.
- Taille fixe nœuds/données.

Fichiers clés:

- `kernel/fs/ramfs.c`
- `kernel/fs/vfs.c`

## 10.5 Entrées clavier / internationalisation

Risque faible à moyen:

- Mapping AZERTY spécifique, cas spéciaux partiels.
- Codes accentués/étendus gérés de façon approximative dans certains chemins.

Fichiers clés:

- `driver/keyboard.c`
- `kernel/jack/vm_interp.c` (`key_to_hack`, fallback scancode)

## 10.6 Build/ops

Risque faible à moyen:

- Scripts image disque demandent root et opèrent des loop devices.
- Dépendances externes (grub, xorriso, parted, etc.) non encapsulées.

Fichiers clés:

- `tools/build_iso.sh`
- `tools/build_disk_image.sh`

## 11) Ce qu’il faut améliorer (roadmap priorisée)

## 11.1 Priorité P0 (fondations)

- Ajouter paging + séparation kernel/user.
- Sécuriser `asm_exec` (mode bac à sable ou retrait en prod).
- Renforcer vérifications bornes VM stack/segments/adresses.
- Exploiter proprement la carte mémoire multiboot (zones disponibles uniquement).

## 11.2 Priorité P1 (OS “normal”)

- VFS unifié plus riche (permissions, métadonnées, dirs robustes).
- Système de fichiers persistant (ext2-like simplifié ou FAT minimal).
- Drivers disque réels (au moins ATA PIO, puis AHCI).
- Process model plus propre (fork/exec-lite, wait, kill robuste).

## 11.3 Priorité P2 (ergonomie/perf)

- Double buffering graphique VM + invalidation plus fine.
- Profiling VM opcodes hot path.
- Historique shell, édition ligne avancée, completion.
- Logs kernel circulaires + niveaux de verbosité.

## 11.4 Priorité P3 (long terme)

- ELF loader userland.
- Réseau minimal (driver + stack léger).
- IPC basique.
- Suite tests automatique kernel+VM.

## 12) Plan de tests recommandé

## 12.1 Build/boot

- `make all`
- `make run`
- `make iso && make run-iso`
- `make disk && make run-disk`

## 12.2 Shell/FS

- `pwd`, `ls`, `mkdir`, `touch`, `write`, `cat`, `rm`, `cd`
- vérification erreurs (read-only, dossier non vide, chemins invalides)

## 12.3 Processus

- `newtask`, `ps`, `kill`, `uptime`

## 12.4 Jack/VM

- `vmls`
- `jack <jeu>`
- test `F5/F9/ESC`
- test retour shell après sortie VM

## 12.5 Graphique

- ouvrir plusieurs tailles de fenêtre VM
- vérifier centrage, scale auto, retour texte correct

## 13) Notes de maintenance

## 13.1 Fichiers générés automatiquement

Ne pas éditer à la main:

- `kernel/jack/jack_data.c`
- `kernel/jack/jack_data.h`
- `kernel/jack/jack_mount.c`

Ils sont régénérés par `tools/embed_vm.py`.

## 13.2 Artefacts historiques

Présence d’artefacts anciens (`aios.*`, `bootloader/boot.bin`, `kernel/kernel.bin`) conservés pour compat/debug.  
Prévoir un nettoyage structuré si vous figez une release.

## 13.3 Compatibilité

Plateforme cible actuelle:

- x86 32-bit
- BIOS/GRUB Multiboot
- QEMU/VirtualBox

Pas de support UEFI natif à ce stade.

## 14) FAQ rapide

### Puis-je embarquer plusieurs jeux VM?

Oui. `embed_vm.py` supporte multi-jeux avec namespacing:

```bash
python tools/embed_vm.py Pokemon=./jackpokemonV Guess=../games/JackGuess
```

Puis:

```text
vmls
jack Pokemon
jack Guess
```

### Le jeu est sauvegardé sur disque?

Non, actuellement c’est une sauvegarde runtime en RAM (F5/F9) pendant la session.

---