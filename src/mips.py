"""MIPS R3000A (PlayStation) instruction decoder.

decode(word, pc) -> Insn with fields used by both the disassembler and the recompiler.
"""
import struct

REG = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
       't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
       't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


def sx16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


class Insn:
    __slots__ = ('pc', 'word', 'op', 'rs', 'rt', 'rd', 'sa', 'imm', 'simm', 'target',
                 'mn', 'kind', 'cop')

    def __init__(self, pc, word):
        self.pc = pc
        self.word = word
        self.op = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.imm = word & 0xffff
        self.simm = sx16(word)
        self.target = None
        self.mn = '???'
        self.kind = 'alu'   # alu, load, store, branch, jump, jr, jalr, j, jal, sys, cop, invalid
        self.cop = None

    # branch/jump helpers
    @property
    def is_branch(self):
        return self.kind == 'branch'

    @property
    def has_delay(self):
        return self.kind in ('branch', 'j', 'jal', 'jr', 'jalr')

    @property
    def is_link(self):
        return self.mn in ('jal', 'jalr', 'bltzal', 'bgezal')

    def __repr__(self):
        return '%08x: %s' % (self.pc, disasm(self))


_SPECIAL = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr',
            12: 'syscall', 13: 'break', 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo',
            24: 'mult', 25: 'multu', 26: 'div', 27: 'divu',
            32: 'add', 33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor',
            42: 'slt', 43: 'sltu'}
_OPS = {2: 'j', 3: 'jal', 4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz',
        8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori', 15: 'lui',
        32: 'lb', 33: 'lh', 34: 'lwl', 35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr',
        40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw', 46: 'swr',
        50: 'lwc2', 58: 'swc2'}
LOADS = {'lb', 'lh', 'lwl', 'lw', 'lbu', 'lhu', 'lwr'}
STORES = {'sb', 'sh', 'swl', 'sw', 'swr'}


