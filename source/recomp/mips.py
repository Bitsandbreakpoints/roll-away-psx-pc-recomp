"""MIPS R3000A (PlayStation CPU) instruction decoder, with the GTE (COP2).

Only what the recompiler needs: the fields of each instruction, its
mnemonic, which registers it reads and writes, and its control flow.
"""

SPECIAL = {
    0x00: 'sll', 0x02: 'srl', 0x03: 'sra', 0x04: 'sllv', 0x06: 'srlv', 0x07: 'srav',
    0x08: 'jr', 0x09: 'jalr', 0x0C: 'syscall', 0x0D: 'break',
    0x10: 'mfhi', 0x11: 'mthi', 0x12: 'mflo', 0x13: 'mtlo',
    0x18: 'mult', 0x19: 'multu', 0x1A: 'div', 0x1B: 'divu',
    0x20: 'add', 0x21: 'addu', 0x22: 'sub', 0x23: 'subu',
    0x24: 'and', 0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x2A: 'slt', 0x2B: 'sltu',
}

PRIMARY = {
    0x02: 'j', 0x03: 'jal', 0x04: 'beq', 0x05: 'bne', 0x06: 'blez', 0x07: 'bgtz',
    0x08: 'addi', 0x09: 'addiu', 0x0A: 'slti', 0x0B: 'sltiu',
    0x0C: 'andi', 0x0D: 'ori', 0x0E: 'xori', 0x0F: 'lui',
    0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x26: 'lwr',
    0x28: 'sb', 0x29: 'sh', 0x2A: 'swl', 0x2B: 'sw', 0x2E: 'swr',
    0x32: 'lwc2', 0x3A: 'swc2',
}

LOADS = {'lb', 'lh', 'lwl', 'lw', 'lbu', 'lhu', 'lwr'}
STORES = {'sb', 'sh', 'swl', 'sw', 'swr'}
BRANCHES = {'beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'bltzal', 'bgezal'}

REG_NAMES = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
             't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
             's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
             't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


class Insn:
    __slots__ = ('addr', 'word', 'op', 'rs', 'rt', 'rd', 'sa', 'imm', 'uimm', 'target', 'code')

    def __init__(self, addr, word):
        self.addr = addr
        self.word = word
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.uimm = word & 0xFFFF
        self.imm = self.uimm - 0x10000 if self.uimm & 0x8000 else self.uimm
        self.target = None
        self.code = 0
        primary = word >> 26
        if primary == 0:
            self.op = SPECIAL.get(word & 0x3F)
            if self.op in ('syscall', 'break'):
                self.code = (word >> 6) & 0xFFFFF
        elif primary == 1:
            # (the R3000 decodes only bit 0 (ge/lt) and whether bits 4..1 are 1000b: link)
            link = (self.rt & 0x1E) == 0x10
            self.op = ('bgez' if self.rt & 1 else 'bltz') + ('al' if link else '')
        elif primary == 0x10:
            if self.rs == 0:
                self.op = 'mfc0'
            elif self.rs == 4:
                self.op = 'mtc0'
            elif self.rs == 0x10 and (word & 0x3F) == 0x10:
                self.op = 'rfe'
            else:
                self.op = None
        elif primary == 0x12:
            if self.rs & 0x10:
                self.op = 'cop2'
                self.code = word & 0x1FFFFFF
            else:
                self.op = {0: 'mfc2', 2: 'cfc2', 4: 'mtc2', 6: 'ctc2'}.get(self.rs)
        else:
            self.op = PRIMARY.get(primary)
        if self.op in ('j', 'jal'):
            self.target = ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        elif self.op in BRANCHES:
            self.target = (addr + 4 + (self.imm << 2)) & 0xFFFFFFFF

    @property
    def valid(self):
        return self.op is not None

    # control flow -------------------------------------------------------
    @property
    def has_delay_slot(self):
        return self.op in BRANCHES or self.op in ('j', 'jal', 'jr', 'jalr')

    @property
    def is_branch(self):
        return self.op in BRANCHES

    @property
    def always_taken(self):
        """A branch that is always taken (b / bal)."""
        return (self.op == 'beq' and self.rs == self.rt) or \
            (self.op in ('bgez', 'bgezal', 'blez') and self.rs == 0)

    @property
    def ends_block(self):
        """No fall-through after the delay slot."""
        return self.op in ('j', 'jr') or (self.always_taken and self.op in ('beq', 'bgez', 'blez'))

    # registers ----------------------------------------------------------
    def reads(self):
        op = self.op
        if op in ('sll', 'srl', 'sra'):
            return {self.rt}
        if op in ('sllv', 'srlv', 'srav', 'add', 'addu', 'sub', 'subu', 'and', 'or', 'xor',
                  'nor', 'slt', 'sltu', 'mult', 'multu', 'div', 'divu', 'beq', 'bne'):
            return {self.rs, self.rt}
        if op in ('jr', 'jalr', 'mthi', 'mtlo', 'blez', 'bgtz', 'bltz', 'bgez', 'bltzal',
                  'bgezal', 'addi', 'addiu', 'slti', 'sltiu', 'andi', 'ori', 'xori',
                  'lb', 'lh', 'lw', 'lbu', 'lhu', 'lwc2', 'swc2'):
            return {self.rs}
        if op in ('lwl', 'lwr', 'sb', 'sh', 'sw', 'swl', 'swr'):
            return {self.rs, self.rt}
        if op in ('mtc0', 'mtc2', 'ctc2'):
            return {self.rt}
        return set()

    def writes(self):
        op = self.op
        if op in ('sll', 'srl', 'sra', 'sllv', 'srlv', 'srav', 'add', 'addu', 'sub', 'subu',
                  'and', 'or', 'xor', 'nor', 'slt', 'sltu', 'mfhi', 'mflo', 'jalr'):
            return {self.rd} - {0}
        if op in ('addi', 'addiu', 'slti', 'sltiu', 'andi', 'ori', 'xori', 'lui', 'mfc0',
                  'mfc2', 'cfc2') or op in LOADS:
            return {self.rt} - {0}
        if op in ('jal', 'bltzal', 'bgezal'):
            return {31}
        return set()

    def __repr__(self):
        return f'{self.addr:08X}: {self.word:08X} {self.op}'


def decode(addr, word):
    return Insn(addr, word)
