#!/usr/bin/env python3
"""Turn your Muppet Monster Adventure (NTSC-U, SLUS-01238) bin/cue into the `game` folder.

    python3 tools/prepare_disc.py "Muppet Monster Adventure.cue" game

The game folder holds:
    track01.bin       the data track (raw 2352-byte sectors; the runtime reads it like a CD)
    track02.bin       the CD audio track (raw 16-bit stereo, pregap stripped)
    SLUS_012.38       the main executable (input to the recompiler)
    overlays/*.BIN    the per-level code overlays (input to the recompiler)
"""
import hashlib
import os
import re
import shutil
import struct
import sys

TRACK1_SHA1 = '6d4ad42f62c178de729967d13a2df9eead772295'
EXE_SHA1 = 'd514629b7b7aed6244b4bb0ba32331903897d484'


class Iso:
    def __init__(self, path):
        self.f = open(path, 'rb')

    def sector(self, lba):
        self.f.seek(lba * 2352)
        return self.f.read(2352)[24:24 + 2048]

    def read(self, lba, size):
        out = b''.join(self.sector(lba + i) for i in range((size + 2047) // 2048))
        return out[:size]

    def walk(self):
        pvd = self.sector(16)
        assert pvd[1:6] == b'CD001', 'track 1 is not a PlayStation data track'
        lba, size = struct.unpack('<I', pvd[158:162])[0], struct.unpack('<I', pvd[166:170])[0]
        yield from self._dir(lba, size, '')

    def _dir(self, lba, size, prefix):
        data = self.read(lba, size)
        i = 0
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            e = data[i:i + n]
            elba = struct.unpack('<I', e[2:6])[0]
            elen = struct.unpack('<I', e[10:14])[0]
            name = e[33:33 + e[32]]
            if name not in (b'\0', b'\1'):
                nm = prefix + name.decode('ascii').split(';')[0]
                if e[25] & 2:
                    yield from self._dir(elba, elen, nm + '/')
                else:
                    yield nm, elba, elen
            i += n


def parse_cue(cue):
    base = os.path.dirname(os.path.abspath(cue))
    tracks = []
    cur_file = None
    for line in open(cue, encoding='latin-1'):
        m = re.match(r'\s*FILE\s+"(.*)"', line)
        if m:
            cur_file = os.path.join(base, m.group(1))
        m = re.match(r'\s*TRACK\s+(\d+)\s+(\S+)', line)
        if m:
            tracks.append({'file': cur_file, 'mode': m.group(2), 'index': {}})
        m = re.match(r'\s*INDEX\s+(\d+)\s+(\d+):(\d+):(\d+)', line)
        if m:
            mm, ss, ff = int(m.group(2)), int(m.group(3)), int(m.group(4))
            tracks[-1]['index'][int(m.group(1))] = (mm * 60 + ss) * 75 + ff
    return tracks


def sha1(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    cue, out = sys.argv[1], sys.argv[2]
    tracks = parse_cue(cue)
    assert len(tracks) == 2 and tracks[0]['mode'] == 'MODE2/2352' and tracks[1]['mode'] == 'AUDIO', \
        'expected 2 tracks (MODE2/2352 data + AUDIO); got %r' % [t['mode'] for t in tracks]
    t1 = tracks[0]['file']
    h = sha1(t1)
    if h != TRACK1_SHA1:
        print('warning: track 1 sha1 %s does not match the NTSC-U release this project was made from (%s).'
              % (h, TRACK1_SHA1))
    os.makedirs(os.path.join(out, 'overlays'), exist_ok=True)
    shutil.copyfile(t1, os.path.join(out, 'track01.bin'))
    # Track 2: strip the pregap (INDEX 00 -> INDEX 01) so sector 0 of track02.bin is INDEX 01.
    a = tracks[1]
    pre = a['index'].get(1, 0) - a['index'].get(0, a['index'].get(1, 0))
    with open(a['file'], 'rb') as src, open(os.path.join(out, 'track02.bin'), 'wb') as dst:
        src.seek(pre * 2352)
        shutil.copyfileobj(src, dst)
    iso = Iso(t1)
    n = 0
    for name, lba, size in iso.walk():
        if name == 'SLUS_012.38':
            data = iso.read(lba, size)
            open(os.path.join(out, name), 'wb').write(data)
            if hashlib.sha1(data).hexdigest() != EXE_SHA1:
                print('warning: SLUS_012.38 does not match; symbols/addresses in this project will be wrong')
        elif name.endswith('.BIN') and name.startswith('WORLDS'):
            open(os.path.join(out, 'overlays', os.path.basename(name)), 'wb').write(iso.read(lba, size))
            n += 1
    # The runtime needs the track layout: LBA where track 2 starts on the disc (track 1 length + 2s pregap).
    t1_sectors = os.path.getsize(t1) // 2352
    with open(os.path.join(out, 'disc.txt'), 'w') as f:
        f.write('track1_sectors=%d\ntrack2_start=%d\n' % (t1_sectors, t1_sectors + pre))
    print('game folder ready: %s (%d overlays)' % (out, n))


if __name__ == '__main__':
    main()
