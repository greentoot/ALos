#!/usr/bin/env python3
"""
tools/alosfs_tool.py - Utilitaire cote hote pour AlosFS (format "v2",
allocateur de blocs, voir kernel/fs/diskfs.c). Lit/ecrit directement les
secteurs d'une image disque brute (ex: alos_persist.img cree par
`qemu-img create -f raw ... 8M`), SANS passer par le noyau -- pratique pour
preparer un disque persistant avant le tout premier boot (ex: copier un
binaire ELF de test sur /mnt avant de lancer `elfrun`/`elfrun3`).

Binaire-compatible avec kernel/fs/diskfs.c : ne pas modifier les offsets ou
le format ci-dessous sans repercuter le changement des deux cotes.

Usage :
    alosfs_tool.py format IMAGE --size TAILLE [--offset SECTEUR]
    alosfs_tool.py info   IMAGE [--offset SECTEUR]
    alosfs_tool.py ls     IMAGE [CHEMIN] [--offset SECTEUR]
    alosfs_tool.py put    IMAGE FICHIER_LOCAL CHEMIN_ALOSFS [--offset SECTEUR]
    alosfs_tool.py get    IMAGE CHEMIN_ALOSFS FICHIER_LOCAL [--offset SECTEUR]
    alosfs_tool.py rm     IMAGE CHEMIN_ALOSFS [--offset SECTEUR]
    alosfs_tool.py mkdir  IMAGE CHEMIN_ALOSFS [--offset SECTEUR]

--offset : secteur de depart (LBA) de la zone AlosFS. Si omis : recherche
automatique d'une partition MBR de type 0xA0 (meme convention que
kernel/install/installer.c et tools/build_disk_image.sh) ; a defaut,
secteur 0 (image/disque brut entier) -- c'est aussi exactement le fallback
utilise par le noyau (diskfs_init()) quand aucune table MBR valide n'est
trouvee, donc le cas courant pour une image de test creee directement avec
`qemu-img create` (sans passer par l'installeur).

TAILLE (pour "format") accepte des suffixes K/M/G (ex: 8M). Si l'image
n'existe pas encore, elle est creee (remplie de zeros) a cette taille.

Exemple complet :
    qemu-img create -f raw alos_persist.img 8M
    python3 tools/alosfs_tool.py format alos_persist.img --size 8M
    python3 tools/alosfs_tool.py put alos_persist.img hello.elf /hello.elf
    python3 tools/alosfs_tool.py ls alos_persist.img
    qemu-system-i386 -m 512 -vga std -cdrom alos-linux-like.iso \\
        -hda alos_persist.img -boot d
    # puis dans le shell : elfrun /mnt/hello.elf  (ou elfrun3 pour ring3)
"""
import argparse
import os
import struct
import sys

SECTOR = 512
MAX_NODES = 128
MAX_PATH = 80
BLOCK_PAYLOAD = SECTOR - 4
BLOCK_END = 0xFFFFFFFF
PART_TYPE_PERSIST = 0xA0

NODE_NONE, NODE_FILE, NODE_DIR = 0, 1, 2


def parse_size(s):
    s = s.strip().upper()
    mult = 1
    if s.endswith("K"):
        mult, s = 1024, s[:-1]
    elif s.endswith("M"):
        mult, s = 1024 * 1024, s[:-1]
    elif s.endswith("G"):
        mult, s = 1024 * 1024 * 1024, s[:-1]
    return int(s) * mult


class Image:
    def __init__(self, path):
        self.f = open(path, "r+b")

    def close(self):
        self.f.close()

    def read_sector(self, lba):
        self.f.seek(lba * SECTOR)
        data = self.f.read(SECTOR)
        if len(data) < SECTOR:
            data = data + b"\x00" * (SECTOR - len(data))
        return bytearray(data)

    def write_sector(self, lba, data):
        assert len(data) == SECTOR
        self.f.seek(lba * SECTOR)
        self.f.write(bytes(data))

    def size_sectors(self):
        self.f.seek(0, os.SEEK_END)
        return self.f.tell() // SECTOR


