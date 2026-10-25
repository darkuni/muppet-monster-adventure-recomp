#!/usr/bin/env python3
"""Reference STR/BS (v1/v2/v3) Huffman decoder -> MDEC run-length halfwords (psx-spx).
Used to verify the recompiled libpress decoder.

    python3 bs_decode.py movie.str frame_number [out.bin]
"""
import struct, sys

AC = [
    ('10', None, [0xFE00]),
    ('11', 's', [0x0001]),
    ('011', 's', [0x0401]),
    ('010', 'xs', [0x0002, 0x0801]),
    ('0011', 'xs', [0x1001, 0x0C01]),
    ('00101', 's', [0x0003]),
    ('00100', 'xxxs', [0x3401, 0x0006, 0x3001, 0x2C01, 0x0C02, 0x0403, 0x0005, 0x2801]),
    ('0001', 'xxs', [0x1C01, 0x1801, 0x0402, 0x1401]),
    ('00001', 'xxs', [0x0802, 0x2401, 0x0004, 0x2001]),
    ('0000001', 'xxxs', [0x4001, 0x1402, 0x0007, 0x0803, 0x0404, 0x3C01, 0x3801, 0x1002]),
    ('00000001', 'xxxxs', [0x000B, 0x2002, 0x1003, 0x000A, 0x0804, 0x1C02, 0x5401, 0x5001,
                           0x0009, 0x4C01, 0x4801, 0x0405, 0x0C03, 0x0008, 0x1802, 0x4401]),
    ('000000001', 'xxxxs', [0x2802, 0x2402, 0x1403, 0x0C04, 0x0805, 0x0407, 0x0406, 0x000F,
                            0x000E, 0x000D, 0x000C, 0x6801, 0x6401, 0x6001, 0x5C01, 0x5801]),
    ('0000000001', 'xxxxs', [0x001F, 0x001E, 0x001D, 0x001C, 0x001B, 0x001A, 0x0019, 0x0018,
                             0x0017, 0x0016, 0x0015, 0x0014, 0x0013, 0x0012, 0x0011, 0x0010]),
    ('00000000001', 'xxxxs', [0x0028, 0x0027, 0x0026, 0x0025, 0x0024, 0x0023, 0x0022, 0x0021,
                              0x0020, 0x040E, 0x040D, 0x040C, 0x040B, 0x040A, 0x0409, 0x0408]),
    ('000000000001', 'xxxxs', [0x0412, 0x0411, 0x0410, 0x040F, 0x1803, 0x4002, 0x3C02, 0x3802,
                               0x3402, 0x3002, 0x2C02, 0x7C01, 0x7801, 0x7401, 0x7001, 0x6C01]),
]

class Bits:
    def __init__(self, data):
        self.h = struct.unpack('<%dH' % (len(data) // 2), data[:len(data) // 2 * 2])
        self.pos = 0
    def peek(self, n):
        v = 0
        for i in range(n):
            p = self.pos + i
            w = self.h[p >> 4] if (p >> 4) < len(self.h) else 0
            v = (v << 1) | ((w >> (15 - (p & 15))) & 1)
        return v
    def get(self, n):
        v = self.peek(n); self.pos += n; return v
    def match(self, prefix):
        n = len(prefix)
        return self.peek(n) == int(prefix, 2)

DC_C = [(2, 0, 0), (3, 1, 1), (4, 2, 2), (6, 6, 3), (8, 14, 4), (10, 30, 5), (12, 62, 6), (14, 126, 7), (16, 254, 8)]

def dc_v3(b, chroma):
    # (prefix bits, prefix value, magnitude bits)
    if b.peek(10) == 0x3FF:
        return None
    if chroma:
        table = [('00', 0), ('01', 1), ('10', 2), ('110', 3), ('1110', 4), ('11110', 5), ('111110', 6), ('1111110', 7), ('11111110', 8)]
    else:
        table = [('100', 0), ('00', 1), ('01', 2), ('101', 3), ('110', 4), ('1110', 5), ('11110', 6), ('111110', 7), ('1111110', 8)]
    for pre, nbits in table:
        if b.match(pre):
            b.pos += len(pre)
            if nbits == 0:
                return 0
            v = b.get(nbits)
            if v & (1 << (nbits - 1)):
                return v * 4
            return (v - (1 << nbits) + 1) * 4
    raise ValueError('bad dc code at bit %d' % b.pos)

def decode_frame(bs):
    size4, fid, quant, ver = struct.unpack_from('<4H', bs, 0)
    assert fid == 0x3800, hex(fid)
    b = Bits(bs[8:])
    out = []
    old = [0, 0, 0]
    blk = 0
    while True:
        kind = blk % 6   # 0 Cr, 1 Cb, 2-5 Y
        if ver == 3:
            d = dc_v3(b, kind < 2)
            if d is None:
                break
            idx = kind if kind < 2 else 2
            dc = old[idx] + d
            old[idx] = dc
            out.append(((quant << 10) | (dc & 0x3FF)) & 0xFFFF)
        else:
            dc = b.get(10)
            if dc == 0x1FF:
                break
            out.append((quant << 10) | dc)
        while True:
            if b.match('000001'):
                b.pos += 6
                out.append(b.get(16))
                continue
            for pre, extra, vals in AC:
                if b.match(pre):
                    b.pos += len(pre)
                    if vals[0] == 0xFE00:
                        out.append(0xFE00)
                        break
                    x = 0
                    nx = extra.count('x')
                    if nx:
                        x = b.get(nx)
                    v = vals[x]
                    if b.get(1):
                        v = (v & 0xFC00) | ((-(v & 0x3FF)) & 0x3FF)
                    out.append(v)
                    break
            else:
                raise ValueError('bad ac code at bit %d (block %d)' % (b.pos, blk))
            if out[-1] == 0xFE00:
                break
        blk += 1
    return size4, quant, ver, out

def frames(path):
    d = open(path, 'rb').read()
    fr = {}
    for s in range(len(d) // 2352):
        sec = d[s * 2352:(s + 1) * 2352]
        sub = sec[16:24]
        if not (sub[2] & 0x08):
            continue   # not a data sector
        user = sec[24:24 + 2048]
        st, typ, idx, cnt, fno, fsize = struct.unpack_from('<HHHHII', user, 0)
        if st != 0x0160 or typ != 0x8001:
            continue
        fr.setdefault(fno, {})[idx] = (cnt, fsize, user[0x20:])
    out = {}
    for fno, parts in fr.items():
        cnt, fsize, _ = next(iter(parts.values()))
        if len(parts) != cnt:
            continue
        out[fno] = b''.join(parts[i][2] for i in range(cnt))[:fsize]
    return out

if __name__ == '__main__':
    f = frames(sys.argv[1])
    n = int(sys.argv[2])
    size4, quant, ver, codes = decode_frame(f[n])
    print('frame %d: %d bytes, mdec size %d words, quant %d, version %d, %d halfwords, %d blocks' %
          (n, len(f[n]), size4 * 2, quant, ver, len(codes), codes.count(0xFE00)))
    print(' '.join('%04x' % c for c in codes[:48]))
    if len(sys.argv) > 3:
        open(sys.argv[3], 'wb').write(struct.pack('<%dH' % len(codes), *codes))
