#!/usr/bin/env python3
"""Generate this project for another disc of the game.

This project is built for the European SLES-03398 (Es, It, Nl). The other
discs carry the same program, recompiled: the American SLUS-01214 (NTSC,
English) and the European SLES-03397 (Fr, De). About 98% of the
instructions line up, the global data moved by a few kilobytes and some code
in between grew or shrank (the PAL conversion, the language choice). This
tool aligns the two instruction streams, pairs the absolute addresses the
aligned code forms (lui + lo, jal), and rewrites every address the mods use
-- plugin C sources, game.toml, the mod manifest, seeds, symbols -- to its
twin in the target program. Recompiler and manifest code patches are re-based
on the target's bytes. The track data fix (the SLES-03398 dump's misread
values) is left out; the American build gets its own language option (its
program reads the English entry of every text table: tsr_lang_tables.h).

    python tools/make_disc_project.py --disc SLUS-01214 --exe <SLUS_012.14> --probe <dir> --out <project>
    python tools/make_disc_project.py --disc SLES-03397 --exe <SLES_033.97> --probe <dir> --out <project>

--probe is a directory written by psxrecomp/tools/new_project_layout/
probe_disc.py for the target's cue (game.toml, catalog_identity.json,
ghidra_funcs.txt, probe.json). The framework (psxrecomp, recomp-ui) is linked,
not copied, so every project builds with the same patched checkout.
"""
import argparse, bisect, difflib, json, os, re, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Addresses that are not program addresses (or need a hand decision).
FIXED = {
    0x801FFFF0: 0x801FFFF0,   # stack base
    0x80000000: 0x80000000, 0x80000120: 0x80000120, 0x80000124: 0x80000124,
}
NO_TWIN = set()

VARIANTS = {
    'SLUS-01214': {
        'exe': 'SLUS_012.14', 'title': 'Disney-Pixar Toy Story Racer (USA) Recompiled',
        'region': 'USA', 'suffix': 'USA', 'ntsc': True, 'text_tables': True,
        # The menu prompt drawer makes nine text-drawer calls in a row
        # (number, then label and glyph below, above, right, left); the US
        # code is shorter between them and the alignment slips by two calls:
        # by their order.
        'fixed': {
            0x80036188: 0x80035FE8, 0x80036218: 0x8003606C, 0x800362C8: 0x8003611C,
            0x8003635C: 0x800361AC, 0x8003640C: 0x8003625C, 0x800364A4: 0x800362EC,
            0x80036554: 0x8003639C, 0x800365EC: 0x8003642C, 0x800366A0: 0x800364DC,
        },
        # code with no twin: a value no return address can have (ra checks)
        'no_twin': {0x8003D494},
    },
    'SLES-03397': {
        'exe': 'SLES_033.97', 'title': 'Disney-Pixar Toy Story Racer (Europe) (Fr,De) Recompiled',
        'region': 'Europe', 'suffix': 'FrDe', 'ntsc': False, 'text_tables': False,
        'voices_note': "this disc has French and German voices. English voices come from the American disc, Italian, "
                       "Spanish (in part) and Dutch from the European Es, It, Nl disc, when they are installed too "
                       "(otherwise English voices if there, else the disc's own).",
        'fixed': {}, 'no_twin': set(),
    },
}


# ---- the map --------------------------------------------------------------------
def exe_words(data):
    t_addr, t_size = struct.unpack_from('<II', data, 0x18)
    return t_addr, [struct.unpack_from('<I', data, 0x800 + i)[0] for i in range(0, t_size, 4)]

def norm(w):
    op = w >> 26
    if op in (2, 3): return op << 26
    if op == 0x0F: return w & 0xFFFF0000
    if op in (1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0x20, 0x21, 0x23, 0x24, 0x25,
              0x28, 0x29, 0x2B, 0x32, 0x3A):
        return w & 0xFFFF0000
    return w