def find_persist_region(img):
    """Reproduit find_persist_region() de kernel/fs/diskfs.c : cherche une
    partition MBR de type 0xA0. Retourne (start_lba, sector_count) ou None
    si pas de table MBR valide."""
    mbr = img.read_sector(0)
    if not (mbr[510] == 0x55 and mbr[511] == 0xAA):
        return None
    for i in range(4):
        e = mbr[446 + i * 16: 446 + i * 16 + 16]
        ptype = e[4]
        start = struct.unpack_from("<I", e, 8)[0]
        count = struct.unpack_from("<I", e, 12)[0]
        if ptype == PART_TYPE_PERSIST and start != 0 and count != 0:
            return (start, count)
    return None


def node_pack(name, size, used, ntype, first_block):
    sec = bytearray(SECTOR)
    nb = name.encode("ascii", "replace")[:MAX_PATH - 1]
    sec[0:len(nb)] = nb
    struct.pack_into("<I", sec, MAX_PATH, size & 0xFFFFFFFF)
    sec[MAX_PATH + 4] = used
    sec[MAX_PATH + 5] = ntype
    struct.pack_into("<I", sec, MAX_PATH + 8, first_block & 0xFFFFFFFF)
    return sec


def node_unpack(sec):
    raw = bytes(sec[0:MAX_PATH - 1])
    name = raw.split(b"\x00", 1)[0].decode("ascii", "replace")
    size = struct.unpack_from("<I", sec, MAX_PATH)[0]
    used = sec[MAX_PATH + 4]
    ntype = sec[MAX_PATH + 5]
    first_block = struct.unpack_from("<I", sec, MAX_PATH + 8)[0]
    return {"name": name, "size": size, "used": used, "type": ntype, "first_block": first_block}


def normalize(path):
    if not path or not path.startswith("/"):
        raise SystemExit(f"erreur: chemin non absolu: {path!r}")
    parts = [p for p in path.split("/") if p not in ("", ".")]
    if ".." in parts:
        raise SystemExit("erreur: '..' non supporte par cet outil, donner un chemin absolu direct")
    return "/" + "/".join(parts) if parts else "/"


def parent_of(path):
    if path == "/":
        return None
    idx = path.rfind("/")
    return path[:idx] if idx > 0 else "/"


