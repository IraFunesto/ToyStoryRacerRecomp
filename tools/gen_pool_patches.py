"""Generate the near-polygon pool patches (widescreen mod, Toy Story Racer).

The clippers take 52-byte pieces from a u16 index list (index = n*13, a word
offset; bit 15 is a flag, so at most ~2520 pieces) and address them as
base + index*4. The "stride" change makes pieces 56 bytes (index = n*7,
address = base + index*8) so up to 4681 pieces fit in 15 bits. It touches hot
code, so it is compiled in ([[recompiler.patch]]) and applied to RAM by an
always-on hidden feature; the pool size and place (rarely run code) are
option patches.

  python tools/gen_pool_patches.py --recompiler     -> [[recompiler.patch]] blocks
  python tools/gen_pool_patches.py --size WHEN N LISTBASE BUFA|- BUFB|-
                                                    -> option [[patch]] blocks

Every expected word is checked against the stock executable.
"""
import struct
import sys

EXE = open('disc/SLES_033.98', 'rb').read()


def word(a):
    return struct.unpack_from('<I', EXE, a - 0x80010000 + 0x800)[0]


def le(w):
    return " ".join("%02x" % b for b in struct.pack('<I', w))


# Piece allocations from the free list (one piece; the second of a pair is
# re-read from the object's list and covered by the "+s3" sites).
ALLOC = [0x80012c30, 0x80012fdc, 0x8001334c, 0x80013668, 0x800139d4, 0x80013ad8, 0x800142e8,
         0x800143ec, 0x80014c54, 0x80014d90, 0x80015778, 0x800158b4, 0x800161f8, 0x80016648,
         0x80016adc, 0x8001703c, 0x8001dbd8, 0x8001dfe4, 0x8001e3c4, 0x8001e740, 0x8001ea70,
         0x8001ee0c, 0x8001f1b4, 0x8001f530, 0x8001f848, 0x8001fb24]
# Index * 4 added to the previous-frame buffer (object lists, children).
S3 = [int(l.split()[0], 16) for l in open('tools/pool_s3_sites.txt') if l.strip()]
# Four-child allocations (index * 4 added 17 instructions later).
QUAD = [int(l, 16) for l in open('tools/pool_quad_sites.txt') if l.strip()]
# Recursive free routines (entry + children).
FREE = [0x8001D408, 0x8001d4bc, 0x8001d4c8, 0x8001d4e4, 0x8001d500,
        0x8001d5e4, 0x8001d5f0, 0x8001d60c, 0x8001d628]
SCALE = sorted(set(ALLOC + S3 + QUAD + FREE))

# Two list initialisers: (lui v0, addiu v0, addiu end, x13 multiply, slti count, lui at, sh terminator)
INITS = ((0x800408D4, 0x800408D8, 0x800408E0, 0x80040900, 0x80040914, 0x80040920, 0x80040924),
         (0x8004CA54, 0x8004CA58, 0x8004CA60, 0x8004CA9C, 0x8004CAB0, 0x8004CAD0, 0x8004CAD4))
# Buffer flips that load the two piece buffers (lui/addiu pairs).
FLIPS = (0x80049238, 0x80049248, 0x80049258, 0x80049268, 0x8005D270, 0x8005D27C, 0x8005D28C,
         0x8005D298, 0x8005D5D8, 0x8005D5E4, 0x8005D5F4, 0x8005D600, 0x8005DA74, 0x8005DA80,
         0x8005DA90, 0x8005DA9C)


def checked(a, exp, new, cmt):
    assert word(a) == exp, "%08X: %08X != %08X" % (a, word(a), exp)
    return (a, exp, new, cmt)


def stride_words():
    out = []
    for init in INITS:
        mul = init[3]
        out += [checked(mul, 0x00031040, 0x000310C0, "sll v0,v1,3"),
                checked(mul + 4, 0x00431021, 0x00431023, "subu v0,v0,v1  (index = n*7)"),
                checked(mul + 8, 0x00021080, 0x00000000, "nop"),
                checked(mul + 12, 0x00431021, 0x00000000, "nop")]
    for a in SCALE:
        w = word(a)
        assert (w >> 26) == 0 and (w & 63) == 0 and ((w >> 6) & 31) == 2, hex(a)
        out.append(checked(a, w, w + 0x40, "sll ...,3 (piece = base + index*8)"))
    return out


