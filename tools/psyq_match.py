#!/usr/bin/env python3
"""Name PsyQ SDK functions in the executable using lab313ru/psx_psyq_signatures.

    python3 tools/psyq_match.py <psx_psyq_signatures dir> game/SLUS_012.38 [version] > config/psyq.txt

Without a version, every SDK version is scored and the best one is used.
Output: one 'ADDR NAME LIB/OBJ' line per label (sorted by address).
"""
import glob
import json
import os
import re
import sys

BASE = 0x80010000


def load(sigdir, ver):
    sigs = []
    for path in glob.glob(os.path.join(sigdir, ver, '*.json')):
        lib = os.path.basename(path)[:-5]
        for obj in json.load(open(path)):
            if 'sig' not in obj:
                continue
            toks = obj['sig'].split()
            fixed = sum(1 for t in toks if t != '??')
            if fixed < 20:
                continue
            pat = b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in toks)
            sigs.append((lib + '/' + obj['name'], re.compile(pat, re.S), obj.get('labels', []), fixed))
    return sigs


def match(code, sigs):
    hits = []
    for name, rx, labels, fixed in sigs:
        ms = [m.start() for m in rx.finditer(code)]
        if len(ms) != 1:
            continue
        off = ms[0]
        if off % 4:
            continue
        hits.append((off, name, labels, fixed))
    return hits


def main():
    sigdir, exe = sys.argv[1], sys.argv[2]
    code = open(exe, 'rb').read()[0x800:]
    if len(sys.argv) > 3:
        best = sys.argv[3]
    else:
        scores = []
        for v in sorted(os.listdir(sigdir)):
            if not v.isdigit():
                continue
            hits = match(code, load(sigdir, v))
            scores.append((sum(h[3] for h in hits), len(hits), v))
            print('version %s: %d objects, %d fixed bytes' % (v, len(hits), scores[-1][0]), file=sys.stderr)
        best = max(scores)[2]
        print('best single version: %s (union of all versions is used)' % best, file=sys.stderr)
    out = {}
    vers = [best] if len(sys.argv) > 3 else sorted((v for v in os.listdir(sigdir) if v.isdigit()), key=int)
    hits = []
    for v in vers:  # union over versions; later (newer) versions win on conflicts
        hits += match(code, load(sigdir, v))
    for off, name, labels, fixed in hits:
        for lab in labels:
            n = lab['name']
            if n.startswith('loc_') or n.startswith('text_') or n.startswith('jpt_') or n.startswith('def_'):
                continue
            out[BASE + off + lab['offset']] = (n, name)
    print('# PsyQ library functions found by signature (union of SDK versions)')
    for a in sorted(out):
        print('%08x %s %s' % (a, out[a][0], out[a][1]))


if __name__ == '__main__':
    main()