class AlosFS:
    """Ouvre une zone AlosFS deja formatee (leve une erreur sinon -- voir
    cmd_format() pour creer une zone neuve)."""

    def __init__(self, image_path, offset=None):
        self.img = Image(image_path)
        if offset is None:
            found = find_persist_region(self.img)
            self.region = found[0] if found else 0
        else:
            self.region = offset

        sec = self.img.read_sector(self.region)
        if sec[0:4] != b"ALFS":
            raise SystemExit(
                f"erreur: pas de signature AlosFS au secteur {self.region} "
                f"(lancer 'format' d'abord)")
        self.version = sec[4]
        self.bitmap_lba = self.region + struct.unpack_from("<I", sec, 8)[0]
        self.data_lba = self.region + struct.unpack_from("<I", sec, 12)[0]
        self.data_blocks = struct.unpack_from("<I", sec, 16)[0]

    def close(self):
        self.img.close()

    def read_node(self, idx):
        return node_unpack(self.img.read_sector(self.region + 1 + idx))

    def write_node(self, idx, node):
        sec = node_pack(node["name"], node["size"], node["used"], node["type"], node["first_block"])
        self.img.write_sector(self.region + 1 + idx, sec)

    def all_nodes(self):
        return [self.read_node(i) for i in range(MAX_NODES)]

    def find_index(self, abs_path):
        for i, n in enumerate(self.all_nodes()):
            if n["used"] and n["name"] == abs_path:
                return i, n
        return -1, None

    def find_free_slot(self):
        for i, n in enumerate(self.all_nodes()):
            if not n["used"]:
                return i
        return -1

    def bitmap_get(self, block_idx):
        byte_off = block_idx // 8
        sector_off = byte_off // SECTOR
        byte_in_sector = byte_off % SECTOR
        sec = self.img.read_sector(self.bitmap_lba + sector_off)
        return (sec[byte_in_sector] >> (block_idx % 8)) & 1

    def bitmap_set(self, block_idx, used):
        byte_off = block_idx // 8
        sector_off = byte_off // SECTOR
        byte_in_sector = byte_off % SECTOR
        sec = self.img.read_sector(self.bitmap_lba + sector_off)
        if used:
            sec[byte_in_sector] |= (1 << (block_idx % 8))
        else:
            sec[byte_in_sector] &= (~(1 << (block_idx % 8)) & 0xFF)
        self.img.write_sector(self.bitmap_lba + sector_off, sec)

    def alloc_block(self):
        for i in range(self.data_blocks):
            if not self.bitmap_get(i):
                self.bitmap_set(i, 1)
                return i
        raise SystemExit("erreur: plus d'espace libre sur AlosFS")

    def free_chain(self, first_block):
        cur = first_block
        while cur != BLOCK_END:
            sec = self.img.read_sector(self.data_lba + cur)
            nxt = struct.unpack_from("<I", sec, BLOCK_PAYLOAD)[0]
            self.bitmap_set(cur, 0)
            cur = nxt

    def read_file(self, node):
        data = bytearray()
        cur = node["first_block"]
        remaining = node["size"]
        while cur != BLOCK_END and remaining > 0:
            sec = self.img.read_sector(self.data_lba + cur)
            chunk = min(remaining, BLOCK_PAYLOAD)
            data += sec[0:chunk]
            remaining -= chunk
            cur = struct.unpack_from("<I", sec, BLOCK_PAYLOAD)[0]
        return bytes(data)

    def write_file(self, idx, node, content):
        if node["first_block"] != BLOCK_END:
            self.free_chain(node["first_block"])
            node["first_block"] = BLOCK_END

        nblocks = 0 if len(content) == 0 else (len(content) + BLOCK_PAYLOAD - 1) // BLOCK_PAYLOAD
        chain = [self.alloc_block() for _ in range(nblocks)]

        off = 0
        for b, blk in enumerate(chain):
            chunk = content[off: off + BLOCK_PAYLOAD]
            sec = bytearray(SECTOR)
            sec[0:len(chunk)] = chunk
            nxt = chain[b + 1] if b + 1 < nblocks else BLOCK_END
            struct.pack_into("<I", sec, BLOCK_PAYLOAD, nxt)
            self.img.write_sector(self.data_lba + blk, sec)
            off += len(chunk)

        node["first_block"] = chain[0] if nblocks else BLOCK_END
        node["size"] = len(content)
        node["type"] = NODE_FILE
        node["used"] = 1
        self.write_node(idx, node)