def s16(x): return x - 0x10000 if x & 0x8000 else x

def abs_refs(W, base):
    out = {}
    for i, w in enumerate(W):
        if w >> 26 == 0x0F:
            rt = (w >> 16) & 31; hi = (w & 0xFFFF) << 16
            for k in range(1, 12):
                if i + k >= len(W): break
                x = W[i + k]; op = x >> 26; rs = (x >> 21) & 31; xt = (x >> 16) & 31
                if op in (8, 9, 13, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B, 0x32, 0x3A) and rs == rt:
                    lo = (x & 0xFFFF) if op == 13 else s16(x & 0xFFFF)
                    a = (hi + lo) & 0xFFFFFFFF
                    out[i + k] = a; out.setdefault(i, a)
                    if xt == rt and op in (8, 9, 13, 0x23): break
                if op == 0x0F and xt == rt: break
        elif w >> 26 in (2, 3):
            out[i] = (base & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
    return out

def build_map(eu_exe, us_exe):
    ea, E = exe_words(open(eu_exe, 'rb').read())
    ua, U = exe_words(open(us_exe, 'rb').read())
    sm = difflib.SequenceMatcher(None, [norm(w) for w in E], [norm(w) for w in U], autojunk=False)
    blocks, changed, pairs = [], [], []
    for t, i1, i2, j1, j2 in sm.get_opcodes():
        if t == 'equal':
            blocks.append([ea + 4 * i1, ua + 4 * j1, i2 - i1])
            pairs += [(i1 + k, j1 + k) for k in range(i2 - i1)]
        elif t == 'replace' and i2 - i1 == j2 - j1:
            changed.append([ea + 4 * i1, ua + 4 * j1, i2 - i1])
            pairs += [(i1 + k, j1 + k) for k in range(i2 - i1)]
        elif i2 > i1:
            changed.append([ea + 4 * i1, None, i2 - i1])
    RE, RU = abs_refs(E, ea), abs_refs(U, ua)
    votes = {}
    for i, j in pairs:
        if i in RE and j in RU:
            k = (RE[i], RU[j]); votes[k] = votes.get(k, 0) + 1
    data = {}
    for (a, b), n in votes.items():
        if a not in data or n > data[a][1]: data[a] = (b, n)
    # the image's code ends at its last 'jr ra'; after it come the program's
    # initialised data, where the alignment of words means nothing
    code_end = max(ea + 4 * i for i, w in enumerate(E) if w == 0x03E00008) + 8
    return {'eu_text_end': ea + 4 * len(E), 'eu_code_end': code_end, 'blocks': blocks, 'changed': changed,
            'data': {'%08X' % a: '%08X' % b for a, (b, n) in data.items()}}


class Mapper:
    def __init__(self, m):
        self.blocks = sorted(m['blocks']); self.starts = [b[0] for b in self.blocks]
        self.changed = sorted(m['changed'], key=lambda b: b[0])
        self.text_end = m['eu_text_end']
        self.code_end = m['eu_code_end']
        self.data = {int(k, 16): int(v, 16) for k, v in m['data'].items()}
        self.dk = sorted(a for a in self.data if 0x80000000 <= a < 0x80200000)

    def _block(self, blocks, a):
        for b in blocks:
            if b[0] <= a < b[0] + 4 * b[2]: return b
        return None

    def map(self, a):
        """(us address, how) or (None, why)"""
        if a in FIXED: return FIXED[a], 'fixed'
        if a in NO_TWIN: return None, 'no US twin'
        if 0x80010000 <= a < self.code_end:
            i = bisect.bisect_right(self.starts, a) - 1
            if i >= 0:
                b = self.blocks[i]
                if a < b[0] + 4 * b[2]: return b[1] + (a - b[0]), 'code'
            c = self._block(self.changed, a)
            if c and c[1] is not None:
                return c[1] + (a - c[0]), 'changed code'
        if a in self.data: return self.data[a], 'ref'
        i = bisect.bisect_left(self.dk, a)
        lo = self.dk[i - 1] if i > 0 else None
        hi = self.dk[i] if i < len(self.dk) else None
        dl = self.data[lo] - lo if lo is not None else None
        dh = self.data[hi] - hi if hi is not None else None
        if dl is not None and dl == dh: return a + dl, 'between refs'
        return None, 'ambiguous (%s / %s)' % (dl, dh)


# ---- rewriting --------------------------------------------------------------------
LIT = re.compile(r'0x(80[01][0-9A-Fa-f]{5})(?![0-9A-Fa-f])')   # also 0x...u in C

def rewrite_text(text, mapper, where, report, strict=True):
    def sub(m):
        a = int(m.group(1), 16)
        u, how = mapper.map(a)
        if u is None:
            report.append('%s: 0x%08X left as is (%s)' % (where, a, how))
            if a in NO_TWIN: return '0x%08X' % (0xFFFF0000 | (a & 0xFFFF))
            return m.group(0)
        if how == 'changed code': report.append('%s: 0x%08X -> 0x%08X in changed code' % (where, a, u))
        s = '%08X' % u
        if not any(ch.isupper() for ch in m.group(1)) and any(ch.isalpha() for ch in m.group(1)): s = s.lower()
        return '0x' + s
    return LIT.sub(sub, text)

def word_at(us_data, a):
    off = 0x800 + (a - 0x80010000)
    return struct.unpack_from('<I', us_data, off)[0]

def adapt_word(E, Uw, R, mapper):
    """The patch's EU (expected E -> replacement R) carried onto the US word Uw:
    the same change to the US value, a jump to the mapped target, an absolute
    new address kept. None when the instructions differ."""
    if E == Uw: return R, None
    if norm(E) != norm(Uw): return None, 'different instruction'
    op = E >> 26
    if op in (2, 3):
        if R >> 26 in (2, 3):
            t, how = mapper.map(0x80000000 | ((R & 0x3FFFFFF) << 2))
            if t is None: return None, 'jump target unmapped'
            return (R & 0xFC000000) | ((t >> 2) & 0x3FFFFFF), 'jump target mapped'
        return R, 'replacement kept'
    if (R & 0xFFFF) == (E & 0xFFFF):   # another instruction with the same value
        return (R & 0xFFFF0000) | (Uw & 0xFFFF), 'the US value kept'
    if (R >> 16) != (E >> 16): return R, 'replacement kept'
    d = s16(R & 0xFFFF) - s16(E & 0xFFFF)
    du = s16(Uw & 0xFFFF) - s16(E & 0xFFFF)
    # an address (the value moved with the data) set to a new place far off:
    # absolute; a small change (a stride, a position, a bound) carries over
    if du and abs(d) > 0x800: return R, 'absolute address kept'
    return (R & 0xFFFF0000) | ((Uw + d) & 0xFFFF), 'same change (%+d) on the US value' % d

def fix_toml_patches(text, mapper, us_data, report):
    """[[recompiler.patch]]: expected/replacement re-based on the US word"""
    def sub(m):
        a, E, R = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)
        Uw = word_at(us_data, a)
        nr, how = adapt_word(E, Uw, R, mapper)
        if nr is None:
            report.append('game.toml patch at 0x%08X NOT ADAPTED (%s): EU 0x%08X -> 0x%08X, US 0x%08X' % (a, how, E, R, Uw))
            return m.group(0)
        if how: report.append('game.toml patch at 0x%08X: %s: 0x%08X -> 0x%08X' % (a, how, Uw, nr))
        return 'address = "0x%08X"\nexpected = "0x%08X"\nreplacement = "0x%08X"' % (a, Uw, nr)
    return re.sub(r'address = "0x([0-9A-Fa-f]{8})"\s*\nexpected = "0x([0-9A-Fa-f]{8})"\s*\n'
                  r'replacement = "0x([0-9A-Fa-f]{8})"', sub, text)

