# BIOS Layout

Organisation actuelle du dossier `bios/` :

- `gba_bios.bin`
  - BIOS GBA principal utilise par ALOS aujourd'hui
  - taille attendue : `16384` bytes

- `gb/gb_bios.bin`
  - BIOS/boot ROM Game Boy
  - taille detectee : `256` bytes

- `gb/dmg0_rom.bin`
  - autre boot ROM DMG
  - taille detectee : `256` bytes
  - ce n'est pas un doublon exact de `gb_bios.bin`

- `gbc/gbc_bios.bin`
  - BIOS Game Boy Color
  - taille detectee : `2304` bytes

- `nds/biosnds7.rom`
  - BIOS ARM7 Nintendo DS
  - taille detectee : `16384` bytes

- `nds/biosnds9.rom`
  - BIOS ARM9 Nintendo DS
  - taille detectee : `4096` bytes

- `nds/firmware.bin`
  - firmware Nintendo DS
  - taille detectee : `262144` bytes

- `nds/key.cfg`
  - fichier de configuration present dans le pack DS
  - contenu detecte : mapping texte court
  - ce n'est pas une cle crypto type `aes_keys.txt`

- `nds/BIOSGBA.ROM`
  - BIOS GBA livre dans le pack DS
  - doublon de `bios/gba_bios.bin` (meme hash)

- `3ds/boot9.bin`
  - boot ROM 3DS
  - taille detectee : `65536` bytes

- `3ds/boot11.bin`
  - boot ROM 3DS
  - taille detectee : `65536` bytes

Notes :

- ALOS utilise deja `bios/gba_bios.bin`.
- Les BIOS `GB`, `GBC` et `NDS` sont maintenant ranges proprement pour la suite.
- Les fichiers `boot9.bin` et `boot11.bin` 3DS sont maintenant presents.
- Je n'ai pas trouve dans ce lot d'autres fichiers 3DS typiques comme `aes_keys.txt`, `seeddb.bin` ou `movable.sed`.
