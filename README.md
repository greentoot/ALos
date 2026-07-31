# ALOS Clean Kernel

Ce dossier contient la version epuree du systeme ALOS, sans emulateurs,
sans ROMs, sans BIOS DS/GBA et sans contenu de jeu embarque.

La copie complete avec jeux est gardee ici :

```text
C:\Users\anima\Documents\systeme jeu
```

Les morceaux retires du dossier propre sont gardes ici :

```text
C:\Users\anima\Documents\Alos_removed_game_parts
```

## ISO propre

Fichier genere :

```text
C:\Users\anima\Documents\Alos\alos-linux-like.iso
```

Regenerer l'ISO depuis WSL :

```bash
cd /mnt/c/Users/anima/Documents/Alos
make iso-linux-like
```

Lancer avec QEMU :

```bash
qemu-system-i386 -m 512 -vga std -cdrom /mnt/c/Users/anima/Documents/Alos/alos-linux-like.iso -boot d
```

Depuis Windows, si QEMU est dans le PATH :

```powershell
qemu-system-i386 -m 512 -vga std -cdrom "C:\Users\anima\Documents\Alos\alos-linux-like.iso" -boot d
```