def drop_manifest_blocks(text, pred):
    """remove [[...]] blocks for which pred(block_text) is true"""
    parts = re.split(r'(?m)^(?=\[\[)', text)
    return ''.join(p for p in parts if not (p.startswith('[[') and pred(p)))

def fix_manifest(text, mapper, us_data, report, v, serial):
    # SLES-03398 only: the track data fix (feature + its disc_user patches)
    text = drop_manifest_blocks(text, lambda b: 'feature = "track-data-fix"' in b or 'id = "track-data-fix"' in b)
    text = re.sub(r'(?m)^# Track data fix:.*\n(?:#.*\n)*', '', text)
    if v.get('voices_note'):
        assert EU_VOICES_NOTE in text
        text = text.replace(EU_VOICES_NOTE, v['voices_note'])
    if v['text_tables']:
        # the language option and its choices (the option block runs until
        # the next [[option]]/[[feature]]/...): the American program's own six
        # languages, set through its text tables instead of a language byte
        text = drop_manifest_blocks(text, lambda b: 'when = { language =' in b)
        parts = re.split(r'(?m)^(?=\[\[)', text)
        out, skip = [], False
        for p in parts:
            if p.startswith('[[option]]'):
                skip = 'id = "language"' in p
                if skip: out.append(US_LANGUAGE_OPTION)
            elif not p.startswith('[[option.choice]]'): skip = False
            if not skip: out.append(p)
        text = ''.join(out)
    text = text.replace('game_id = "SLES-03398"', 'game_id = "%s"' % serial)
    if v['ntsc']:
        # the Version option starts on this program's standard
        a = 'id = "standard"'
        i = text.index(a)
        j = text.index('default = "pal"', i)
        text = text[:j] + 'default = "ntsc"' + text[j + len('default = "pal"'):]
        # NTSC: the game draws 30 frames per second, the option doubles it to 60
        for eu, us in (('50 fps', '60 fps'), ('25 fps', '30 fps'), ('draws 25 frames per second', 'draws 30 frames per second'),
                       ('falls back to 25 ', 'falls back to 30 '), ('menus stay at 25.', 'menus stay at 30.')):
            text = text.replace(eu, us)
    text = rewrite_text(text, mapper, 'manifest', report)
    # main_exe patches: re-based on the US bytes at the mapped address
    def sub(m):
        a = int(m.group(1), 16)
        exp = bytes.fromhex(m.group(2).replace(' ', '')); rep = bytes.fromhex(m.group(3).replace(' ', ''))
        off = 0x800 + (a - 0x80010000)
        got = us_data[off:off + len(exp)]
        if got == exp: return m.group(0)
        if len(exp) != 4 or len(rep) != 4:
            report.append('manifest patch at 0x%08X NOT ADAPTED (length)' % a); return m.group(0)
        E, R, Uw = (struct.unpack('<I', x)[0] for x in (exp, rep, got))
        nr, how = adapt_word(E, Uw, R, mapper)
        if nr is None:
            report.append('manifest patch at 0x%08X NOT ADAPTED (%s): EU %s -> %s, US %s' % (a, how, exp.hex(' '), rep.hex(' '), got.hex(' ')))
            return m.group(0)
        report.append('manifest patch at 0x%08X: %s: %s -> %s' % (a, how, got.hex(' '), struct.pack('<I', nr).hex(' ')))
        return '%saddress = 0x%08X\nexpected = "%s"\nreplace = "%s"' % (
            m.group(0)[:m.group(0).index('address')], a, got.hex(' '), struct.pack('<I', nr).hex(' '))
    text = re.sub(r'target = "main_exe"\s*\naddress = 0x([0-9A-Fa-f]{8})\s*\nexpected = "([0-9a-fA-F ]+)"\s*\n'
                  r'replace = "([0-9a-fA-F ]+)"', sub, text)
    return text


