# ALOS

ALOS est un noyau x86 (i386) écrit from scratch en C et assembleur, bootable via GRUB (Multiboot2) et testable sous QEMU. C'est un projet de type "hobby OS" : il n'y a pas encore d'espace utilisateur isolé, tout tourne en ring 0.

Ce dossier contient la version épurée du noyau, sans émulateurs, sans ROMs, sans BIOS DS/GBA et sans contenu de jeu embarqué.

## Fonctionnalités actuelles

- **Boot** : chargement Multiboot2 via GRUB, parsing des infos de boot, détection de framebuffer, diagnostics de démarrage. Un mode "bare-metal strict" peut refuser de démarrer si un hyperviseur est détecté ; une variante "hw-safe" existe en parallèle de l'ISO standard.
- **Mémoire** : gestionnaire de mémoire physique par bitmap (`pmm`, jusqu'à 1 Gio), pagination identité (PSE, pages 4 Mo, `kernel/memory/paging.c`) activée au boot juste après le `pmm`, et un tas noyau (`heap`, 8 Mio) avec `kmalloc`/`kfree` et statistiques d'usage.
- **Processus** : table de tâches (8 max), ordonnanceur préemptif à tick (round-robin, réveil sur `task_sleep`), changement de contexte en assembleur (`switch_ctx.asm` + trampoline `task_resume_point` dans `idt_stubs.asm`, voir Correctifs ci-dessous).
- **Fichiers** : système de fichiers en RAM (`ramfs`, 256 nœuds max, volatile) exposé via une couche VFS (fd-based, `open`/`read`/`write`/`close`), **plus un système de fichiers persistant** monté sur `/mnt` (voir section dédiée ci-dessous).
- **Appels système** : gérés via `int 0x80` (write, exit, getpid, sleep, open, read).
- **Mini-exécuteur ASM** : un assembleur/interpréteur x86 simplifié embarqué (`kernel/exec/asm_exec.c`, `asmexec_run`) capable d'assembler un sous-ensemble d'instructions i386 (mov, add, sub, xor, and, or, cmp, imul, jmp/je/jne/jl/jg/jle/jge, call/ret, push/pop, int, nop, hlt, cli, sti, inc/dec/not/neg) et de lancer le résultat comme une tâche préemptible.
- **Chargeur ELF32** (`kernel/exec/elf_loader.c`) : charge et lance un exécutable ELF32 statique (i386, `ET_EXEC`) comme une tâche préemptible — voir section dédiée ci-dessous.
- **Shell type Unix** (`kernel/shell_linux_like.c`) : `help`, `clear`, `uname`, `uptime`, `mem`, `ps`, `usb`, `pwd`, `cd`, `ls`/`dir`, `cat`, `mkdir`, `rm`, `touch`, `echo`, `asm` (mode de saisie multi-lignes pour le micro-assembleur), `elfrun`/`elfmod` (ring 0) et `elfrun3`/`elfmod3` (ring 3, voir section dédiée) pour le chargeur ELF, `net info`/`net ping <ip>` (pile réseau minimale, voir section dédiée).
- **Mode utilisateur** : GDT + TSS réels (`kernel/gdt.c`), tâches ring 3 via `task_create_user()` — voir section dédiée pour ce que ça apporte (et pas) en l'absence d'isolation mémoire.
- **Pilotes** : VGA texte/graphique, couche graphique générique, port série, clavier et souris PS/2, timer (PIT), disque ATA (lecture/écriture PIO, `driver/ata.c`), carte réseau RTL8139 (`driver/rtl8139.c`, polling), audio, USB (probe, contrôleur xHCI, clavier HID).
- **Installeur** : module capable d'écrire une table de partitions MBR sur une cible ATA, utilisable comme module GRUB séparé (`ALOS Installer`).

## Système de fichiers persistant (`/mnt`)

`kernel/fs/diskfs.c` ("AlosFS") est un système de fichiers simple stocké sur disque ATA, monté sur `/mnt`. Tout ce qui est écrit sous `/mnt` survit à un redémarrage — contrairement au reste de l'arbre (`/home`, `/tmp`, etc.), qui reste en RAM via `ramfs` et disparaît au reboot.

Format volontairement simple pour cette première version : pas d'allocation fine de blocs. Chaque fichier a un slot de taille fixe (4 Ko) sur le disque, chaque nœud a son propre secteur de métadonnées (128 nœuds max). Ça gaspille de l'espace (~576 Ko au total) mais évite d'avoir à écrire un allocateur/bitmap de blocs pour l'instant.

Montage : au boot, `diskfs_init()` cherche une partition MBR de type `0xA0` (même convention que l'installeur, `INSTALLER_PART_TYPE_PERSIST`) sur le premier disque ATA détecté. Si aucune table MBR valide n'est trouvée, le disque entier est utilisé tel quel depuis le LBA 0 — pratique pour tester avec une image disque brute attachée directement à QEMU (`-hda alos_persist.img`), sans passer par l'installeur. Si aucun disque ATA n'est présent du tout, `/mnt` reste simplement vide/indisponible et le reste du système continue de fonctionner normalement.

```bash
# Test rapide sans installeur : un disque brut vide suffit, AlosFS se
# formate tout seul au premier boot (signature "ALFS" absente -> superbloc neuf).
qemu-img create -f raw alos_persist.img 8M
qemu-system-i386 -m 512 -vga std -cdrom alos-linux-like.iso -hda alos_persist.img -boot d
```

Dans le shell : `cd /mnt`, `mkdir /mnt/notes`, `echo bonjour > ...` (pas de redirection shell pour l'instant, utiliser `touch` + un éditeur, ou `elfrun`/`asm` pour écrire par programme) fonctionnent comme sur le reste de l'arbre ; le contenu survit à un `qemu-system-i386 ... -hda alos_persist.img` suivant.

## Chargeur ELF32 (`elfrun`, `elfmod`)

`kernel/exec/elf_loader.c` charge un exécutable ELF32 statique (`ET_EXEC`, i386, little-endian) : les segments `PT_LOAD` sont copiés à leur `p_vaddr` (adressage identité grâce à la pagination), le `.bss` est mis à zéro, puis `e_entry` est lancé comme une nouvelle tâche via `task_create()` — préemptible comme n'importe quel programme `asm`.

**Important — même modèle de confiance que `asm_exec`** : il n'y a pas encore de mode utilisateur (ring 3) dans ALOS, donc un ELF chargé tourne en ring 0 sans aucune isolation mémoire. Ce n'est pas un bac à sable ; un programme buggé peut planter le noyau, exactement comme un programme `asm` mal écrit. Un garde-fou minimal rejette les segments dont l'adresse sort de la plage `[0x00200000, 0x10000000)` (2–256 Mo), pour éviter les erreurs les plus grossières (mauvais linker script, fichier corrompu) — pas une vraie protection.

Deux façons de lancer un ELF :
- `elfrun <chemin>` : lit le fichier depuis `ramfs` ou `/mnt` (limité à 4 Ko, la taille max d'un fichier sur ces deux systèmes de fichiers pour l'instant).
- `elfmod <nom>` : charge directement un module multiboot (même mécanisme que l'installeur), sans limite de taille — pratique pour un vrai binaire.

Recette pour compiler un ELF de test (nécessite un cross-compilateur i686-elf ou `gcc -m32` avec binutils supportant `-m elf_i386`) :

```bash
cat > hello.c <<'EOF'
void _start(void) {
    /* Pas de libc : juste une boucle, verifiable via 'ps' dans le shell.
     * int 0x80 (write/exit/...) est disponible si besoin, cf. kernel/syscall/syscall.c */
    volatile int x = 0;
    while (1) { x++; if (x > 1000000) x = 0; }
}
EOF
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -c hello.c -o hello.o
ld -m elf_i386 -Ttext 0x00200000 -e _start -o hello.elf hello.o --oformat elf32-i386
```

Puis soit copier `hello.elf` sur `/mnt` avec l'utilitaire côté hôte (voir section dédiée juste en dessous), soit le passer en module GRUB (`module /hello.elf hello`) et faire `elfmod hello` dans le shell.

## Mode utilisateur (ring 3)

Depuis cette session, ALOS peut lancer des tâches en ring 3 (mode utilisateur), en plus du mode ring 0 historique. Nouveau module `kernel/gdt.c` : une vraie GDT (6 descripteurs : nul, code/données noyau à 0x08/0x10, code/données utilisateur à 0x18/0x20, TSS à 0x28) plus un TSS minimal, chargés très tôt dans `kernel_main()` — **avant** `keyboard_init()`, qui capture le `%cs` courant pour construire toutes les portes de l'IDT (contrainte d'ordre importante, documentée en commentaire dans `kernel.c` et `kernel/gdt.h`). Jusqu'à cette session la GDT était du code mort : le noyau tournait sur les segments plats fournis par GRUB.

`kernel/process/task.c` a un nouveau `task_create_user()` (à côté de `task_create()` existant) qui construit une fausse frame `iret` à 5 mots (`ss`, `esp`, `eflags`, `cs`, `eip`) au lieu de 3, avec `cs`/`ss` pointant sur les descripteurs utilisateur (RPL=3). Chaque tâche a maintenant une pile utilisateur dédiée (`user_stack`, 16 Ko) en plus de sa pile noyau existante (64 Ko, qui sert alors uniquement de pile d'interruption/syscall pour les tâches ring3, via `TSS.esp0` — mis à jour à chaque changement de contexte dans `kernel/process/scheduler.c`).

`elfrun3 <chemin>` et `elfmod3 <module>` chargent et lancent un ELF32 en ring3 (mêmes garde-fous d'adresse que `elfrun`/`elfmod`, voir plus haut). **Important** : sans crt0/libc, un programme ring3 doit se terminer en appelant explicitement `int 0x80` avec `eax=1` (`SYS_EXIT`, voir `kernel/syscall/syscall.c`) — tomber en fin de fonction (`ret`) est un comportement indéfini, la pile utilisateur ne contient aucune adresse de retour valide. Exemple minimal :

```c
void _start(void) {
    /* ... travail du programme ... */
    __asm__ volatile ("mov $1, %eax\n\t int $0x80"); /* SYS_EXIT */
    __builtin_unreachable();
}
```

**Ce que ring3 apporte réellement** : une vraie séparation de privilège CPU (un programme ring3 ne peut pas exécuter `cli`/`hlt`/`in`/`out`/écrire dans `CR0` etc. — ces instructions génèrent un `#GP` géré par le gestionnaire d'exceptions existant, sans planter le noyau). **Ce que ça n'apporte PAS encore** : d'isolation mémoire — la pagination reste en identité (`kernel/memory/paging.c`), donc un programme ring3 peut toujours lire/écrire n'importe quelle adresse physique tant que le CPU ne le lui interdit pas explicitement par un mécanisme *autre* que la pagination (il n'y en a pas). Une vraie isolation mémoire par tâche demanderait des tables de pages 4 Ko par-tâche, pas encore fait.

## Systeme de fichiers persistant (`/mnt`) — allocateur de blocs

`kernel/fs/diskfs.c` (AlosFS) est passé d'un format "un slot fixe de 4 Ko par fichier" à un vrai allocateur de blocs cette session : un bitmap de blocs libres (1 bit/bloc de 512 octets) et des fichiers stockés en chaîne de blocs (les 4 derniers octets de chaque bloc pointent vers le bloc suivant, `0xFFFFFFFF` = fin de chaîne). Le layout exact (bitmap, zone de données) est calculé à partir de la taille réelle de la partition/disque au premier montage et persisté dans le superbloc (version bumpée à 2) — un remontage relit ce layout au lieu de le recalculer.

Concrètement : une taille de fichier n'est plus limitée à un slot fixe, seulement par l'espace libre sur le disque (dans la limite de ce qu'un seul appel à `diskfs_create_bytes()` peut allouer d'un coup, voir `DISKFS_MAX_BLOCKS_PER_WRITE` dans `diskfs.c` — une limite d'implémentation, pas du format). Le format reste 128 nœuds max (`DISKFS_MAX_NODES`), chaque nœud son propre secteur de métadonnées comme avant.

**Correctif au passage** : au premier formatage, TOUS les secteurs de métadonnées (128, pas seulement la racine) sont maintenant explicitement écrits à zéro — sinon un remontage ultérieur pourrait relire des octets bruts du disque comme des nœuds fantômes `used=1`.

### Utilitaire côté hôte (`tools/alosfs_tool.py`)

Nouveau script Python, binaire-compatible avec le format ci-dessus, qui lit/écrit directement les secteurs d'une image disque brute **sans passer par le noyau** :

```bash
qemu-img create -f raw alos_persist.img 8M
python3 tools/alosfs_tool.py format alos_persist.img --size 8M
python3 tools/alosfs_tool.py put alos_persist.img hello.elf /hello.elf
python3 tools/alosfs_tool.py ls alos_persist.img
python3 tools/alosfs_tool.py info alos_persist.img
```

Puis démarrer avec `-hda alos_persist.img` et faire `elfrun /mnt/hello.elf` (ou `elfrun3` pour un lancement ring3) dans le shell — plus besoin de passer par un module GRUB pour tester un binaire sur `/mnt`. Sous-commandes : `format`, `info`, `ls`, `put`, `get`, `rm`, `mkdir`. Voir l'en-tête du script pour le detail complet.

## Accélération SIMD (SSE2)

`kernel/lib/simd.c` (nouveau) détecte SSE2 via CPUID puis active `CR0.MP`/`CR4.OSFXSR`/`CR4.OSXMMEXCPT` (sans quoi la première instruction SSE déclenche un `#UD`). `kernel/lib/kmemcpy`/`kmemset` (`kernel/lib/string.c`) ont un chemin rapide qui copie/remplit par blocs de 16 octets via `movdqu`/`pshufd` quand SSE2 est disponible ET que la taille dépasse 64 octets (`SIMD_MIN_BYTES`) ; en dessous, ou si SSE2 est absent/pas encore initialisé, le chemin scalaire octet-par-octet historique est utilisé tel quel.

**Choix de sécurité important** : les `CFLAGS` globales du Makefile ne contiennent PAS `-msse2` — volontairement. Seules les deux fonctions concernées sont marquées `__attribute__((target("sse2")))`, ce qui autorise gcc à utiliser les registres `xmm*` en assembleur inline dans CES fonctions précises, sans changer les options de compilation du reste du noyau. Objectif : empêcher l'auto-vectorisation de `-O3` de glisser des instructions SSE ailleurs dans le noyau, qui s'exécuteraient potentiellement AVANT `simd_init()` (appelé juste après `pmm_init()`) et donc avant que `CR0`/`CR4` soient configurés — ce qui planterait le tout premier `memset`/`memcpy` du boot avec un `#UD`. `simd_available()` vaut 0 par défaut (avant tout appel à `simd_init()`, ou si le CPU ne supporte pas SSE2), donc ce risque est nul même sans cette précaution — la précaution vise le risque résiduel d'auto-vectorisation ailleurs.

Pas de sauvegarde/restauration `FXSAVE`/`FXRSTOR` par tâche : `xmm0` est utilisé en pur registre scratch dans des fonctions courtes sans appel intermédiaire, jamais supposé survivre à un changement de contexte. Suffisant pour l'usage actuel (kmemcpy/kmemset uniquement) ; deviendrait nécessaire si du code flottant/SIMD persistant par tâche était ajouté plus tard.

## Pile réseau minimale (`net`)

**Scope volontairement réduit** par rapport à "une pile réseau" au sens complet — voir plus bas ce qui manque. Nouveau pilote `driver/rtl8139.c` (carte RTL8139, détection PCI, mode polling — pas d'IRQ) et `kernel/net/net.c` (Ethernet + ARP + ICMP echo par-dessus). Adresse IP fixe codée en dur (`10.0.2.15`, choisie pour correspondre à ce que QEMU en réseau usermode `-netdev user` attribuerait de toute façon par défaut) : pas de client DHCP.

Commandes shell : `net info` (affiche MAC/IP), `net ping <ip>` (résout l'IP en ARP — cache d'UNE seule entrée — puis émet un echo ICMP, bloquant avec un timeout de 2s). Le noyau répond aussi automatiquement, en tâche de fond (`net_poll()`, appelé dans la boucle d'attente clavier du shell), à toute requête ARP "who-has" ou tout ping ICMP entrant qui lui est adressé — donc pingable depuis l'extérieur si la configuration réseau QEMU le permet (tap/bridge), ou testable en interne via `net ping 10.0.2.2` (le routeur virtuel SLIRP de QEMU répond à ARP/ICMP).

Test QEMU :

```bash
qemu-system-i386 -m 512 -vga std -cdrom alos-linux-like.iso -boot d \
    -device rtl8139,netdev=n0 -netdev user,id=n0
```

**Ce qui manque, explicitement** (pas juste "pas encore fait", des choix de scope assumés pour cette première tranche) :
- pas de TCP/UDP, donc pas de sockets, pas de serveur/client applicatif possible ;
- pas de DHCP (IP fixe) ;
- pas d'IRQ pour la carte réseau : tout est en `polling` (`net_poll()` doit être appelé régulièrement, ce qui limite la réactivité et le débit) ;
- cache ARP à une seule entrée (pas une vraie table) ;
- pas de fragmentation IP, pas d'IPv6.

## Stade d'avancement

## Correctifs notables (non planifiés, trouvés en testant réellement sous QEMU)

Deux bugs réels ont été trouvés et corrigés en pilotant le noyau au clavier/souris dans QEMU (pas seulement en relisant le code) :

- **Souris figée** : `mouse_init()` faisait des échanges synchrones avec le contrôleur PS/2 (port 0x64/0x60) qui pouvaient être court-circuités par l'IRQ1 clavier déjà active, empêchant l'envoi de la commande d'activation du reporting (0xF4). Corrigé en encadrant l'init de `cli`/`sti` (`driver/mouse.c`), comme le faisait déjà `keyboard_init()`.
- **`task_create()` plantait (#UD) au premier changement de contexte vers une tâche neuve** : `setup_stack()` (`kernel/process/task.c`) ne posait pas l'adresse de retour que le `ret` final de `do_context_switch` (`switch_ctx.asm`) doit trouver en haut de la pile. Corrigé en ajoutant une étiquette `task_resume_point` juste après le `call do_context_switch` dans `irq0_stub` (`driver/idt_stubs.asm`) et en la poussant explicitement dans `setup_stack()`.
- **Corollaire** : une fois ce premier bug corrigé, un second est apparu — un programme `asm` se terminant par `hlt` seul plantait (#UD/#GP aléatoire) au premier tick timer suivant, car l'exécution reprenait un octet après le `hlt` dans une zone du buffer non initialisée. Corrigé dans `kernel/exec/asm_exec.c` : le code généré se termine toujours par une boucle fermée `hlt ; jmp $-2` (sauf s'il se termine déjà par un vrai `ret`), plus un remplissage de sécurité du buffer en `hlt`.

## Stade d'avancement

Projet en développement actif, pas encore prêt pour un usage réel. Fonctionnalités connues comme manquantes ou incomplètes (roadmap, voir `complet_list`) :

- [x] Pagination basique : identité (virt == phys), pages larges 4 Mo (PSE), activée au boot (`kernel/memory/paging.c`). Pas encore de tables de pages fines 4 Ko ni de bit NX. **Écrit cette session, pas encore recompilé/testé sous QEMU** (voir note ci-dessous).
- [x] Système de fichiers persistant basique (`kernel/fs/diskfs.c`, montage `/mnt`, voir section dédiée). Pas d'allocation fine de blocs, 4 Ko max par fichier, 128 fichiers max. **Écrit cette session, pas encore recompilé/testé sous QEMU.**
- [x] Chargeur ELF32 statique (`kernel/exec/elf_loader.c`, `elfrun`/`elfmod`). Tourne en ring 0 sans isolation (voir section dédiée). **Écrit cette session, pas encore recompilé/testé sous QEMU.**
- [x] Séparation mode utilisateur / noyau (ring 3) : GDT+TSS réels (`kernel/gdt.c`), `task_create_user()`, `elfrun3`/`elfmod3`. Pas encore d'isolation mémoire (pagination toujours en identité) — voir section dédiée pour le détail de ce que ring3 apporte et n'apporte pas. **Écrit cette session, pas encore recompilé/testé sous QEMU.**
- [x] Allocateur de blocs sur `/mnt` (bitmap + fichiers en chaîne de blocs, remplace le slot fixe de 4 Ko) + utilitaire côté hôte `tools/alosfs_tool.py`. **Écrit cette session, pas encore recompilé/testé sous QEMU** (le script Python lui-même n'a pas non plus pu être exécuté, l'environnement d'exécution Python du sandbox étant tombé en panne au même moment que l'environnement de build C — voir note ci-dessous).
- [x] Optimisations SIMD (SSE2) sur `kmemcpy`/`kmemset` (`kernel/lib/simd.c`), avec repli scalaire automatique si SSE2 absent. **Écrit cette session, pas encore recompilé/testé sous QEMU** — la partie la plus délicate à vérifier sans compilation réelle (voir section dédiée, notamment le choix de ne pas compiler tout le fichier en `-msse2`).
- [x] Pile réseau minimale : pilote RTL8139 (`driver/rtl8139.c`, polling) + Ethernet/ARP/ICMP echo (`kernel/net/net.c`), commandes `net info`/`net ping`. Scope volontairement réduit (pas de TCP/UDP/DHCP/IRQ, voir section dédiée pour la liste complète). **Écrit cette session, pas encore recompilé/testé sous QEMU — c'est, avec le SIMD, la partie la moins éprouvée de cette session** (registres RTL8139 et calculs de checksum non exécutés une seule fois en pratique).

> **Note sur l'état de vérification** : la pagination, `/mnt`, le chargeur ELF, le mode ring3, l'allocateur de blocs AlosFS + son utilitaire hôte, l'accélération SIMD et la pile réseau ont tous été écrits et relus attentivement (structure, tailles de buffers, cohérence des types, offsets binaires) mais **n'ont pas pu être recompilés ni testés sous QEMU** dans cette session à cause d'une panne de l'environnement de build qui a persisté sur toute la durée de la session (disque plein, sandbox bloqué — y compris pour l'exécution du script Python `alosfs_tool.py`, qui n'a donc pas non plus pu être testé). Tout le reste de ce README (souris, `task_resume_point`, le fix `hlt`, l'historique de commandes, la coloration syntaxique côté AlosGraphEngine) a en revanche été vérifié pour de vrai en pilotant le noyau au clavier/souris sous QEMU, lors d'une session précédente où le sandbox fonctionnait. Recompiler et lancer `make run` (ou l'équivalent QEMU manuel ci-dessous) avant de committer si une confirmation est nécessaire — c'est la priorité numéro un pour la prochaine session disposant d'un environnement de build fonctionnel, avant tout nouveau développement.

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

Avec un disque persistant (`/mnt`, voir plus haut) :

```bash
qemu-system-i386 -m 512 -vga std -cdrom alos-linux-like.iso -hda alos_persist.img -boot d
```
