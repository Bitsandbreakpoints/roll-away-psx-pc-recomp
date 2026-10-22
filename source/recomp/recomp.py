#!/usr/bin/env python3
"""Static recompiler: PS1 executable (MIPS R3000A) -> C.

    python recomp/recomp.py <exe> <config> <out_dir>

Every function of the executable becomes a C function that works on the
emulated CPU registers (g_cpu, src/runtime/recomp.h) and on PS1 memory.
Functions listed as `hle` in the config are not translated: calls to them
go to the PC port's replacement (hle_<name>), e.g. Sony's GPU, CD and
controller libraries.

Functions are found by following the code from the entry point: every
`jal` target, functions whose address the code builds with lui/addiu or
that are stored in the data, and `func` lines of the config.  A function
is everything reachable from its start without calls; code shared by two
functions is simply translated into both.  Jumps through a register are
jump tables (a C switch) when the table can be found, otherwise calls of
whatever function the address belongs to at run time (rt_call()).

A `hook <address> <name>` line has hook_<name>() (the PC port's) run just
before the instruction at that address: a small change to one of the
game's own decisions, made in the registers.

Only instructions are translated: no data of the executable ends up in
the C files (jump tables are read from PS1 memory at run time, which the
PC port loads from the player's disc).
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mips          # noqa: E402
import psxexe        # noqa: E402

JR_RA = 0x03E00008
INSNS_PER_FILE = 12000
MAX_TABLE = 1024


# ---------------------------------------------------------------------------
# config

class Config:
    def __init__(self, path):
        self.name = 'main'
        self.prefix = 'F'
        self.sha1 = None
        self.hle = {}           # address -> name
        self.funcs = set()      # extra entry points
        self.not_funcs = set()  # addresses that are never entry points
        self.jtbl = {}          # jr address -> (table address, count)
        self.names = {}         # address -> name (comments only)
        self.hle_ranges = []    # (start, end, name): library code that must not run
        self.hooks = {}         # address -> name: hook_<name>() runs before that instruction
        for line in open(path, encoding='utf-8'):
            line = line.split('#', 1)[0].split()
            if not line:
                continue
            key, args = line[0], line[1:]
            if key == 'name':
                self.name = args[0]
            elif key == 'prefix':
                self.prefix = args[0]
            elif key == 'sha1':
                self.sha1 = args[0].lower()
            elif key == 'hle':
                self.hle[int(args[0], 16)] = args[1]
            elif key == 'func':
                self.funcs.add(int(args[0], 16))
                if len(args) > 1:
                    self.names[int(args[0], 16)] = args[1]
            elif key == 'notfunc':
                self.not_funcs.add(int(args[0], 16))
            elif key == 'jtbl':
                self.jtbl[int(args[0], 16)] = (int(args[1], 16), int(args[2], 0))
            elif key == 'hlerange':
                self.hle_ranges.append((int(args[0], 16), int(args[1], 16), args[2]))
            elif key == 'sym':
                self.names[int(args[0], 16)] = args[1]
            elif key == 'hook':
                self.hooks[int(args[0], 16)] = args[1]
            else:
                raise SystemExit(f'{path}: unknown key {key}')


# ---------------------------------------------------------------------------
# analysis

class Function:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}         # address -> Insn
        self.labels = {entry}
        self.tables = {}        # jr address -> [targets]
        self.calls = set()
        self.tails = set()
        self.pointers = set()
        self.problems = []


class Recompiler:
    def __init__(self, exe, config):
        self.exe = exe
        self.cfg = config
        self.cache = {}
        self.entries = set()
        self.funcs = {}
        self.stubs = set()
        self.warnings = []
        self.hooks_placed = set()

    def in_hle_range(self, addr):
        return any(start <= addr < end for start, end, _ in self.cfg.hle_ranges)

    def insn(self, addr):
        i = self.cache.get(addr)
        if i is None:
            w = self.exe.word(addr)
            if w is None:
                return None
            i = self.cache[addr] = mips.decode(addr, w)
        return i

    def plausible_entry(self, v):
        """A function seems to start at v (code before it ended with jr ra)."""
        if v in self.cfg.not_funcs or not self.exe.contains(v) or v & 3:
            return False
        i = self.insn(v)
        if i is None or not i.valid:
            return False
        q = v - 4
        for _ in range(8):                      # (zero padding between functions)
            if self.exe.word(q) != 0:
                break
            q -= 4
        return self.exe.word(q) == JR_RA or self.exe.word(q - 4) == JR_RA

    # jump tables ----------------------------------------------------------
    def find_table(self, func, jr):
        if jr.addr in self.cfg.jtbl:
            base, count = self.cfg.jtbl[jr.addr]
        else:
            found = self.match_table(jr)
            if not found:
                return None
            base, count = found
        targets = []
        for k in range(count if count else MAX_TABLE):
            t = self.exe.word(base + 4 * k)
            if t is None:
                break
            if not count:
                # (no bound found: stop at the first entry that is not code near the function)
                if not (func.entry <= t < func.entry + 0x10000) or t & 3:
                    break
                i = self.insn(t)
                if i is None or not i.valid:
                    break
            if not self.exe.contains(t):
                self.warnings.append(f'{jr.addr:08X}: table entry {t:08X} outside the image')
                continue
            targets.append(t)
        return targets or None

    def last_write(self, addr, reg, limit=24):
        a = addr - 4
        for _ in range(limit):
            i = self.insn(a)
            if i is None:
                return None
            if reg in i.writes():
                return i
            if i.op in ('jal', 'jalr'):
                return None
            a -= 4
        return None

    def constant(self, at, reg):
        """Value of reg before `at` if it comes from lui (+ addiu/ori)."""
        i = self.last_write(at, reg)
        if i is None:
            return None
        if i.op == 'lui':
            return i.uimm << 16
        if i.op in ('addiu', 'ori') and i.rs != 0:
            hi = self.constant(i.addr, i.rs)
            if hi is None:
                return None
            return (hi + i.imm) & 0xFFFFFFFF if i.op == 'addiu' else hi | i.uimm
        return None

    def match_table(self, jr):
        lw = self.last_write(jr.addr, jr.rs)
        if lw is None or lw.op != 'lw':
            return None
        base = None
        add = self.last_write(lw.addr, lw.rs)
        if add is not None and add.op == 'addu':
            for a in (add.rs, add.rt):
                c = self.constant(add.addr, a)
                if c is not None:
                    base = c
                    break
        if base is None:
            return None
        table = (base + lw.imm) & 0xFFFFFFFF
        # the bound: sltiu rX, rI, N (on the index before it was scaled)
        count = 0
        a = jr.addr - 4
        for _ in range(32):
            i = self.insn(a)
            if i is None:
                break
            if i.op == 'sltiu':
                count = i.uimm
                break
            a -= 4
        return table, count

    # exploring ------------------------------------------------------------
    def is_tail(self, func, at, target):
        if target in self.cfg.hle:
            return True
        if target not in self.entries or target == func.entry:
            return False
        return not (func.entry < target < at)      # (a loop back into the function: goto)

    def note_pointer(self, f, i, lui):
        """Function pointers built in registers (lui + addiu/ori)."""
        if i.op == 'lui':
            lui[i.rt] = i.uimm << 16
        elif i.op in ('addiu', 'ori') and i.rs in lui:
            v = (lui[i.rs] + i.imm) & 0xFFFFFFFF if i.op == 'addiu' else lui[i.rs] | i.uimm
            if self.plausible_entry(v):
                f.pointers.add(v)

    def explore(self, entry):
        f = Function(entry)
        flowed = set()          # executed in sequence (not only as a delay slot)
        work = [(entry, True)]
        while work:
            a, jumped = work.pop()
            lui = {}
            first = True
            while True:
                if a in flowed:
                    break
                if not (first and jumped):
                    if a != entry and (a in self.entries or a in self.cfg.hle):
                        break       # falls into another function: a tail call (see emit)
                first = False
                i = self.insn(a)
                if i is None:
                    f.problems.append(f'{a:08X}: outside the image')
                    break
                f.insns[a] = i
                flowed.add(a)
                if not i.valid:
                    break
                self.note_pointer(f, i, lui)
                if i.has_delay_slot:
                    d = self.insn(a + 4)
                    if d is None:
                        f.problems.append(f'{a:08X}: no delay slot')
                        break
                    f.insns[a + 4] = d
                    self.note_pointer(f, d, lui)
                    if d.has_delay_slot:
                        f.problems.append(f'{a:08X}: branch in a delay slot')
                    op = i.op
                    if op == 'jal':
                        f.calls.add(i.target)
                        a += 8
                        continue
                    if op == 'jalr':
                        a += 8
                        continue
                    if op in ('bltzal', 'bgezal'):
                        if i.target != a + 8:
                            f.calls.add(i.target)
                        a += 8
                        continue
                    if op == 'j':
                        if self.is_tail(f, a, i.target):
                            f.tails.add(i.target)
                        else:
                            f.labels.add(i.target)
                            work.append((i.target, True))
                        break
                    if op == 'jr':
                        if i.rs != 31:
                            targets = self.find_table(f, i)
                            if targets:
                                f.tables[a] = targets
                                for t in targets:
                                    f.labels.add(t)
                                    work.append((t, True))
                        break
                    # conditional branch
                    f.labels.add(i.target)
                    work.append((i.target, True))
                    if i.ends_block:
                        break
                    a += 8
                    continue
                a += 4
        return f

    def analyse(self):
        exe, cfg = self.exe, self.cfg
        self.entries = {exe.pc} | cfg.funcs | set(cfg.hle)
        # function pointers stored in the data
        for addr, w in exe.words():
            if exe.base <= w < exe.end and self.plausible_entry(w):
                self.entries.add(w)
        queue = sorted(self.entries)
        while queue:
            e = queue.pop()
            if e in self.funcs or e in cfg.hle:
                continue
            if self.in_hle_range(e):
                self.stubs.add(e)       # (internal code of a replaced library object)
                continue
            f = self.explore(e)
            self.funcs[e] = f
            for t in sorted(f.calls | f.tails | f.pointers):
                if t not in self.entries:
                    self.entries.add(t)
                    queue.append(t)
        # load delay slots that the translation does not reproduce
        for f in self.funcs.values():
            for a, i in f.insns.items():
                if i.op in mips.LOADS and i.rt:
                    n = f.insns.get(a + 4)
                    if n is not None and i.rt in n.reads() and i.op not in ('lwl', 'lwr') \
                            and n.op not in ('lwl', 'lwr'):
                        self.warnings.append(f'{a:08X}: load delay slot uses r{i.rt} (old value ignored)')
        self.warnings = sorted(set(self.warnings))

    def fname(self, addr):
        if addr in self.cfg.hle:
            return 'hle_' + self.cfg.hle[addr]
        return f'{self.cfg.prefix}_{addr:08X}'


# ---------------------------------------------------------------------------
# C output

def R(n):
    return '0u' if n == 0 else f'GPR[{n}]'


def W(n, expr):
    return '' if n == 0 else f'GPR[{n}] = {expr};'


def hexu(v):
    return f'0x{v & 0xFFFFFFFF:X}u'


def ea(i):
    if i.rs == 0:
        return hexu(i.imm)
    if i.imm == 0:
        return R(i.rs)
    return f'{R(i.rs)} + {hexu(i.imm)}'


def condition(i):
    rs, rt = R(i.rs), R(i.rt)
    return {
        'beq': f'{rs} == {rt}', 'bne': f'{rs} != {rt}',
        'blez': f'(int32_t) {rs} <= 0', 'bgtz': f'(int32_t) {rs} > 0',
        'bltz': f'(int32_t) {rs} < 0', 'bgez': f'(int32_t) {rs} >= 0',
        'bltzal': f'(int32_t) {rs} < 0', 'bgezal': f'(int32_t) {rs} >= 0',
    }[i.op]


GTE_SXY_REGS = (12, 13, 14, 15)     # SXY0..2, SXYP


def simple(i):
    """C for an instruction without control flow."""
    op, rs, rt, rd = i.op, i.rs, i.rt, i.rd
    # a register copied (move: or/addu with $zero) keeps its precise vertex
    if op in ('or', 'addu') and rd and (rs == 0) != (rt == 0):
        src = rt if rs == 0 else rs
        return W(rd, R(src)) + f' RT_PGXP_MOVE({rd}, {src});'
    if op == 'sll':
        return W(rd, f'{R(rt)} << {i.sa}') if i.sa else W(rd, R(rt))
    if op == 'srl':
        return W(rd, f'{R(rt)} >> {i.sa}')
    if op == 'sra':
        return W(rd, f'(uint32_t) ((int32_t) {R(rt)} >> {i.sa})')
    if op == 'sllv':
        return W(rd, f'{R(rt)} << ({R(rs)} & 31)')
    if op == 'srlv':
        return W(rd, f'{R(rt)} >> ({R(rs)} & 31)')
    if op == 'srav':
        return W(rd, f'(uint32_t) ((int32_t) {R(rt)} >> ({R(rs)} & 31))')
    if op in ('add', 'addu'):
        return W(rd, f'{R(rs)} + {R(rt)}')
    if op in ('sub', 'subu'):
        return W(rd, f'{R(rs)} - {R(rt)}')
    if op == 'and':
        return W(rd, f'{R(rs)} & {R(rt)}')
    if op == 'or':
        return W(rd, f'{R(rs)} | {R(rt)}')
    if op == 'xor':
        return W(rd, f'{R(rs)} ^ {R(rt)}')
    if op == 'nor':
        return W(rd, f'~({R(rs)} | {R(rt)})')
    if op == 'slt':
        return W(rd, f'(int32_t) {R(rs)} < (int32_t) {R(rt)}')
    if op == 'sltu':
        return W(rd, f'{R(rs)} < {R(rt)}')
    if op in ('addi', 'addiu'):
        return W(rt, f'{R(rs)} + {hexu(i.imm)}' if rs else hexu(i.imm))
    if op == 'slti':
        return W(rt, f'(int32_t) {R(rs)} < {i.imm}')
    if op == 'sltiu':
        return W(rt, f'{R(rs)} < {hexu(i.imm)}')
    if op == 'andi':
        return W(rt, f'{R(rs)} & {hexu(i.uimm)}')
    if op == 'ori':
        return W(rt, f'{R(rs)} | {hexu(i.uimm)}' if rs else hexu(i.uimm))
    if op == 'xori':
        return W(rt, f'{R(rs)} ^ {hexu(i.uimm)}')
    if op == 'lui':
        return W(rt, hexu(i.uimm << 16))
    if op in ('mult', 'multu', 'div', 'divu'):
        return f'rt_{op}({R(rs)}, {R(rt)});'
    if op == 'mfhi':
        return W(rd, 'g_cpu.hi')
    if op == 'mflo':
        return W(rd, 'g_cpu.lo')
    if op == 'mthi':
        return f'g_cpu.hi = {R(rs)};'
    if op == 'mtlo':
        return f'g_cpu.lo = {R(rs)};'
    if op == 'lb':
        return W(rt, f'(uint32_t) (int8_t) RD8({ea(i)})')
    if op == 'lbu':
        return W(rt, f'RD8({ea(i)})')
    if op == 'lh':
        return W(rt, f'(uint32_t) (int16_t) RD16({ea(i)})')
    if op == 'lhu':
        return W(rt, f'RD16({ea(i)})')
    if op == 'lw':
        if not rt:
            return W(rt, f'RD32({ea(i)})')
        # (precise vertices: a screen position loaded keeps its float position)
        return f'{{ uint32_t a_ = {ea(i)}; GPR[{rt}] = RD32(a_); RT_PGXP_LW({rt}, a_); }}'
    if op == 'lwl':
        return W(rt, f'rt_lwl({R(rt)}, {ea(i)})')
    if op == 'lwr':
        return W(rt, f'rt_lwr({R(rt)}, {ea(i)})')
    if op == 'sb':
        return f'WR8({ea(i)}, {R(rt)});'
    if op == 'sh':
        return f'WR16({ea(i)}, {R(rt)});'
    if op == 'sw':
        # (precise vertices: a screen position the CPU stores goes with it)
        track = f' RT_PGXP_SW({rt}, {ea(i)});' if rt else ''
        return f'WR32({ea(i)}, {R(rt)});' + track
    if op == 'swl':
        return f'rt_swl({ea(i)}, {R(rt)});'
    if op == 'swr':
        return f'rt_swr({ea(i)}, {R(rt)});'
    if op == 'lwc2':
        return f'rt_lwc2({rt}, {ea(i)});'
    if op == 'swc2':
        return f'rt_swc2({rt}, {ea(i)});'
    if op == 'mfc2':
        if rt and rd in GTE_SXY_REGS:
            return W(rt, f'gte_mfc2({rd})') + f' rt_pgxp_mfc2({rt}, {rd});'
        return W(rt, f'gte_mfc2({rd})')
    if op == 'cfc2':
        return W(rt, f'gte_cfc2({rd})')
    if op == 'mtc2':
        return f'gte_mtc2({rd}, {R(rt)});'
    if op == 'ctc2':
        return f'gte_ctc2({rd}, {R(rt)});'
    if op == 'cop2':
        return f'gte_cop2({hexu(i.code)});'
    if op == 'mfc0':
        return W(rt, f'g_cpu.cop0[{rd}]')
    if op == 'mtc0':
        return f'rt_mtc0({rd}, {R(rt)});'
    if op == 'rfe':
        return 'rt_rfe();'
    if op == 'syscall':
        return f'rt_syscall({hexu(i.addr)});'
    if op == 'break':
        return f'rt_break({hexu(i.addr)}, {hexu(i.code)});'
    return f'rt_bad_insn({hexu(i.addr)}, {hexu(i.word)});'


class Emitter:
    def __init__(self, rc):
        self.rc = rc

    def call(self, target):
        return f'{self.rc.fname(target)}();'

    def function(self, f):
        rc = self.rc
        used = set()

        def goto(t, backward):
            used.add(t)
            return ('RT_POLL(); ' if backward else '') + f'goto L_{t:08X};'

        def leave(t):
            if t in f.insns:
                return '    ' + goto(t, False)
            return f'    {self.call(t)} return;'        # (falls into another function)

        body = []
        prev_falls = None
        delay_slots = {a + 4 for a, i in f.insns.items() if i.has_delay_slot}
        # (a delay slot that is also a jump target is emitted on its own, so the
        # branch before it falls through to the instruction after it: a label)
        for a in delay_slots & f.labels:
            if a + 4 in f.insns:
                f.labels.add(a + 4)
        for a in sorted(f.insns):
            i = f.insns[a]
            if a in delay_slots and a not in f.labels:
                continue
            if prev_falls is not None and prev_falls != a:
                body.append(leave(prev_falls))
            if a in f.labels:
                body.append((a, f'L_{a:08X}: ;'))
            if a in rc.cfg.hooks:
                body.append(f'    hook_{rc.cfg.hooks[a]}();')
                rc.hooks_placed.add(a)
            body.append(f'    /* {a:08X} */ ' + self.insn(f, i, goto))
            if not i.valid:
                prev_falls = None
            elif i.has_delay_slot:
                prev_falls = None if i.ends_block else a + 8
            else:
                prev_falls = a + 4
        if prev_falls is not None:
            body.append(leave(prev_falls))
        out = []
        name = rc.cfg.names.get(f.entry)
        if name:
            out.append(f'/* {name} */')
        out += [f'void {rc.fname(f.entry)}(void)', '{']
        # (labels nobody jumps to are left out: they would only hinder optimisation)
        for line in body:
            if isinstance(line, tuple):
                if line[0] in used:
                    out.append(line[1])
            else:
                out.append(line)
        out.append('}')
        return '\n'.join(out) + '\n'

    def insn(self, f, i, goto):
        if not i.valid:
            return f'rt_bad_insn({hexu(i.addr)}, {hexu(i.word)}); return;'
        if not i.has_delay_slot:
            return simple(i)
        a = i.addr
        d = f.insns.get(a + 4)
        ds = simple(d) if d is not None and d.valid and not d.has_delay_slot else ''
        op = i.op
        if op == 'jal':
            return f'GPR[31] = {hexu(a + 8)}; {ds} {self.call(i.target)}'
        if op == 'jalr':
            link = W(i.rd, hexu(a + 8))
            return f'{{ uint32_t t_ = {R(i.rs)}; {link} {ds} rt_call(t_); }}'
        if op == 'j':
            if i.target in f.labels and i.target in f.insns:
                return f'{ds} {goto(i.target, i.target <= a)}'
            return f'{ds} {self.call(i.target)} return;'
        if op == 'jr':
            if i.rs == 31:
                return f'{ds} return;'
            targets = f.tables.get(a)
            if not targets:
                return f'{{ uint32_t t_ = {R(i.rs)}; {ds} rt_call(t_); return; }}'
            cases = ' '.join(f'case {hexu(t)}: {goto(t, t <= a)}' for t in sorted(set(targets)))
            return (f'{{ uint32_t t_ = {R(i.rs)}; {ds} switch (t_) {{ {cases} '
                    f'default: rt_call(t_); return; }} }}')
        if op in ('bltzal', 'bgezal'):
            link = f'GPR[31] = {hexu(a + 8)};'
            if i.target == a + 8:
                return f'{link} {ds}'
            # (hand-written code: the routine called may restore the registers
            # saved by its caller's caller and return there: then so do we)
            back = f'if (GPR[31] != {hexu(a + 8)}) return;'
            return f'{{ int c_ = {condition(i)}; {link} {ds} if (c_) {{ {self.call(i.target)} {back} }} }}'
        # conditional branches
        if i.always_taken:
            return f'{ds} {goto(i.target, i.target <= a)}'
        return f'{{ int c_ = {condition(i)}; {ds} if (c_) {{ {goto(i.target, i.target <= a)} }} }}'


def write_if_changed(path, text):
    old = open(path, encoding='utf-8').read() if os.path.exists(path) else None
    if old != text:
        open(path, 'w', encoding='utf-8', newline='\n').write(text)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    exe_path, cfg_path, out_dir = sys.argv[1:]
    data = open(exe_path, 'rb').read()
    cfg = Config(cfg_path)
    digest = hashlib.sha1(data).hexdigest()
    if cfg.sha1 and digest != cfg.sha1:
        raise SystemExit(f'{exe_path}: SHA1 {digest} is not the expected {cfg.sha1}')
    exe = psxexe.PsxExe(data)
    rc = Recompiler(exe, cfg)
    rc.analyse()
    em = Emitter(rc)
    os.makedirs(out_dir, exist_ok=True)
    mod = cfg.name
    funcs = [rc.funcs[e] for e in sorted(rc.funcs)]
    gen = f'/* GENERATED by recomp/recomp.py from {os.path.basename(exe_path)} - do not edit */'

    header = [gen, f'#ifndef RECOMP_{mod.upper()}_H', f'#define RECOMP_{mod.upper()}_H', '']
    header += [f'void {rc.fname(f.entry)}(void);' for f in funcs]
    header += [f'void {rc.fname(a)}(void);' for a in sorted(cfg.hle)]
    header += [f'void {rc.fname(a)}(void);' for a in sorted(rc.stubs)]
    header += [f'void hook_{name}(void);' for name in sorted(set(cfg.hooks.values()))]
    header += ['', '#endif', '']
    write_if_changed(os.path.join(out_dir, f'{mod}_funcs.h'), '\n'.join(header))
    files = []
    chunks, size = [[]], 0
    for f in funcs:                     # (files of about the same size: parallel builds)
        if size >= INSNS_PER_FILE:
            chunks.append([])
            size = 0
        chunks[-1].append(f)
        size += len(f.insns)
    stubs = [f'void {rc.fname(a)}(void) {{ rt_hle_internal({hexu(a)}); }}' for a in sorted(rc.stubs)]
    for n, chunk in enumerate(chunks):
        text = [gen, '#include "runtime/recomp.h"', f'#include "{mod}_funcs.h"', '']
        text += [em.function(f) for f in chunk]
        if n == 0:
            text += stubs + ['']
        name = f'{mod}_{n:03d}.c'
        write_if_changed(os.path.join(out_dir, name), '\n'.join(text))
        files.append(name)
    entries = sorted(set(rc.funcs) | set(cfg.hle) | rc.stubs)
    table = [gen, '#include "runtime/recomp.h"', f'#include "{mod}_funcs.h"', '',
             f'const RtFunction g_{mod}_functions[] = {{']
    table += [f'    {{ {hexu(a)}, {rc.fname(a)} }},' for a in entries]
    table += ['};', f'const int g_{mod}_function_count = {len(entries)};',
              f'/* the SHA1 of the file the code was made from (checked when it is loaded) */',
              f'const char g_{mod}_sha1[] = "{digest}";', '']
    write_if_changed(os.path.join(out_dir, f'{mod}_table.c'), '\n'.join(table))
    files.append(f'{mod}_table.c')
    missing = sorted(set(cfg.hooks) - rc.hooks_placed)
    if missing:
        raise SystemExit('hook not placed (not an instruction of its own): '
                         + ', '.join(f'{a:08X}' for a in missing))
    write_if_changed(os.path.join(out_dir, f'{mod}_files.txt'), '\n'.join(files) + '\n')
    report = [f'{os.path.basename(exe_path)}: {len(rc.funcs)} functions, {len(cfg.hle)} replaced (hle), '
              f'{sum(len(f.tables) for f in funcs)} jump tables, '
              f'{sum(len(f.insns) for f in funcs)} instructions translated']
    for f in funcs:
        for p in f.problems:
            report.append(f'problem in {f.entry:08X}: {p}')
        for a, i in sorted(f.insns.items()):
            if i.op == 'jr' and i.rs != 31 and a not in f.tables:
                report.append(f'{f.entry:08X}: jr {mips.REG_NAMES[i.rs]} at {a:08X} without a table (rt_call)')
    for f in funcs:
        for t in sorted(f.calls | f.tails | f.pointers):
            for start, end, name in cfg.hle_ranges:
                if start <= t < end and t not in cfg.hle:
                    report.append(f'{f.entry:08X}: calls {t:08X} in {name}, which is replaced (hle)')
    for e in sorted(rc.stubs):
        report.append(f'{e:08X}: library-internal (hle object), not translated')
    report += rc.warnings
    write_if_changed(os.path.join(out_dir, f'{mod}_report.txt'), '\n'.join(report) + '\n')
    print(report[0])


if __name__ == '__main__':
    main()
