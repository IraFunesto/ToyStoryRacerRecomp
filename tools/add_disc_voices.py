#!/usr/bin/env python3
"""Add another disc's character voices to an extracted disc folder.

Every version of the game plays a voice line as (file, XA channel) from
AUDIO/x_CHAR1..4.XA, the same lines on the same channels in every language:
x = E on the American disc (SLUS-01214), I / S / D (Italian, Spanish, Dutch)
on the European SLES-03398, F / G (French, German) on the European
SLES-03397. With the discs on the same PC, this copies the voice files the
target folder lacks from the source and lists them in the target's layout
(disc.psxdisc) after the disc's last sector; the game's file search then
finds them (the runtime looks a file up in the layout when the disc's own
directories do not hold it) and the quick start's Language option picks them.

    python tools/add_disc_voices.py --from <other disc: its tsr_data or .cue> --to <tsr_data>

The source may be an extracted folder or a disc image (a .cue with a
MODE2/2352 data track, read through its ISO 9660 directory). Nothing of any
disc leaves the PC: every file is the user's own.
"""
import argparse, os, re, struct

VOICE = re.compile(r'AUDIO/[A-Z]_CHAR[1-4][.]XA')


def layout_entries(text):
    return re.findall(r'(?m)^(raw|plain) (\d+) (\d+) (?:\S+ \S+ \S+ \S+ )?(.+)$', text)


def image_voices(cue):
    """[(path, raw sectors)] of the voice files of a disc image"""
    text = open(cue, encoding='latin1').read()
    m = re.search(r'FILE "([^"]+)"', text)
    f = open(os.path.join(os.path.dirname(cue), m.group(1)), 'rb')

    def sector(lba):
        f.seek(lba * 2352 + 24)
        return f.read(2048)

    out = []

    def walk(lba, size, path):
        data = b''.join(sector(lba + i) for i in range((size + 2047) // 2048))
        o = 0
        while o < len(data):
            n = data[o]
            if n == 0:
                o = (o // 2048 + 1) * 2048
                continue
            rec = data[o:o + n]
            o += n
            elba = struct.unpack_from('<I', rec, 2)[0]
            esize = struct.unpack_from('<I', rec, 10)[0]
            name = rec[33:33 + rec[32]]
            if name in (bytes([0]), bytes([1])):
                continue
            nm = name.decode('latin1').split(';')[0].upper()
            if rec[25] & 2:
                walk(elba, esize, path + nm + '/')
            elif VOICE.fullmatch(path + nm):
                f.seek(elba * 2352)
                out.append((path + nm, f.read(((esize + 2047) // 2048) * 2352)))

    root = sector(16)[156:156 + 34]
    walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '')
    return out


def folder_voices(folder):
    out = []
    text = open(os.path.join(folder, 'disc.psxdisc'), encoding='utf-8').read()
    for kind, lba, count, path in layout_entries(text):
        path = path.strip()
        if kind != 'raw' or not VOICE.fullmatch(path.upper()):
            continue
        data = open(os.path.join(folder, path), 'rb').read()
        if len(data) != int(count) * 2352:
            raise SystemExit('%s: %d bytes, the layout says %s raw sectors' % (path, len(data), count))
        out.append((path.upper(), data))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--from', dest='src', required=True)
    ap.add_argument('--to', dest='dst', required=True)
    args = ap.parse_args()
    dst_path = os.path.join(args.dst, 'disc.psxdisc')
    dst_layout = open(dst_path, encoding='utf-8', newline='').read()
    have = {p.strip().upper() for _, _, _, p in layout_entries(dst_layout)}
    end = int(re.search(r'(?m)^sectors (\d+)', dst_layout).group(1))
    sources = image_voices(args.src) if args.src.lower().endswith('.cue') else folder_voices(args.src)
    added = []
    for path, data in sources:
        if path in have:
            continue
        dst_file = os.path.join(args.dst, path)
        os.makedirs(os.path.dirname(dst_file), exist_ok=True)
        open(dst_file, 'wb').write(data)
        count = len(data) // 2352
        added.append('raw %d %d %s' % (end, count, path))
        end += count
    if not added:
        print('nothing to add')
        return
    nl = '\r\n' if '\r\n' in dst_layout else '\n'
    dst_layout = re.sub(r'(?m)^sectors \d+', 'sectors %d' % end, dst_layout, count=1)
    if not dst_layout.endswith(nl):
        dst_layout += nl
    dst_layout += "# another disc's character voices (tools/add_disc_voices.py)" + nl + nl.join(added) + nl
    open(dst_path, 'w', encoding='utf-8', newline='').write(dst_layout)
    print('\n'.join(added))


if __name__ == '__main__':
    main()
