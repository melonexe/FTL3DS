#!/usr/bin/env python3
"""PC-side tool for FTL's ftl.dat (1.6+ "PKG" format).

Mirrors source/ftldat.c so the archive reader can be tested without a 3DS.

    python ftldat.py list    <ftl.dat> [filter]
    python ftldat.py extract <ftl.dat> <path-in-archive> <out-file>

PKG layout (all big-endian):
    header : "PKG\n", u16 header_size, u16 entry_size, u32 entry_count, u32 path_region_size
    entries: u32 path_hash, u32 flags|path_offset, u32 data_offset, u32 data_size, u32 unpacked_size
    paths  : NUL-terminated strings; flag 0x01000000 means the data is zlib-deflated
"""
import struct
import sys
import zlib

FLAG_DEFLATED = 0x01000000


class FtlDat:
    def __init__(self, path):
        self.f = open(path, "rb")
        magic = self.f.read(4)
        if magic != b"PKG\n":
            raise ValueError("not a PKG-format ftl.dat (pre-1.6 archives are not supported)")
        header_size, entry_size, count, path_size = struct.unpack(">HHII", self.f.read(12))
        self.f.seek(header_size)
        raw = self.f.read(entry_size * count)
        paths = self.f.read(path_size)
        self.entries = {}
        for i in range(count):
            _hash, flags_off, off, size, unpacked = struct.unpack_from(">IIIII", raw, i * entry_size)
            name_off = flags_off & 0x00FFFFFF
            name = paths[name_off:paths.index(b"\0", name_off)].decode("ascii")
            self.entries[name] = (off, size, unpacked, bool(flags_off & FLAG_DEFLATED))

    def read(self, name):
        off, size, unpacked, deflated = self.entries[name]
        self.f.seek(off)
        data = self.f.read(size)
        return zlib.decompress(data) if deflated else data


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    cmd, dat = argv[1], FtlDat(argv[2])
    if cmd == "list":
        filt = argv[3] if len(argv) > 3 else ""
        for name, (_o, _s, unpacked, deflated) in sorted(dat.entries.items()):
            if filt in name:
                print(f"{unpacked:>10}  {'z' if deflated else ' '}  {name}")
    elif cmd == "extract":
        with open(argv[4], "wb") as out:
            out.write(dat.read(argv[3]))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
