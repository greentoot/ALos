# ALOS — Plan de test complet

Ce document liste tout ce qui a été écrit mais jamais recompilé ni testé
sous QEMU (à cause d'une panne du sandbox de build sur plusieurs sessions
consécutives), plus les tests de non-régression à refaire sur le code
partagé qui a été modifié. À exécuter dans l'ordre, dans un environnement
Linux/WSL avec le toolchain complet.

## Prérequis

```bash
# Toolchain de compilation
gcc --version        # gcc -m32 doit fonctionner (paquet gcc-multilib sur Debian/Ubuntu)
nasm --version
ld --version          # doit supporter -m elf_i386
qemu-system-i386 --version
grub-mkrescue --version   # ou grub2-mkrescue selon la distro

# Pour l'utilitaire hôte AlosFS
python3 --version    # 3.6+, aucune dépendance externe (stdlib uniquement)
```

## 0. Build de base (bloquant pour tout le reste)

```bash
cd Alos
make clean
make iso-linux-like
```

**Attendu** : `alos-linux-like.iso` généré sans erreur ni warning nouveau
(`gcc -Wall -Wextra` va afficher des warnings existants tolérés, mais ne
doit rien afficher de nouveau lié aux fichiers modifiés cette session :
`kernel/gdt.c`, `kernel/lib/simd.c`, `driver/rtl8139.c`, `kernel/net/net.c`,
`kernel/fs/diskfs.c`, `kernel/exec/elf_loader.c`, `kernel/process/task.c`,
`kernel/process/scheduler.c`).

Si le link échoue : vérifier que tous les nouveaux `.o` sont bien dans
`OBJS` du `Makefile` (`kernel/gdt.o`, `kernel/lib/simd.o`,
`driver/rtl8139.o`, `kernel/net/net.o`).

**Test de démarrage minimal** :

```bash
qemu-system-i386 -m 512 -vga std -cdrom alos-linux-like.iso -boot d
```

**Attendu** : le noyau démarre jusqu'au shell (`ALOS clean kernel pret.`)
sans écran noir ni triple-fault. Si `earlydiag` est activé, chaque étape
(`gdt init`, `simd init`, `paging init`, `net init`, ...) doit s'afficher
sans planter avant `shell run`. **C'est le test le plus important** : si
le boot plante quelque part dans cette liste, il plante précisément à
l'étape affichée juste avant — ça dit immédiatement quel sous-système est
en cause.

## 1. Pagination (`kernel/memory/paging.c`)

Écrite il y a deux sessions, jamais testée.

- Booter normalement (commande ci-dessus). Si `paging_init()` plante ou
  produit un triple-fault, l'écran redémarre en boucle (symptôme QEMU
  typique) juste après l'étape `paging init`.