def decode(word, pc):
    i = Insn(pc, word)
    op = i.op
    if op == 0:
        f = word & 63
        mn = _SPECIAL.get(f)
        if mn is None:
            i.kind = 'invalid'
            return i
        i.mn = mn
        if mn == 'jr':
            i.kind = 'jr'
        elif mn == 'jalr':
            i.kind = 'jalr'
        elif mn in ('syscall', 'break'):
            i.kind = 'sys'
    elif op == 1:
        rt = i.rt
        mn = {0: 'bltz', 1: 'bgez', 16: 'bltzal', 17: 'bgezal'}.get(rt)
        if mn is None:
            # R3000 decodes only bits 0 and 4 of rt for REGIMM; others alias
            mn = ('bgez' if rt & 1 else 'bltz')
            if (rt & 0x1e) == 0x10:
                mn += 'al'
        i.mn = mn
        i.kind = 'branch'
        i.target = (pc + 4 + (i.simm << 2)) & 0xffffffff
    elif op in (2, 3):
        i.mn = _OPS[op]
        i.kind = i.mn
        i.target = ((pc + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)
    elif op in (4, 5, 6, 7):
        i.mn = _OPS[op]
        i.kind = 'branch'
        i.target = (pc + 4 + (i.simm << 2)) & 0xffffffff
    elif op in (16, 18):   # cop0 / cop2
        i.kind = 'cop'
        i.cop = op - 16
        if word & (1 << 25):
            if op == 18:
                i.mn = 'cop2'
            else:
                i.mn = {16: 'rfe'}.get(word & 63, 'cop0?')
        else:
            sub = i.rs
            names = {0: 'mfc', 2: 'cfc', 4: 'mtc', 6: 'ctc'}
            if sub in names:
                i.mn = names[sub] + str(op - 16)
            else:
                i.mn = 'cop%d?' % (op - 16)
                i.kind = 'invalid'
    elif op in _OPS:
        i.mn = _OPS[op]
        if i.mn in LOADS:
            i.kind = 'load'
        elif i.mn in STORES:
            i.kind = 'store'
        elif i.mn in ('lwc2', 'swc2'):
            i.kind = 'cop'
            i.cop = 2
    else:
        i.kind = 'invalid'
    return i


GTE_OPS = {0x01: 'RTPS', 0x06: 'NCLIP', 0x0c: 'OP', 0x10: 'DPCS', 0x11: 'INTPL', 0x12: 'MVMVA',
           0x13: 'NCDS', 0x14: 'CDP', 0x16: 'NCDT', 0x1b: 'NCCS', 0x1c: 'CC', 0x1e: 'NCS',
           0x20: 'NCT', 0x28: 'SQR', 0x29: 'DCPL', 0x2a: 'DPCT', 0x2d: 'AVSZ3', 0x2e: 'AVSZ4',
           0x30: 'RTPT', 0x3d: 'GPF', 0x3e: 'GPL', 0x3f: 'NCCT'}


def disasm(i):
    r = REG
    mn = i.mn
    k = i.kind
    if k == 'invalid':
        return '.word 0x%08x' % i.word
    if i.word == 0:
        return 'nop'
    if k in ('load', 'store'):
        return '%s %s, %d(%s)' % (mn, r[i.rt], i.simm, r[i.rs])
    if mn in ('lwc2', 'swc2'):
        return '%s $%d, %d(%s)' % (mn, i.rt, i.simm, r[i.rs])
    if k == 'branch':
        if mn in ('beq', 'bne'):
            return '%s %s, %s, 0x%08x' % (mn, r[i.rs], r[i.rt], i.target)
        return '%s %s, 0x%08x' % (mn, r[i.rs], i.target)
    if k in ('j', 'jal'):
        return '%s 0x%08x' % (mn, i.target)
    if k == 'jr':
        return 'jr %s' % r[i.rs]
    if k == 'jalr':
        return 'jalr %s, %s' % (r[i.rd], r[i.rs])
    if k == 'sys':
        return '%s 0x%x' % (mn, (i.word >> 6) & 0xfffff)
    if k == 'cop':
        if mn == 'cop2':
            return 'cop2 %s (0x%07x)' % (GTE_OPS.get(i.word & 63, '?'), i.word & 0x1ffffff)
        if mn == 'rfe':
            return 'rfe'
        return '%s %s, $%d' % (mn, r[i.rt], i.rd)
    if mn in ('sll', 'srl', 'sra'):
        return '%s %s, %s, %d' % (mn, r[i.rd], r[i.rt], i.sa)
    if mn in ('sllv', 'srlv', 'srav'):
        return '%s %s, %s, %s' % (mn, r[i.rd], r[i.rt], r[i.rs])
    if mn in ('mfhi', 'mflo'):
        return '%s %s' % (mn, r[i.rd])
    if mn in ('mthi', 'mtlo'):
        return '%s %s' % (mn, r[i.rs])
    if mn in ('mult', 'multu', 'div', 'divu'):
        return '%s %s, %s' % (mn, r[i.rs], r[i.rt])
    if mn == 'lui':
        return 'lui %s, 0x%x' % (r[i.rt], i.imm)
    if mn in ('andi', 'ori', 'xori'):
        return '%s %s, %s, 0x%x' % (mn, r[i.rt], r[i.rs], i.imm)
    if mn in ('addi', 'addiu', 'slti', 'sltiu'):
        return '%s %s, %s, %d' % (mn, r[i.rt], r[i.rs], i.simm)
    return '%s %s, %s, %s' % (mn, r[i.rd], r[i.rs], r[i.rt])


# --- register def/use (for liveness/analysis) ---
def reads(i):
    """GPRs read by the instruction."""
    mn, k = i.mn, i.kind
    if k == 'invalid' or i.word == 0:
        return ()
    if k in ('load',):
        return (i.rs, i.rt) if mn in ('lwl', 'lwr') else (i.rs,)
    if k == 'store':
        return (i.rs, i.rt)
    if mn in ('lwc2', 'swc2'):
        return (i.rs,)
    if k == 'branch':
        return (i.rs, i.rt) if mn in ('beq', 'bne') else (i.rs,)
    if k in ('jr', 'jalr'):
        return (i.rs,)
    if k in ('j', 'jal', 'sys'):
        return ()
    if k == 'cop':
        return (i.rt,) if mn in ('mtc0', 'ctc0', 'mtc2', 'ctc2') else ()
    if mn in ('sll', 'srl', 'sra'):
        return (i.rt,)
    if mn in ('mfhi', 'mflo'):
        return ()
    if mn in ('mthi', 'mtlo'):
        return (i.rs,)
    if mn == 'lui':
        return ()
    if i.op != 0:
        return (i.rs,)
    return (i.rs, i.rt)


def writes(i):
    """GPR written (or None)."""
    mn, k = i.mn, i.kind
    if k == 'invalid' or i.word == 0:
        return None
    if k == 'load':
        return i.rt
    if k in ('store', 'branch', 'j', 'jr', 'sys'):
        return 31 if (k == 'branch' and mn.endswith('al')) else None
    if k == 'jal':
        return 31
    if k == 'jalr':
        return i.rd
    if k == 'cop':
        return i.rt if mn in ('mfc0', 'cfc0', 'mfc2', 'cfc2') else None
    if mn in ('mult', 'multu', 'div', 'divu', 'mthi', 'mtlo'):
        return None
    if i.op == 0:
        return i.rd
    return i.rt


def words(data, base):
    n = len(data) // 4
    return struct.unpack('<%dI' % n, data[:n * 4])
