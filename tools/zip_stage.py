#!/usr/bin/env python3
"""Zip a staged release kit (scripts/package_setup_release.sh).

    python tools/zip_stage.py <stage dir> <zip>

Files only, paths relative to the stage, deflated -- the layout of the
packager's `zip -r -q`. Refuses a stage holding disc data (extracted discs,
disc images, generated game code, memory cards outside the framework's tests).
"""
import os, re, sys, zipfile

BAD = re.compile(r'^(versions/[^/]+/)?(tsr_data|generated|build[^/]*|disc|saves)/'
                 r'|\.(bin|cue|chd|iso|img|pst)$|(^|/)SL[EU]S_\d{3}\.\d{2}$', re.I)
ALLOWED = ('psxrecomp/bios/openbios.bin',)


def main():
    stage, out = sys.argv[1], sys.argv[2]
    names = []
    for root, dirs, files in os.walk(stage):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(root, f)
            rel = os.path.relpath(full, stage).replace(os.sep, '/')
            if BAD.search(rel) and rel not in ALLOWED and not rel.startswith('psxrecomp/recompiler/lib/'):
                raise SystemExit('refusing to ship %s' % rel)
            names.append((full, rel))
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for full, rel in names:
            z.write(full, rel)
    print('%s: %d files, %d bytes' % (out, len(names), os.path.getsize(out)))


if __name__ == '__main__':
    main()