- Une fois au shell : `mem` (affiche l'état du tas) doit donner des
  chiffres cohérents (pas de valeurs aberrantes/négatives qui trahiraient
  une corruption mémoire liée à un mauvais mapping).
- Test de stress simple : `asm` (lancer plusieurs petits programmes
  assembleur d'affilée), `ps` doit toujours montrer une table de tâches
  saine.

## 2. SIMD / SSE2 (`kernel/lib/simd.c`, `kernel/lib/string.c`)

**Le plus délicat à valider sans exécution réelle** — c'est là qu'un bug
silencieux (mauvais offset, xmm0 pas vraiment scratch, CR4 mal posé) est
le plus probable.

1. Vérifier le message `earlydiag` : `simd: sse2 actif` (QEMU/TCG émule
   SSE2, donc ça doit être actif) — si `simd: sse2 indisponible` apparaît
   à la place sous QEMU, quelque chose ne va pas dans `cpu_has_sse2()`.
2. **Test de corruption silencieuse** (le risque principal) : créer un
   fichier de plus de 64 octets (seuil `SIMD_MIN_BYTES`, donc ça déclenche
   le chemin SSE2) sur `/mnt` ou en `ramfs`, le relire avec `cat`, comparer
   octet à octet avec le contenu attendu. Un bug de troncature/offset dans
   `simd_copy_blocks`/`simd_set_blocks` se verrait immédiatement comme des
   données corrompues ou tronquées.
   ```
   # Dans le shell ALOS :
   touch /home/test.txt
   # écrire >64 octets dedans (via l'éditeur si dispo, ou 'echo' répété,
   # ou plus simple : copier un fichier existant de bonne taille avec elfrun/asm)
   cat /home/test.txt
   ```
3. Test croisé le plus fiable : `elfrun`/`elfmod` copie un ELF potentiellement
   >64 octets en mémoire via `kmemcpy` (segments `PT_LOAD`) — si le
   programme chargé s'exécute correctement (voir section 4), c'est une
   confirmation indirecte que `kmemcpy` accéléré SSE2 est correct sur des
   données réelles.
4. Si un crash (#UD) survient très tôt au boot (avant même le message
   `simd init`), c'est que du code auto-vectorisé SSE s'est glissé
   ailleurs dans le noyau malgré la précaution `target("sse2")` — dans ce
   cas, désactiver temporairement l'appel à `simd_init()` dans `kernel.c`
   pour confirmer le diagnostic, puis inspecter `objdump -d alos.elf` à la
   recherche d'instructions `movdqu`/`xmm` en dehors de
   `simd_copy_blocks`/`simd_set_blocks`.

## 3. GDT + TSS + ring 3 (`kernel/gdt.c`, `task_create_user`, `elfrun3`/`elfmod3`)

**Contrainte d'ordre à vérifier en premier** : si le clavier ne répond
plus du tout après le boot (aucune touche n'est prise en compte), c'est
probablement que `gdt_init()` a cassé la construction de l'IDT dans
`keyboard_init()` (mauvais selecteur `%cs`). Dans ce cas, tout le reste
de cette section est bloqué tant que ça n'est pas corrigé.

1. **Compiler un programme de test ring3** (voir README, section "Mode
   utilisateur") :
   ```bash
   cat > ring3_test.c <<'EOF'
   void _start(void) {
       volatile int x = 0;
       for (int i = 0; i < 500000; i++) x++;
       __asm__ volatile ("mov $1, %eax\n\t int $0x80"); /* SYS_EXIT */
       __builtin_unreachable();
   }
   EOF
   gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -c ring3_test.c -o ring3_test.o
   ld -m elf_i386 -Ttext 0x00200000 -e _start -o ring3_test.elf ring3_test.o --oformat elf32-i386
   ```
2. Copier sur `/mnt` avec l'utilitaire hôte (voir section 5) ou passer en
   module GRUB.
3. Dans le shell : `elfrun3 /mnt/ring3_test.elf` (ou `elfmod3 ring3test`
   si passé en module).
   **Attendu** : message `tache 'elfX' lancee (ring3, voir 'ps')`, la
   tâche apparaît dans `ps`, tourne quelques instants, puis **disparaît
   proprement** de `ps` une fois `int 0x80`/`SYS_EXIT` exécuté (pas de
   crash, pas de tâche zombie qui traîne indéfiniment).
4. **Test de l'isolation de privilège** (le vrai test de "ring3 marche
   vraiment") : modifier le programme de test pour qu'il exécute une
   instruction privilégiée, ex. `__asm__ volatile ("cli");` au milieu.
   **Attendu** : ça doit déclencher un `#GP` intercepté par le gestionnaire
   d'exceptions existant — la tâche fautive doit être arrêtée/killée
   **sans faire planter tout le noyau** (le shell doit rester utilisable
   après). Si le noyau entier plante ou redémarre, l'isolation ring3 ne
   fonctionne pas correctement (TSS.esp0 mal posé, ou GDT user incorrecte).
5. Test de non-régression : relancer un `elfrun`/`elfmod` (ring0, sans le
   `3`) classique, vérifier qu'il fonctionne toujours identiquement à
   avant (le chemin ring0 ne doit pas avoir été affecté par les
   changements de `setup_stack()`/`task_create_internal()`).

## 4. Allocateur de blocs AlosFS (`kernel/fs/diskfs.c`)

```bash
qemu-img create -f raw alos_persist.img 8M
qemu-system-i386 -m 512 -vga std -cdrom alos-linux-like.iso -hda alos_persist.img -boot d
```

1. **Format initial** : au premier boot, vérifier le message `earlydiag`
   `diskfs: /mnt persistant disponible`.
2. **Fichiers multiples, tailles variées** :
   ```
   cd /mnt
   mkdir docs
   touch docs/a.txt
   touch docs/b.txt
   ls docs
   ```
   Créer plusieurs fichiers de tailles différentes (via l'éditeur ou
   `elfrun`/`asm` qui écrivent dessus) — notamment au moins un fichier
   **de plus de 4 Ko** (impossible avec l'ancien format à slot fixe) pour
   confirmer que l'allocateur de blocs fonctionne vraiment. Vérifier avec
   `cat` que le contenu relu correspond exactement à ce qui a été écrit.
3. **Persistance au reboot** (le test le plus important pour `/mnt`) :
   fermer QEMU (`Ctrl+A puis X`, ou fermer la fenêtre), relancer EXACTEMENT
   la même commande QEMU ci-dessus (même `-hda alos_persist.img`), puis
   `cat /mnt/docs/a.txt` etc. **Attendu** : contenu identique à avant le
   redémarrage.
4. **Libération d'espace** : créer un gros fichier, le supprimer (`rm`),
   recréer un autre fichier de taille équivalente. **Attendu** : ça doit
   réussir (si `free_chain()`/le bitmap avaient un bug, l'espace ne serait
   jamais réellement libéré et une deuxième création échouerait une fois
   le disque "plein").
5. **Dossier non vide** : `rm docs` sur un dossier contenant encore des
   fichiers doit échouer proprement (`DISKFS_ERR_DIR_NOTEMPTY`), pas
   planter.
6. **Table de nœuds pleine** : créer volontairement plus de 128 fichiers
   (script shell externe ou boucle manuelle) pour vérifier que
   `DISKFS_ERR_FULL` est bien renvoyé sans corruption au-delà de la limite.

## 5. Utilitaire hôte `tools/alosfs_tool.py`

**Aucune exécution Python n'a été possible cette session** — donc même la
syntaxe n'est vérifiée que par relecture. À tester en tout premier, avant
le reste de la section 4 (le put/get via l'outil est le moyen le plus
simple de préparer des fichiers de test).

```bash
cd Alos
python3 -c "import py_compile; py_compile.compile('tools/alosfs_tool.py', doraise=True)"  # doit passer sans erreur

qemu-img create -f raw alos_persist.img 8M
python3 tools/alosfs_tool.py format alos_persist.img --size 8M
python3 tools/alosfs_tool.py info alos_persist.img
# Attendu : region_start=0, data_blocks > 0, noeuds: 1/128 (juste la racine)

echo "bonjour alosfs" > /tmp/hello.txt
python3 tools/alosfs_tool.py put alos_persist.img /tmp/hello.txt /hello.txt
python3 tools/alosfs_tool.py ls alos_persist.img
# Attendu : hello.txt (15 o)

python3 tools/alosfs_tool.py get alos_persist.img /hello.txt /tmp/hello_out.txt
diff /tmp/hello.txt /tmp/hello_out.txt
# Attendu : aucune différence

python3 tools/alosfs_tool.py mkdir alos_persist.img /sub
python3 tools/alosfs_tool.py put alos_persist.img /tmp/hello.txt /sub/hello2.txt
python3 tools/alosfs_tool.py rm alos_persist.img /hello.txt
python3 tools/alosfs_tool.py ls alos_persist.img
# Attendu : seulement "sub/", hello.txt disparu
```

**Test croisé (le plus important — confirme la compatibilité binaire
outil ↔ noyau)** :

1. `put` un fichier avec l'outil hôte → booter ALOS avec cette image →
   `cat /mnt/hello.txt` dans le shell doit afficher le bon contenu.
2. Dans le shell ALOS, créer/modifier un fichier sur `/mnt` → éteindre
   QEMU → `python3 tools/alosfs_tool.py get alos_persist.img /le_fichier
   /tmp/verif.txt` → comparer avec ce qui a été tapé dans le shell.

Si l'un des deux sens échoue mais pas l'autre, le bug est localisé (soit
dans le format lu/écrit par le noyau, soit dans celui de l'outil Python)
plutôt que dans le format lui-même.

## 6. Chargeur ELF32 ring 0 (`elfrun`, `elfmod`)

Voir le README pour la recette de compilation de `hello.elf`. Une fois
compilé :

```
elfrun /mnt/hello.elf      # ou copier d'abord via alosfs_tool.py, voir section 5
ps                          # la tache doit apparaitre, tourner en boucle
```

```
# variante module GRUB : ajouter "module /hello.elf hello" dans grub.cfg,
# regenerer l'ISO, puis dans le shell :
elfmod hello
ps
```

**Attendu** : la tâche apparaît dans `ps`, aucun crash. Comme ce
`hello.elf` d'exemple ne fait jamais `int 0x80`, il reste indéfiniment en
`ps` (comportement normal pour ce cas ring0 précis — contrairement au
test ring3 de la section 3 qui doit lui se terminer proprement).

## 7. Pile réseau (`driver/rtl8139.c`, `kernel/net/net.c`)

**La partie la moins éprouvée avec le SIMD** — checksum IP/ICMP et
registres RTL8139 jamais exercés une seule fois en pratique.

```bash
qemu-system-i386 -m 512 -vga std -cdrom alos-linux-like.iso -boot d \
    -device rtl8139,netdev=n0 -netdev user,id=n0
```

1. Vérifier le message `earlydiag` : `net: RTL8139 detectee`. Si ça dit
   `pas de carte RTL8139`, la détection PCI (`find_rtl8139`) ou le BAR0 a
   un problème — vérifier avec `info pci` dans le moniteur QEMU que la
   carte est bien vue par l'hyperviseur d'abord (élimine QEMU comme cause).
2. `net info` dans le shell : doit afficher une adresse MAC plausible
   (non nulle, non `FF:FF:FF:FF:FF:FF`) et l'IP fixe `10.0.2.15`.
3. **`net ping 10.0.2.2`** (le routeur virtuel SLIRP de QEMU, qui répond
   à ARP et ICMP par construction) : c'est LE test qui valide toute la
   chaîne (ARP request → ARP reply → cache → ICMP echo request → checksum
   → ICMP echo reply → parsing retour). **Attendu** : `reponse de 10.0.2.2
   en X ms`. Si ça timeout systématiquement, activer la capture réseau
   pour diagnostiquer :
   ```bash
   # ajouter à la ligne QEMU : -object filter-dump,id=f1,netdev=n0,file=/tmp/dump.pcap
   # puis ouvrir /tmp/dump.pcap avec wireshark/tcpdump pour voir si les
   # trames ARP/ICMP sortent correctement de la carte, et si des reponses
   # arrivent (bug de parsing RX) ou non (bug d'emission/checksum TX).
   ```
4. **Test du "responder"** (répondre à un ping entrant) : plus difficile à
   tester avec `-netdev user` seul (NAT, pas directement joignable depuis
   l'hôte). Si une configuration `tap`/bridge est disponible :
   `ping 10.0.2.15` (ou l'IP configurée) depuis l'hôte ou une autre VM,
   pendant que `net_poll()` tourne côté ALOS (juste avoir le shell ouvert
   suffit, il appelle `net_poll()` dans sa boucle d'attente clavier).
   **Attendu** : les pings aboutissent (latence normale, pas de perte).
5. Test de robustesse : envoyer un flot de pings rapides (`ping -f` côté
   hôte si test 4 possible, ou plusieurs `net ping` d'affilée côté ALOS)
   pour vérifier que le rebouclage de l'anneau RX (`g_rx_offset >= 8192`)
   ne désynchronise pas après plusieurs tours.

## 8. Non-régression sur le code partagé

Ces fichiers ont été modifiés par plusieurs des features ci-dessus
(`task.c`/`scheduler.c` par le ring3, `string.c` par le SIMD) — reprendre
les tests déjà validés lors des sessions précédentes pour confirmer
qu'ils marchent toujours à l'identique :

- Souris fonctionnelle (si testé côté AlosGraphEngine, sans objet ici côté
  Alos texte).
- `asm` (micro-assembleur) : lancer un programme simple se terminant par
  `hlt`, vérifier qu'il apparaît dans `ps` sans crash (régression du fix
  `task_resume_point`/hlt-fallthrough, qui partage `task.c`/`idt_stubs.asm`
  avec le nouveau code ring3).
- `ps`, `mem`, `uptime` : sortie cohérente après un usage prolongé du
  shell (pas de fuite/corruption visible après avoir enchaîné plusieurs
  tests des sections précédentes dans la même session QEMU).

## Récapitulatif (à cocher au fur et à mesure)

- [ ] 0. Build + boot minimal
- [ ] 1. Pagination (pas de crash, `mem` cohérent)
- [ ] 2. SIMD (pas de corruption sur fichier >64 o)
- [ ] 3. Ring3 (`elfrun3` termine proprement, `#GP` isolé sans crash noyau)
- [ ] 4. Allocateur de blocs `/mnt` (fichiers >4 Ko, persistance reboot, libération d'espace)
- [ ] 5. `alosfs_tool.py` (syntaxe, put/get round-trip, test croisé avec le noyau)
- [ ] 6. `elfrun`/`elfmod` ring0 (régression)
- [ ] 7. Réseau (`net ping 10.0.2.2` réussit)
- [ ] 8. Non-régression (`asm`, `ps`, `mem`)
