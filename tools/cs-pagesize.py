#!/usr/bin/env python3
"""cs-pagesize.py: reports the page size each file's code signature was made with.

The HakiOS kernel (BCM2837 configuration) uses 4K pages: it only accepts signatures with
pageSize 12 (4K). If a dyld cache file carries pageSize 14 (16K), XNU prints
"disallowing unsupported code signature page shift: 14" and dyld cannot load the cache.

Usage:
  python3 cs-pagesize.py FILE_OR_FOLDER [...]
Example:
  python3 cs-pagesize.py ~/dev/hakios/ios165/20F66__iPhone10,3_6
"""
import os
import struct
import sys

CSMAGIC_EMBEDDED = 0xFADE0CC0
CSMAGIC_CD = 0xFADE0C02
MH_MAGIC_64 = 0xFEEDFACF
LC_CODE_SIGNATURE = 0x1D


def cds_in_superblob(blob):
    """Returns [(slot, pageshift, hashtype)] for the CodeDirectories of the SuperBlob."""
    out = []
    if len(blob) < 12:
        return out
    magic, _length, count = struct.unpack(">III", blob[:12])
    if magic == CSMAGIC_CD:
        return [(0, blob[39], blob[37])]
    if magic != CSMAGIC_EMBEDDED:
        return out
    for i in range(count):
        typ, off = struct.unpack(">II", blob[12 + 8 * i:20 + 8 * i])
        if off + 40 > len(blob):
            continue
        if struct.unpack(">I", blob[off:off + 4])[0] == CSMAGIC_CD:
            out.append((typ, blob[off + 39], blob[off + 37]))
    return out


def signature_of(path):
    with open(path, "rb") as f:
        head = f.read(4096)
        if head.startswith(b"dyld_v1"):
            cs_off, cs_size = struct.unpack("<QQ", head[40:56])
            kind = "cache"
        elif len(head) >= 32 and struct.unpack("<I", head[:4])[0] == MH_MAGIC_64:
            ncmds, sizeofcmds = struct.unpack("<II", head[16:24])
            cmds = head[32:32 + sizeofcmds]
            if len(cmds) < sizeofcmds:
                f.seek(32)
                cmds = f.read(sizeofcmds)
            cs_off = cs_size = 0
            p = 0
            for _ in range(ncmds):
                cmd, size = struct.unpack("<II", cmds[p:p + 8])
                if cmd == LC_CODE_SIGNATURE:
                    cs_off, cs_size = struct.unpack("<II", cmds[p + 8:p + 16])
                p += size
            kind = "macho"
        else:
            return None, None
        if cs_size == 0:
            return kind, []
        f.seek(cs_off)
        blob = f.read(min(cs_size, 1 << 20))
    return kind, cds_in_superblob(blob)


def files(args):
    for a in args:
        a = os.path.expanduser(a)
        if os.path.isdir(a):
            for root, _dirs, names in os.walk(a):
                for n in sorted(names):
                    yield os.path.join(root, n)
        else:
            yield a


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    malos = 0
    vistos = 0
    for p in files(sys.argv[1:]):
        try:
            kind, cds = signature_of(p)
        except (OSError, struct.error) as e:
            print(f"ERROR  {p}: {e}")
            continue
        if kind is None:
            continue
        vistos += 1
        if not cds:
            print(f"NO SIGNATURE  {p}")
            continue
        shifts = sorted({c[1] for c in cds})
        estado = "OK " if shifts == [12] else "BAD"
        if estado == "BAD":
            malos += 1
        desc = ", ".join(f"slot {t:#x}: {1 << s if s else 0} B (shift {s})" for t, s, _h in cds)
        print(f"{estado}  {os.path.basename(p)}  [{kind}]  {desc}")
    print(f"\n{vistos} files with a recognised header; {malos} with signature pages other than 4K")
    return 1 if malos else 0


if __name__ == "__main__":
    sys.exit(main())