EU_VOICES_NOTE = ("this disc has Italian, Spanish (in part) and Dutch voices. English voices come from the American disc, "
                  "French and German from the European Fr, De disc, when they are installed too (otherwise English voices "
                  "if there, else Italian).")

US_LANGUAGE_OPTION = """[[option]]
feature = "quick-start"
id = "language"
label = "Language"
description = "The language of texts and voices. The game's program carries the texts in six languages. The voices come from the discs installed: English (this disc), Italian, Spanish (in part) and Dutch (the European Es, It, Nl disc), French and German (the European Fr, De disc); without them, English."
group = "Fixes"
type = "choice"
default = "en"

""" + "".join('[[option.choice]]\nvalue = "%s"\nlabel = "%s"\n\n' % c for c in (
    ('en', 'English'), ('fr', 'Français'), ('de', 'Deutsch'), ('it', 'Italiano'),
    ('es', 'Español'), ('nl', 'Nederlands'))) + """# The program has no language choice: the copyright notice is skipped as on
# the European program when a language is set (address mapped like the rest).
[[patch]]
feature = "quick-start"
target = "main_exe"
address = 0x800A9AE0
expected = "00"
replace = "01"                 # copyright notice already shown

"""


def text_tables(eu_data, us_data, mapper=None):
    """The American program's six-language text tables (en fr de it es nl):
    the twins of the European tables its language byte indexes (by their
    Italian text, read by American code) and every run of such tables whose
    Italian, Spanish and Dutch texts the European program has too. Returns
    [(address, English pointer)]."""
    B = 0x80010000 - 0x800
    def mk(d):
        cache = {}
        def s(a):
            if a in cache: return cache[a]
            o = a - B; r = None
            if 0x800 <= o < len(d):
                e = d.find(b'\0', o)
                if 0 <= e - o < 200:
                    t = d[o:e]
                    if t and all(32 <= c < 256 for c in t): r = t
            cache[a] = r; return r
        return s
    se, su = mk(eu_data), mk(us_data)
    def w(d, a): return struct.unpack_from('<I', d, a - B)[0]
    def groups(d, s):
        a0, Wd = exe_words(d)
        R = abs_refs(Wd, a0)
        refs = {a for i, a in R.items() if Wd[i] >> 26 != 15}
        W = [struct.unpack_from('<I', d, o)[0] for o in range(0x800, len(d) - 3, 4)]
        isp = [0x80010000 <= x < 0x800AA000 and s(x) is not None for x in W]
        out = {}; i = 0
        while i < len(W):
            if not isp[i]: i += 1; continue
            j = i
            while j < len(W) and isp[j]: j += 1
            n = j - i; start = B + 0x800 + 4 * i
            if n >= 6:
                ph = {((r - start) // 4) % 6 for r in refs if start <= r < start + 4 * n and (r - start) % 4 == 0}
                phases = [ph.pop()] if len(ph) == 1 else [0] if (not ph and n % 6 == 0) else list(range(min(6, n - 5)))
                for p in phases:
                    for k in range(p, n - 5, 6):
                        out[start + 4 * k] = [s(W[i + k + q]) for q in range(6)]
            i = j
        return out, refs
    ge, refe = groups(eu_data, se)
    gu, refu = groups(us_data, su)
    # two of the Italian, Spanish and Dutch texts the same (the American
    # program's translations were revised here and there)
    keys = set()
    for t in ge.values(): keys |= {('ie', t[3], t[4]), ('in', t[3], t[5]), ('en', t[4], t[5])}
    def known(t): return bool({('ie', t[3], t[4]), ('in', t[3], t[5]), ('en', t[4], t[5])} & keys)
    def text_like(t):   # game texts are upper case (format codes apart)
        return not any(re.search(rb'[a-z]', re.sub(rb'%[a-z]', b'', x)) for x in t)
    sel = {a for a, t in gu.items() if known(t) and t[0] != t[3] and text_like(t) and len(t[0]) > 1}
    # the twins, by Italian text, of the tables the European language byte indexes
    a0, E = exe_words(eu_data)
    RE = abs_refs(E, a0)
    lang = set()
    for i, a in RE.items():
        if a == 0x800A9820 and E[i] >> 26 != 15:
            for k in range(1, 8):
                ix = i + k
                if ix in RE and E[ix] >> 26 == 0x23:
                    if 0x80010000 <= RE[ix] < 0x800AA000 - 24: lang.add(RE[ix])
                    break
    for t in lang:
        v = [se(w(eu_data, t + 4 * j)) for j in range(6)]
        if None in v:
            # six pointers to data (the copyright pages, rows of text and
            # position): the American twin through the address map, when it
            # holds six distinct pointers into the program too
            if mapper is None: continue
            u, how = mapper.map(t)
            if u is None or how not in ('ref', 'between refs'): continue
            p = [w(us_data, u + 4 * j) for j in range(6)]
            if all(0x80010000 <= x < 0x800AA000 for x in p) and len(set(p)) == 6: sel.add(u)
            continue
        for a, u in gu.items():
            if u[3] == v[3] and a in refu and text_like(u) and u[0] != u[3]: sel.add(a)
    return sorted((a, w(us_data, a)) for a in sel)


TEXT_EXT = ('.c', '.h', '.toml', '.json', '.md', '.txt', '.py', '.cmake', '.sh', '.bat', '.ps1', '.ini', '.cfg')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disc', required=True, choices=sorted(VARIANTS))
    ap.add_argument('--exe', required=True)
    ap.add_argument('--probe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rebuild-map', action='store_true')
    # --kit: the project for the release kit's versions/<name> (sources only:
    # no executable of the disc, no framework links -- the setup makes them --
    # and the memory cards of the install root)
    ap.add_argument('--kit', action='store_true')
    args = ap.parse_args()
    serial = args.disc
    v = VARIANTS[serial]
    exe = v['exe']
    FIXED.update(v['fixed'])
    NO_TWIN.update(v['no_twin'])
    map_file = os.path.join(HERE, 'map_%s.json' % serial)
    eu_exe = os.path.join(ROOT, 'disc', 'SLES_033.98')
    if args.rebuild_map or not os.path.exists(map_file):
        print('aligning the executables (a minute or two)...')
        json.dump(build_map(eu_exe, args.exe), open(map_file, 'w'))
    mapper = Mapper(json.load(open(map_file)))
    us_data = open(args.exe, 'rb').read()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    report = []

    files = subprocess.run(['git', 'ls-files'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split('\n')
    skip_prefix = ('psxrecomp', 'recomp-ui', 'patches/', '.github/', 'docs/', 'tools/make_disc_project.py',
                   'tools/map_', 'tools/sync_disc_project.py', 'versions/')
    for f in files:
        if not f or f.startswith(skip_prefix): continue
        src = os.path.join(ROOT, f); dst = os.path.join(out, f)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not f.endswith(TEXT_EXT):
            shutil.copy2(src, dst); continue
        raw = open(src, 'rb').read()
        crlf = b'\r\n' in raw
        text = raw.decode('utf-8').replace('\r\n', '\n')
        if f.endswith('manifest.toml') and 'toystoryracer.widescreen' in f:
            text = fix_manifest(text, mapper, us_data, report, v, serial)
        elif f == 'seeds/ghidra_funcs.txt':
            eu = {int(x, 16) for x in re.findall(r'(?m)^0x([0-9A-Fa-f]{8})', text)}
            us = {int(x, 16) for x in re.findall(r'(?m)^0x([0-9A-Fa-f]{8})', open(os.path.join(args.probe, 'ghidra_funcs.txt')).read())}
            for a in eu:
                u, how = mapper.map(a)
                if u is not None and how in ('code', 'fixed'): us.add(u)
            text = ('# Seeds for %s: its JAL scan plus the SLES-03398 seeds mapped to it\n' % exe +
                    ''.join('0x%08x\n' % a for a in sorted(us)))
        elif f == 'catalog_identity.json':
            text = open(os.path.join(args.probe, 'catalog_identity.json'), encoding='utf-8').read()
        elif f == 'disc_probe.json':
            text = open(os.path.join(args.probe, 'probe.json'), encoding='utf-8').read()
        else:
            text = rewrite_text(text, mapper, f, report)
        text = text.replace('SLES_033.98', exe).replace('SLES-03398', serial)
        if f == 'game.toml':
            text = us_game_toml(text, args.probe, v['title'])
            text = fix_toml_patches(text, mapper, us_data, report)
            if args.kit:
                text = re.sub(r'(?m)^memcard_dir = "saves"', 'memcard_dir = "../../saves"', text)
        if f == '.recomp.json':
            text = text.replace('"Europe"', '"%s"' % v['region']).replace(
                'ToyStoryRacerRecomp"', 'ToyStoryRacerRecomp%s"' % v['suffix'])
        if crlf: text = text.replace('\n', '\r\n')
        open(dst, 'wb').write(text.encode('utf-8'))

    # the framework: links to this project's patched checkouts
    for d in ('psxrecomp', 'recomp-ui'):
        dst = os.path.join(out, d)
        if not args.kit and not os.path.exists(dst):
            if os.name == 'nt':
                subprocess.run(['cmd', '/c', 'mklink', '/J', dst, os.path.join(ROOT, d)], check=True, capture_output=True)
            else:
                os.symlink(os.path.join(ROOT, d), dst)
    os.makedirs(os.path.join(out, 'disc'), exist_ok=True)
    if not args.kit:
        shutil.copy2(args.exe, os.path.join(out, 'disc', exe))
    tables = text_tables(open(eu_exe, "rb").read(), us_data, mapper) if v['text_tables'] else []
    hdr = open(os.path.join(ROOT, 'plugins', 'tsr_lang_tables.h'), encoding='utf-8').read()
    body = ''.join('    { 0x%08Xu, 0x%08Xu },\n' % t for t in tables)
    old = '#define TSR_LANG_TABLE_COUNT 0\nstatic const TsrLangTable TSR_LANG_TABLES[1] = { { 0u, 0u } };'
    assert old in hdr
    hdr = hdr.replace(old, '#define TSR_LANG_TABLE_COUNT %d\nstatic const TsrLangTable TSR_LANG_TABLES[%d] = {\n%s};'
                      % (len(tables), len(tables), body))
    open(os.path.join(out, 'plugins', 'tsr_lang_tables.h'), 'w', encoding='utf-8').write(hdr)
    report.append('%d text language tables' % len(tables))
    open(os.path.join(out, 'DISC_REPORT.txt'), 'w').write('\n'.join(report) + '\n')
    print('\n'.join(report))
    print('%d notes; project in %s' % (len(report), out))

def us_game_toml(text, probe_dir, title):
    p = open(os.path.join(probe_dir, 'game.toml'), encoding='utf-8').read()
    def section(t, name):
        m = re.search(r'(?ms)^\[%s\]\n.*?(?=^\[|\Z)' % re.escape(name), t)
        return m.group(0) if m else None
    # identity from the probe: [game], [prepare_disc], the netplay fingerprint
    for name in ('game', 'prepare_disc'):
        new = section(p, name)
        if new: text = text.replace(section(text, name), new.rstrip('\n') + '\n\n')
    fp = re.search(r'required_disc_fp = "([0-9a-f]+)"', p)
    if fp: text = re.sub(r'required_disc_fp = "[0-9a-f]+"', 'required_disc_fp = "%s"' % fp.group(1), text)
    text = re.sub(r'window_title = "[^"]*"', 'window_title = "%s"' % title, text)
    return text

if __name__ == '__main__':
    main()