def size_words(n, listbase, bufa, bufb):
    out = []
    end = 2 * n
    hi, lo = listbase >> 16, listbase & 0xFFFF
    for lui, adu, ende, mul, slti, luiat, sh in INITS:
        if listbase != 0x800D3178:
            out += [checked(lui, 0x3C02800D, 0x3C020000 | hi, "lui v0,0x%04X (index list)" % hi),
                    checked(adu, 0x24423178, 0x24420000 | lo, "addiu v0,v0,0x%04X" % lo),
                    checked(luiat, 0x3C01800D, 0x3C010000 | hi, "lui at,0x%04X" % hi)]
        out += [checked(ende, 0x24A2138A, 0x24A20000 | (end + 2), "addiu v0,a1,%d (list end)" % (end + 2)),
                checked(slti, 0x286209C4, 0x28620000 | n, "slti v0,v1,%d (pieces)" % n),
                checked(sh, 0xA4204500, 0xA4200000 | ((lo + end) & 0xFFFF),
                        "sh zero,0x%04X(at) (terminator)" % ((lo + end) & 0xFFFF))]
    if listbase != 0x800D3178:
        out.append(checked(0x8004918C, 0x3C03800D, 0x3C030000 | hi, "lui v1,0x%04X (free loop)" % hi))
    out.append(checked(0x80049190, 0x24634502, 0x24630000 | ((lo + end + 2) & 0xFFFF),
                       "addiu v1,v1,0x%04X (list end)" % ((lo + end + 2) & 0xFFFF)))
    if bufa:
        for a in FLIPS:
            if word(a) == 0x3C02801A:
                out += [checked(a, 0x3C02801A, 0x3C020000 | (bufa >> 16), "lui v0,0x%04X (piece buffer A)" % (bufa >> 16)),
                        checked(a + 4, 0x244257D0, 0x24420000 | (bufa & 0xFFFF), "addiu v0,v0,0x%04X" % (bufa & 0xFFFF))]
            else:
                out += [checked(a, 0x3C02801D, 0x3C020000 | (bufb >> 16), "lui v0,0x%04X (piece buffer B)" % (bufb >> 16)),
                        checked(a + 4, 0x2442768C, 0x24420000 | (bufb & 0xFFFF), "addiu v0,v0,0x%04X" % (bufb & 0xFFFF))]
    return out


def manifest_blocks(words, feature, when=None):
    text = ""
    for a, o, n, c in words:
        text += ('\n[[patch]]\nfeature = "%s"\ntarget = "main_exe"\naddress = 0x%08X\n'
                 'expected = "%s"\nreplace = "%s"        # %s\n') % (feature, a, le(o), le(n), c)
        if when:
            text += 'when = { polygon_pool = "%s" }\n' % when
    return text


def mult_words():
    return [w for w in stride_words() if any(init[3] <= w[0] < init[3] + 16 for init in INITS)]


def baked_words():
    """Compiled in: the stride plus the Standard size (2300 in the stock buffers),
    so the native code is consistent even with the mod switched off."""
    return stride_words() + size_words(2300, 0x800D3178, None, None)


def main(argv):
    if argv[1] == '--recompiler':
        for a, o, n, c in baked_words():
            print('[[recompiler.patch]]\nid = "pool-stride-%08X"\naddress = "0x%08X"\n'
                  'expected = "0x%08X"\nreplacement = "0x%08X"\nnote = "%s"\n' % (a, a, o, n, c))
    elif argv[1] == '--stride':
        print(manifest_blocks(stride_words(), "pool-stride"))
    elif argv[1] == '--size':
        # RAM patches for a non-Standard pool. RAM keeps the stock image (the
        # runtime runs native code only while RAM matches it), so the patched
        # functions run interpreted; they are the rarely run list initialisers,
        # free loop and buffer flips. The initialisers also need the x7 index.
        when, n, lb, a, b = argv[2:7]
        print(manifest_blocks(mult_words() + size_words(int(n), int(lb, 16),
                                         None if a == '-' else int(a, 16),
                                         None if b == '-' else int(b, 16)), "widescreen", when))


if __name__ == '__main__':
    main(sys.argv)
