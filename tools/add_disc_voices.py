#!/usr/bin/env python3
"""Add the other disc's character voices to an extracted disc folder.

Both the European (SLES-03398) and the American (SLUS-01214) game play a
voice line as (file, XA channel) from AUDIO/x_CHAR1..4.XA: x = E on the
American disc, I / S / D (Italian, Spanish, Dutch) on the European one, the
same lines on the same channels in every language. With both discs extracted
on the same PC, this copies the voice files the target folder lacks from the
source folder and lists them in the target's layout (disc.psxdisc) after the
disc's last sector; the game's file search then finds them (the runtime looks
a file up in the layout when the disc's own directories do not hold it) and
the quick start's Language option picks them.

    python tools/add_disc_voices.py --from <tsr_data of the other disc> --to <tsr_data>

Nothing of either disc leaves the PC: both folders are the user's own.
"""
import argparse, os, re, shutil

def layout_entries(text):
    return re.findall(r'(?m)^(raw|plain) (\d+) (\d+) (?:\S+ \S+ \S+ \S+ )?(.+)$', text)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--from', dest='src', required=True)
    ap.add_argument('--to', dest='dst', required=True)
    args = ap.parse_args()
    src_layout = open(os.path.join(args.src, 'disc.psxdisc'), encoding='utf-8').read()
    dst_path = os.path.join(args.dst, 'disc.psxdisc')
    dst_layout = open(dst_path, encoding='utf-8', newline='').read()
    have = {p.strip().upper() for _, _, _, p in layout_entries(dst_layout)}
    m = re.search(r'(?m)^sectors (\d+)', dst_layout)
    end = int(m.group(1))
    added = []
    for kind, lba, count, path in layout_entries(src_layout):
        path = path.strip()
        if kind != 'raw' or not re.fullmatch(r'AUDIO/[A-Z]_CHAR[1-4]\.XA', path.upper()) or path.upper() in have:
            continue
        count = int(count)
        src_file = os.path.join(args.src, path)
        if os.path.getsize(src_file) != count * 2352:
            raise SystemExit('%s: %d bytes, the layout says %d raw sectors' % (src_file, os.path.getsize(src_file), count))
        dst_file = os.path.join(args.dst, path)
        os.makedirs(os.path.dirname(dst_file), exist_ok=True)
        shutil.copyfile(src_file, dst_file)
        added.append('raw %d %d %s' % (end, count, path))
        end += count
    if not added:
        print('nothing to add'); return
    nl = '\r\n' if '\r\n' in dst_layout else '\n'
    dst_layout = re.sub(r'(?m)^sectors \d+', 'sectors %d' % end, dst_layout, count=1)
    if not dst_layout.endswith(nl): dst_layout += nl
    dst_layout += '# the other disc\'s character voices (tools/add_disc_voices.py)' + nl + nl.join(added) + nl
    open(dst_path, 'w', encoding='utf-8', newline='').write(dst_layout)
    print('\n'.join(added))

if __name__ == '__main__':
    main()
