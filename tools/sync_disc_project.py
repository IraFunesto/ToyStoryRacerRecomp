#!/usr/bin/env python3
"""Bring files of this project into a generated disc project (made by
make_disc_project.py) the way the generator does: addresses rewritten for
that disc's program. Used after editing a plugin here.

    python tools/sync_disc_project.py --disc SLES-03397 --out <project> plugins/tsr_disc_files.c ...
"""
import argparse, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_disc_project as m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disc', required=True, choices=sorted(m.VARIANTS))
    ap.add_argument('--out', required=True)
    ap.add_argument('files', nargs='+')
    args = ap.parse_args()
    v = m.VARIANTS[args.disc]
    m.FIXED.update(v['fixed'])
    m.NO_TWIN.update(v['no_twin'])
    mapper = m.Mapper(json.load(open(os.path.join(m.HERE, 'map_%s.json' % args.disc))))
    for f in args.files:
        f = f.replace(os.sep, '/')
        if not f.startswith('plugins/'):
            raise SystemExit('%s: only plugins/ files are synced' % f)
        raw = open(os.path.join(m.ROOT, f), 'rb').read()
        crlf = b'\r\n' in raw
        report = []
        text = m.rewrite_text(raw.decode('utf-8').replace('\r\n', '\n'), mapper, f, report)
        text = text.replace('SLES_033.98', v['exe']).replace('SLES-03398', args.disc)
        if crlf: text = text.replace('\n', '\r\n')
        open(os.path.join(args.out, f), 'wb').write(text.encode('utf-8'))
        print(f, *report, sep='\n  ')


if __name__ == '__main__':
    main()
