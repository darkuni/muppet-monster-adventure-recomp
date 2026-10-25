#!/usr/bin/env python3
"""dis.py <game dir> <start> <end|+count> [overlay.BIN]  - disassemble main exe (or an overlay at 0x800DEBB4)"""
import sys, struct, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))
from mips import decode, disasm
OVL_BASE = 0x800DEBB4
g = sys.argv[1]
if len(sys.argv) > 4:
    data = open(sys.argv[4], 'rb').read(); base = OVL_BASE
else:
    data = open(os.path.join(g, 'SLUS_012.38'), 'rb').read()[0x800:]; base = 0x80010000
s = int(sys.argv[2], 16)
e = s + int(sys.argv[3][1:]) * 4 if sys.argv[3].startswith('+') else int(sys.argv[3], 16)
for pc in range(s & ~3, e, 4):
    off = pc - base
    if off < 0 or off + 4 > len(data): continue
    w = struct.unpack('<I', data[off:off + 4])[0]
    print('%08x  %08x  %s' % (pc, w, disasm(decode(w, pc))))