def cmd_format(args):
    path = args.image
    if args.size:
        size_bytes = parse_size(args.size)
        mode = "r+b" if os.path.exists(path) else "wb"
        with open(path, mode) as f:
            f.truncate(size_bytes)
    if not os.path.exists(path):
        raise SystemExit("erreur: l'image n'existe pas (utiliser --size pour la creer)")

    img = Image(path)
    found = None
    if args.offset is not None:
        region = args.offset
    else:
        found = find_persist_region(img)
        region = found[0] if found else 0

    total_sectors = img.size_sectors()
    region_sectors = found[1] if found else (total_sectors - region)

    if region_sectors < (2 + MAX_NODES):
        raise SystemExit("erreur: zone trop petite pour AlosFS")

    usable = region_sectors - 1 - MAX_NODES
    bitmap_sectors = (usable // 8 + SECTOR - 1) // SECTOR
    if bitmap_sectors == 0:
        bitmap_sectors = 1
    data_lba_rel = 1 + MAX_NODES + bitmap_sectors
    if data_lba_rel >= region_sectors:
        raise SystemExit("erreur: zone trop petite pour AlosFS (bitmap)")
    data_blocks = region_sectors - data_lba_rel

    sb = bytearray(SECTOR)
    sb[0:4] = b"ALFS"
    sb[4] = 2
    struct.pack_into("<I", sb, 8, 1 + MAX_NODES)
    struct.pack_into("<I", sb, 12, data_lba_rel)
    struct.pack_into("<I", sb, 16, data_blocks)
    img.write_sector(region, sb)

    bitmap_lba = region + 1 + MAX_NODES
    zero = bytearray(SECTOR)
    for i in range(bitmap_sectors):
        img.write_sector(bitmap_lba + i, zero)

    root = {"name": "/", "size": 0, "used": 1, "type": NODE_DIR, "first_block": BLOCK_END}
    empty = {"name": "", "size": 0, "used": 0, "type": NODE_NONE, "first_block": BLOCK_END}
    for i in range(MAX_NODES):
        node = root if i == 0 else empty
        sec = node_pack(node["name"], node["size"], node["used"], node["type"], node["first_block"])
        img.write_sector(region + 1 + i, sec)

    img.close()
    print(f"AlosFS formate : secteur de depart={region}, {data_blocks} blocs de "
          f"donnees ({data_blocks * BLOCK_PAYLOAD} octets utiles), "
          f"bitmap={bitmap_sectors} secteur(s), {MAX_NODES} noeuds max.")


def cmd_info(args):
    fs = AlosFS(args.image, args.offset)
    used_blocks = sum(1 for i in range(fs.data_blocks) if fs.bitmap_get(i))
    used_nodes = sum(1 for n in fs.all_nodes() if n["used"])
    print(f"region_start={fs.region} version={fs.version}")
    print(f"bitmap_lba={fs.bitmap_lba} data_lba={fs.data_lba} data_blocks={fs.data_blocks}")
    print(f"noeuds: {used_nodes}/{MAX_NODES}")
    print(f"blocs utilises: {used_blocks}/{fs.data_blocks} "
          f"({used_blocks * BLOCK_PAYLOAD} / {fs.data_blocks * BLOCK_PAYLOAD} octets utiles)")
    fs.close()


def cmd_ls(args):
    fs = AlosFS(args.image, args.offset)
    path = normalize(args.path) if args.path else "/"
    idx, node = fs.find_index(path)
    if idx < 0 or node["type"] != NODE_DIR:
        raise SystemExit(f"erreur: pas un dossier: {path}")
    prefix = path if path == "/" else path + "/"
    entries = []
    for n in fs.all_nodes():
        if not n["used"] or n["name"] == path:
            continue
        if not n["name"].startswith(prefix):
            continue
        rest = n["name"][len(prefix):]
        if "/" in rest:
            continue
        tag = "/" if n["type"] == NODE_DIR else f" ({n['size']} o)"
        entries.append(f"{rest}{tag}")
    fs.close()
    print("\n".join(sorted(entries)) if entries else "(vide)")


def cmd_put(args):
    with open(args.local_file, "rb") as f:
        content = f.read()
    fs = AlosFS(args.image, args.offset)
    path = normalize(args.alosfs_path)
    idx, node = fs.find_index(path)
    if idx < 0:
        parent = parent_of(path)
        pidx, pnode = fs.find_index(parent)
        if pidx < 0 or pnode["type"] != NODE_DIR:
            raise SystemExit(f"erreur: dossier parent introuvable: {parent}")
        idx = fs.find_free_slot()
        if idx < 0:
            raise SystemExit("erreur: table de noeuds pleine")
        node = {"name": path, "size": 0, "used": 1, "type": NODE_FILE, "first_block": BLOCK_END}
    elif node["type"] == NODE_DIR:
        raise SystemExit("erreur: c'est un dossier")
    fs.write_file(idx, node, content)
    fs.close()
    print(f"{len(content)} octets ecrits vers {path}")


def cmd_get(args):
    fs = AlosFS(args.image, args.offset)
    path = normalize(args.alosfs_path)
    idx, node = fs.find_index(path)
    if idx < 0 or node["type"] != NODE_FILE:
        raise SystemExit(f"erreur: fichier introuvable: {path}")
    content = fs.read_file(node)
    fs.close()
    with open(args.local_file, "wb") as f:
        f.write(content)
    print(f"{len(content)} octets lus depuis {path} -> {args.local_file}")


def cmd_rm(args):
    fs = AlosFS(args.image, args.offset)
    path = normalize(args.alosfs_path)
    idx, node = fs.find_index(path)
    if idx < 0:
        raise SystemExit(f"erreur: introuvable: {path}")
    if node["type"] == NODE_DIR:
        prefix = path if path.endswith("/") else path + "/"
        for n in fs.all_nodes():
            if n["used"] and n["name"] != path and n["name"].startswith(prefix):
                raise SystemExit("erreur: dossier non vide")
    elif node["first_block"] != BLOCK_END:
        fs.free_chain(node["first_block"])
    fs.write_node(idx, {"name": "", "size": 0, "used": 0, "type": NODE_NONE, "first_block": BLOCK_END})
    fs.close()
    print(f"supprime: {path}")


def cmd_mkdir(args):
    fs = AlosFS(args.image, args.offset)
    path = normalize(args.alosfs_path)
    idx, node = fs.find_index(path)
    if idx >= 0:
        fs.close()
        if node["type"] == NODE_DIR:
            print("deja un dossier, rien a faire")
            return
        raise SystemExit("erreur: existe deja et n'est pas un dossier")
    parent = parent_of(path)
    pidx, pnode = fs.find_index(parent)
    if pidx < 0 or pnode["type"] != NODE_DIR:
        raise SystemExit(f"erreur: dossier parent introuvable: {parent}")
    slot = fs.find_free_slot()
    if slot < 0:
        raise SystemExit("erreur: table de noeuds pleine")
    fs.write_node(slot, {"name": path, "size": 0, "used": 1, "type": NODE_DIR, "first_block": BLOCK_END})
    fs.close()
    print(f"dossier cree: {path}")


def main():
    p = argparse.ArgumentParser(description="Utilitaire cote hote pour AlosFS (voir kernel/fs/diskfs.c)")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_offset(sp):
        sp.add_argument("--offset", type=int, default=None,
                         help="secteur LBA de depart de la zone AlosFS (auto-detecte sinon)")

    sf = sub.add_parser("format", help="formate une zone AlosFS neuve")
    sf.add_argument("image")
    sf.add_argument("--size", help="taille de l'image (cree/redimensionne), ex: 8M")
    add_offset(sf)
    sf.set_defaults(func=cmd_format)

    si = sub.add_parser("info", help="affiche les informations de la zone AlosFS")
    si.add_argument("image")
    add_offset(si)
    si.set_defaults(func=cmd_info)

    sl = sub.add_parser("ls", help="liste un dossier")
    sl.add_argument("image")
    sl.add_argument("path", nargs="?", default="/")
    add_offset(sl)
    sl.set_defaults(func=cmd_ls)

    sp_ = sub.add_parser("put", help="copie un fichier local vers AlosFS")
    sp_.add_argument("image")
    sp_.add_argument("local_file")
    sp_.add_argument("alosfs_path")
    add_offset(sp_)
    sp_.set_defaults(func=cmd_put)

    sg = sub.add_parser("get", help="copie un fichier AlosFS vers le disque local")
    sg.add_argument("image")
    sg.add_argument("alosfs_path")
    sg.add_argument("local_file")
    add_offset(sg)
    sg.set_defaults(func=cmd_get)

    sr = sub.add_parser("rm", help="supprime un fichier ou un dossier vide")
    sr.add_argument("image")
    sr.add_argument("alosfs_path")
    add_offset(sr)
    sr.set_defaults(func=cmd_rm)

    sm = sub.add_parser("mkdir", help="cree un dossier")
    sm.add_argument("image")
    sm.add_argument("alosfs_path")
    add_offset(sm)
    sm.set_defaults(func=cmd_mkdir)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
