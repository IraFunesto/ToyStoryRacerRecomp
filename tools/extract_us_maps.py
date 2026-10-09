#!/usr/bin/env python3
"""Copy the ten track files that are damaged on the European disc from YOUR
OWN copy of the USA disc into disc_override/, where the game reads them in
place of the European ones.

The European disc (SLES-03398) has about 45 two-byte values overwritten with
0x3E3E in these files (the USA disc has 0x3F80 / 0x3F10 there). Everything
else on both discs is identical, text and voices included, so the replacement
only repairs the tracks: Pier (sea grid corruption, GPU crash), Skate Park,
Car Lot, Cinema, Neighbourhood (broken textures) and five tracks with minor
damage.

Usage (from the folder where you extracted the release zip):
    python tools/extract_us_maps.py "Disney-Pixar Toy Story Racer (USA).bin" [game folder]

disc_override/ must sit next to the game executable the wizard built. The
game folder defaults to build/ when it holds ToyStoryRacer_Recompiled.exe
(the release zip layout), else the current directory. Accepts a raw .bin
(2352-byte sectors) or a plain .iso (2048-byte sectors). Every file is checked
against the known SHA-256 of the USA release before it is written.
"""
import hashlib
import os
import struct
import sys

FILES = {
    "COURSE_A/GAS.DAT":      "c1fe2febf021bbe56c06b19070fd8233d851227274542c539d458e5d1f697749",
    "COURSE_A/SIDSYARD.DAT": "9b74adb41f4912b548fd1024884c3605c9a70fd031021b5f9179aaedcb05d5e4",
    "COURSE_A/SNOWY.DAT":    "6463a91ffd6e1a4556a9d9adeac686b0bb7d9b482d93c6b23c8088a11918fd57",
    "COURSE_B/BOWLING.DAT":  "1a658f64a584422945d9b8c04b3eb70767a9d4ee3319542a9c3dcaae2cc1a95c",
    "COURSE_B/CARLOT.DAT":   "cd72be65e0cfb8d101e01bb98f0d331a88215c5d398e6d2a065dc29df894d6f7",
    "COURSE_B/CINEMA.DAT":   "e4eef4e1ffc57692a30186c1bb5bd7a53ea4c6c8fc2798e9902c2600c364056a",
    "COURSE_B/PIER.AXE":     "8e1ab6e0cb7909f1ba99e2028465070241a9185dea693c6b3341cf1d91918109",
    "COURSE_B/SKATE.AXE":    "1ccee71dfd692b9fbd7de275877d1dc98592d7d422e058ba47a21755378e6dc7",
    "COURSE_B/STREET.DAT":   "a112d23cca1783c1d552f246b3982af74b04692e1762e0ea9b74ebafb62552c0",
    "COURSE_C/BASKET.DAT":   "d3c9c85bf891c8adcfd87f4a3fd13ffa2017473fd100d56ef82664c4c0389105",
}


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        size = os.path.getsize(path)
        # a raw image starts with the 12-byte CD sync pattern
        head = self.f.read(12)
        self.raw = head == b"\x00" + b"\xff" * 10 + b"\x00"
        self.sector = 2352 if self.raw else 2048
        self.count = size // self.sector

    def read(self, lba, nbytes):
        out = bytearray()
        for i in range((nbytes + 2047) // 2048):
            if self.raw:
                self.f.seek((lba + i) * 2352)
                s = self.f.read(2352)
                # Mode 1 user data at +16, Mode 2 Form 1 at +24
                out += s[16:16 + 2048] if s[15] == 1 else s[24:24 + 2048]
            else:
                self.f.seek((lba + i) * 2048)
                out += self.f.read(2048)
        return bytes(out[:nbytes])

    def find(self, path):
        pvd = self.read(16, 2048)
        if pvd[1:6] != b"CD001":
            raise SystemExit("not an ISO 9660 disc image")
        lba, size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
        parts = path.upper().split("/")
        for depth, name in enumerate(parts):
            data = self.read(lba, size)
            found = None
            o = 0
            while o < len(data):
                n = data[o]
                if n == 0:
                    o = (o // 2048 + 1) * 2048
                    continue
                rec_name = data[o + 33:o + 33 + data[o + 32]].decode("latin-1").split(";")[0]
                if rec_name.upper() == name:
                    found = (struct.unpack_from("<I", data, o + 2)[0],
                             struct.unpack_from("<I", data, o + 10)[0])
                    break
                o += n
            if not found:
                return None
            lba, size = found
        return self.read(lba, size)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    disc = Disc(sys.argv[1])
    if len(sys.argv) > 2:
        game_dir = sys.argv[2]
    elif os.path.isfile(os.path.join("build", "ToyStoryRacer_Recompiled.exe")):
        game_dir = "build"   # the release zip: the wizard builds the game there
    else:
        game_dir = "."
    out_dir = os.path.join(game_dir, "disc_override")
    ok = 0
    for rel, digest in FILES.items():
        data = disc.find(rel)
        if data is None:
            print(f"  missing on this disc: {rel}")
            continue
        if hashlib.sha256(data).hexdigest() != digest:
            print(f"  {rel}: not the USA release file (is this the USA disc?), skipped")
            continue
        dst = os.path.join(out_dir, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        print(f"  {rel} -> {dst}")
        ok += 1
    print(f"{ok} of {len(FILES)} files written to {out_dir}")
    return 0 if ok == len(FILES) else 1


if __name__ == "__main__":
    sys.exit(main())
