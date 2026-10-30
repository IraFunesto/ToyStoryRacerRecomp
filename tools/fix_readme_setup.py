#!/usr/bin/env python3
"""README-SETUP.txt of a staged kit: the standalone steps for this game (the
packager writes a generic block). Used by scripts/package_setup_release.sh."""
import re, sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
new = """Standalone (nothing to install):
1. Run ToyStoryRacer_Recompiled.
2. Provide one of your legally owned Toy Story Racer discs: European Es, It,
   Nl (SLES-03398), USA (SLUS-01214) or European Fr, De (SLES-03397) (and
   optional retail SCPH-1001 BIOS; otherwise OpenBIOS is regenerated locally).
3. Follow the Generate & rebuild wizard. The first time, the launcher downloads
   its portable toolchain (CMake, Clang and Python) from
   RetroPortingToolKit/RetroPorting-Toolchains, or you can pick a local
   cmake-clang-v1-*.zip for offline builds. No separate Python install is
   needed.
4. Your other discs: launcher -> Mods -> Set up another disc -> Play, then
   select the disc and Generate & rebuild again. Mods -> Version switches
   between PAL 50 Hz and NTSC 60 Hz.
""".replace('\n', nl)
m = re.search(r'Standalone:.*?also works if already on PATH\.' + re.escape(nl), s, re.S)
assert m, 'block not found'
s = s[:m.start()] + new + s[m.end():]
open(p, 'w', encoding='utf-8', newline='').write(s)
print('ok')
