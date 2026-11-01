#!/usr/bin/env python3
"""ovrp_abi.py - deduce the AArch64 ABI of every ovrp_* export of a real libOVRPlugin.so
and compare it with the emulation in runtime/xr/kl_ovrp*.c.

    ovrp_abi.py LIB.so [LIB2.so ...] SRC.c [SRC2.c ...] [options]

  * .so files are the real libraries (data: never executed, only disassembled).
    The NEWEST SDK version given is the reference signature; older ones are only
    reported where their ABI differs from it.
  * .c files are the emulation. The sibling batch files kl_ovrp_{render,input,
    spatial,misc}.c next to kl_ovrp.c are added automatically.
  --report FILE   write the full markdown report
  --json FILE     write the machine-readable result
  --game VER=NAME label which game ships which SDK version (repeatable)
  --strict        exit 1 when a grave discrepancy against the reference is found
                  (default: print the summary and exit 0)
  --quiet         one line only

Needs llvm-objdump + llvm-readelf (any LLVM >= 14); no Python packages.

HONEST LIMITS. This is a linear dataflow over the wrapper functions (with calls into
internal helpers followed up to depth 3), not a decompiler. Every fact carries a
confidence:
  high   seen directly in the export (or a followed helper), no unknown call touches it
  medium reached through a followed helper, or return type inferred from constants
  low    the value is forwarded to a virtual / external call: the real extent or type
         is whatever that callee does and is NOT known here
Trailing arguments an export never touches are invisible (argc is a lower bound).
"""
import glob, hashlib, os, re, subprocess, sys, json
from collections import defaultdict

# ----------------------------------------------------------------------------
# disassembly
# ----------------------------------------------------------------------------
def tool(*names):
    for n in names:
        for d in os.environ.get("PATH", "").split(os.pathsep):
            p = os.path.join(d, n)
            if os.path.isfile(p) and os.access(p, os.X_OK):
                return p
    sys.exit("ovrp_abi: need one of %s in PATH" % ", ".join(names))

INS_RE = re.compile(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*?)\s*(?://.*)?$')

class Lib:
    def __init__(self, path):
        self.path = path
        data = open(path, "rb").read()
        self.sha = hashlib.sha256(data).hexdigest()
        self.version = "?"
        for m in re.finditer(rb'(?<=\x00)(1\.(\d{2,3})\.\d+)(?=\x00)', data):
            self.version = m.group(1).decode()
            break
        self.vkey = tuple(int(x) for x in self.version.split(".")) if self.version != "?" else (0,)
        objdump = tool("llvm-objdump", "llvm-objdump-18", "llvm-objdump-17", "llvm-objdump-16",
                       "llvm-objdump-15", "llvm-objdump-14")
        readelf = tool("llvm-readelf", "llvm-readelf-18", "llvm-readelf-17", "llvm-readelf-16",
                       "llvm-readelf-15", "llvm-readelf-14")
        syms = subprocess.run([readelf, "-W", "--dyn-syms", path], capture_output=True, text=True).stdout
        self.exports = {}
        for ln in syms.splitlines():
            f = ln.split()
            if len(f) >= 8 and f[3] == "FUNC" and f[6] != "UND" and f[7].startswith("ovrp_"):
                self.exports[f[7]] = (int(f[1], 16), int(f[2]))
        self.starts = {a for a, _ in self.exports.values()}
        txt = subprocess.run([objdump, "-d", "--no-show-raw-insn", path],
                             capture_output=True, text=True).stdout
        self.ins = {}
        self.plt = {}
        for ln in txt.splitlines():
            m = INS_RE.match(ln)
            if not m:
                continue
            a = int(m.group(1), 16)
            mn, ops = m.group(2), m.group(3)
            self.ins[a] = (mn, ops)
            if mn in ("bl", "b"):
                pm = re.match(r'0x([0-9a-f]+) <([^>+]*)@plt>', ops)
                if pm:
                    self.plt[int(pm.group(1), 16)] = pm.group(2)
        self.cache = {}
        self.busy = set()

# ----------------------------------------------------------------------------
# operand helpers
# ----------------------------------------------------------------------------
def split_ops(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "[{": depth += 1
        elif ch in "]}": depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip()); cur = ""
        else:
            cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

def parse_imm(t):
    t = t.strip()
    if t.startswith("#"): t = t[1:]
    try: return int(t, 0)
    except ValueError: return None

def preg(t):
    """('g', n, 'w'|'x') | ('z',) | ('sp',) | ('f', n, letter) | None"""
    t = t.strip()
    if t in ("xzr", "wzr"): return ("z",)
    if t in ("sp", "wsp"): return ("sp",)
    m = re.fullmatch(r'([wx])(\d+)', t)
    if m:
        n = int(m.group(2))
        return ("sp",) if n == 31 else ("g", n, m.group(1))
    m = re.fullmatch(r'([bhsdq])(\d+)', t)
    if m: return ("f", int(m.group(2)), m.group(1))
    m = re.fullmatch(r'v(\d+)(\.\S*)?', t)
    if m: return ("f", int(m.group(1)), "v")
    return None

def mem_parse(t):
    """'[x0, #8]!' -> (base_reg_tuple, imm|None|'?', pre_writeback)"""
    pre = t.endswith("!")
    inner = t.rstrip("!")[1:-1]
    parts = [p.strip() for p in inner.split(",")]
    base = preg(parts[0])
    imm = 0
    if len(parts) > 1:
        if parts[1].startswith("#"):
            imm = parse_imm(parts[1])
            if imm is None: imm = "?"
        else:
            imm = "?"
    return base, imm, pre

FSIZE = {"b": 1, "h": 2, "s": 4, "d": 8, "q": 16}

def vec_bytes(t):
    m = re.match(r'v\d+\.(\d+)?([bhsdq])', t)
    if not m: return 16
    return (int(m.group(1)) if m.group(1) else 1) * {"b": 1, "h": 2, "s": 4, "d": 8, "q": 16}[m.group(2)]

# ----------------------------------------------------------------------------
# per-function analysis
# ----------------------------------------------------------------------------
OTHER = ("other",)
NORETURN = {"__stack_chk_fail", "abort", "__cxa_throw", "__cxa_rethrow", "__assert2", "__cxa_bad_cast",
            "_ZSt9terminatev", "__cxa_call_unexpected", "_Unwind_Resume", "__android_log_assert"}

class State:
    __slots__ = ("regs", "fregs", "stack", "sp")
    def __init__(self):
        self.regs = {k: ("init", k) for k in range(8)}
        self.regs[8] = ("init8",)
        self.fregs = {k: ("finit", k) for k in range(8)}
        self.stack = {}
        self.sp = 0
    def copy(self):
        s = State.__new__(State)
        s.regs = dict(self.regs); s.fregs = dict(self.fregs); s.stack = dict(self.stack); s.sp = self.sp
        return s

def meet(states):
    if len(states) == 1: return states[0].copy()
    r = states[0].copy()
    for s in states[1:]:
        for d, o in ((r.regs, s.regs), (r.fregs, s.fregs), (r.stack, s.stack)):
            for k in list(d):
                if o.get(k, OTHER) != d[k]:
                    if d is r.stack: del d[k]
                    elif d is r.regs and k == 0:
                        mem = set()
                        for v in (d[k], o.get(k, OTHER)):
                            mem |= set(v[1]) if v[0] == "set" else {v}
                        d[k] = ("set", frozenset(mem)) if len(mem) <= 12 else OTHER
                    else: d[k] = OTHER
        if s.sp != r.sp: r.sp = s.sp
    return r

class Info:
    def __init__(self):
        self.used = defaultdict(set)       # k -> {'w','x'}
        self.fused = defaultdict(set)      # k -> {'s','d','q'}
        self.ptr = set()                   # k used as a memory base
        self.outs = defaultdict(set)       # k -> {(off,size)}
        self.ins = defaultdict(set)
        self.fwd_unknown = defaultdict(set)  # k -> {why}
        self.sret_used = False
        self.sret_stores = set()
        self.sret_fwd = False
        self.ret_vals = set()
        self.ret_fregs = set()             # ('s'/'d'/'q', idx)
        self.ret_x1 = False
        self.calls = set()
        self.nins = 0
        self.truncated = False
        self.opaque = False
        self.ret_float = []
        self.rets = []
        self.ret_n = 0

def fn_end(lib, start, hard_end=None):
    if hard_end is not None: return hard_end
    a, maxfwd = start, start
    for _ in range(6000):
        ins = lib.ins.get(a)
        if not ins:
            return a
        if a != start and a in lib.starts:
            return a
        mn, ops = ins
        tm = re.search(r'0x([0-9a-f]+)', ops)
        if tm and (mn.startswith("b.") or mn in ("cbz", "cbnz", "tbz", "tbnz", "b")):
            t = int(tm.group(1), 16)
            if t > a: maxfwd = max(maxfwd, t)
        if mn in ("ret", "br", "b") and a >= maxfwd:
            return a + 4
        a += 4
    return a

def analyze(lib, start, end=None, depth=0):
    key = start
    if key in lib.cache: return lib.cache[key]
    if key in lib.busy or depth > 3:
        return Info()
    lib.busy.add(key)
    info = Info()
    end = fn_end(lib, start, end)
    pending = defaultdict(list)
    cur = State()
    alive = True
    a = start

    def rd(tok):
        r = preg(tok)
        if not r: return OTHER
        if r[0] == "z": return ("const", 0, 64)
        if r[0] == "sp": return ("sp",)
        if r[0] == "g":
            v = cur.regs.get(r[1], OTHER)
            if v[0] == "init": info.used[v[1]].add(r[2])
            elif v[0] == "init8": info.sret_used = True
            return v
        if r[0] == "f":
            v = cur.fregs.get(r[1], OTHER)
            if v[0] == "finit": info.fused[v[1]].add("q" if r[2] == "v" else r[2])
            return v
        return OTHER

    def peek(tok):
        r = preg(tok)
        if r and r[0] == "g": return cur.regs.get(r[1], OTHER)
        if r == ("z",): return ("const", 0, 64)
        return OTHER

    def wr(tok, val):
        r = preg(tok)
        if not r: return
        if r[0] == "g":
            if val[0] == "const" and r[2] == "w":
                v = val[1] & 0xffffffff
                if v >= 1 << 31: v -= 1 << 32
                val = ("const", v, 32)
            cur.regs[r[1]] = val
        elif r[0] == "f":
            cur.fregs[r[1]] = ("fw", "q" if r[2] == "v" else r[2])

    def base_hit(base, imm, size, store, label=None):
        """record a memory access through a register base"""
        if not base or base[0] != "g":
            return
        v = cur.regs.get(base[1], OTHER)
        if v[0] in ("init", "arg"):
            k = v[1]; off = v[2] if v[0] == "arg" else 0
            if v[0] == "init": info.used[k].add("x")
            info.ptr.add(k)
            o = "?" if (off == "?" or imm == "?") else off + imm
            (info.outs if store else info.ins)[k].add((o, size))
        elif v[0] in ("init8", "sret"):
            off = v[1] if v[0] == "sret" else 0
            info.sret_used = True
            o = "?" if imm == "?" else off + imm
            if store: info.sret_stores.add((o, size))

    def do_call(target, virtual=False):
        nonlocal alive
        name = lib.plt.get(target) if target else None
        if name in NORETURN:
            alive = False
            return ("noret",)
        callee = None
        if target and not name and target in lib.ins:
            callee = analyze(lib, target, None, depth + 1)
        info.calls.add(name or (hex(target) if target else "virtual"))
        if name in ("memset", "__memset_chk", "bzero", "memcpy", "__memcpy_chk", "memmove", "strncpy"):
            szr = cur.regs.get(1 if name == "bzero" else 2, OTHER)
            size = szr[1] if szr[0] == "const" else "?"
            v = cur.regs.get(0, OTHER)
            if v[0] in ("init", "arg"):
                if v[0] == "init": info.used[v[1]].add("x")
                info.ptr.add(v[1])
                off = v[2] if v[0] == "arg" else 0
                info.outs[v[1]].add((off, size))
            if name.startswith(("memcpy", "__memcpy", "memmove")):
                s = cur.regs.get(1, OTHER)
                if s[0] in ("init", "arg"):
                    if s[0] == "init": info.used[s[1]].add("x")
                    info.ptr.add(s[1]); info.ins[s[1]].add((s[2] if s[0] == "arg" else 0, size))
        elif callee is not None:
            info.opaque = info.opaque or callee.opaque
            for j in range(8):
                v = cur.regs.get(j, OTHER)
                if callee.used.get(j) and v[0] == "init":
                    info.used[v[1]] |= callee.used[j]
                if callee.fused.get(j):
                    fv = cur.fregs.get(j, OTHER)
                    if fv[0] == "finit": info.fused[fv[1]] |= callee.fused[j]
                if v[0] in ("init", "arg"):
                    k = v[1]; off = v[2] if v[0] == "arg" else 0
                    if j in callee.ptr: info.ptr.add(k)
                    for (o, s) in callee.outs.get(j, ()):
                        info.outs[k].add(("?" if "?" in (o, off) else off + o, s))
                    for (o, s) in callee.ins.get(j, ()):
                        info.ins[k].add(("?" if "?" in (o, off) else off + o, s))
                    for why in callee.fwd_unknown.get(j, ()):
                        info.fwd_unknown[k].add(why)
            x8 = cur.regs.get(8, OTHER)
            if callee.sret_used and x8[0] in ("init8", "sret"):
                info.sret_used = True
                off = x8[1] if x8[0] == "sret" else 0
                for (o, s) in callee.sret_stores:
                    info.sret_stores.add(("?" if "?" in (o, off) else off + o, s))
                if callee.sret_fwd: info.sret_fwd = True
        else:
            info.opaque = True
            for j in range(8):
                v = cur.regs.get(j, OTHER)
                if v[0] == "arg":
                    info.fwd_unknown[v[1]].add("virtual" if virtual else (name or "extern"))
                elif v[0] == "init" and j == v[1] and name and name not in ("memset",):
                    pass
            x8 = cur.regs.get(8, OTHER)
            if x8[0] == "sret":
                info.sret_used = True; info.sret_fwd = True
        for j in range(0, 18): cur.regs[j] = OTHER
        for j in range(0, 8): cur.fregs[j] = OTHER
        cur.regs[0] = ("call", target if not name else name, virtual)
        cur.fregs[0] = ("fcall", target)
        return ("ok",)

    def do_ret():
        v = cur.regs.get(0, OTHER)
        if v[0] == "init": v = ("passthru",)
        for m in (v[1] if v[0] == "set" else (v,)):
            info.ret_vals.add(("passthru",) if m[0] == "init" else m)
        for j in range(4):
            f = cur.fregs.get(j, OTHER)
            if f[0] == "fw": info.ret_fregs.add((f[1], j))
        x1 = cur.regs.get(1, OTHER)
        if x1[0] not in ("init", "other") : info.ret_x1 = True
        info.ret_n += 1
        f0 = cur.fregs.get(0, OTHER)
        x0s = [m for m in (v[1] if v[0] == "set" else (v,))]
        if f0[0] == "fw" and f0[1] in ("s", "d", "z") and all(m[0] not in ("const", "and1", "cset") for m in x0s):
            info.ret_float.append(f0[1]); info.rets.append(("f", f0[1]))
        elif f0[0] == "fcall" and isinstance(f0[1], int) and x0s and all(m[0] == "call" for m in x0s):
            info.rets.append(("c", f0[1]))
        else: info.rets.append(("n",))

    while a < end:
        ins = lib.ins.get(a)
        if not ins: break
        mn, ops = ins
        if a in pending:
            sts = pending.pop(a)
            if alive: sts = sts + [cur]
            cur = meet(sts); alive = True
        elif not alive:
            cur = State()
            for k in list(cur.regs): cur.regs[k] = OTHER
            for k in list(cur.fregs): cur.fregs[k] = OTHER
            alive = True
        info.nins += 1
        o = split_ops(ops)
        tm = re.search(r'0x([0-9a-f]+)', ops)
        tgt = int(tm.group(1), 16) if tm else None

        if mn == "ret":
            do_ret(); alive = False
        elif mn == "bl":
            do_call(tgt)
        elif mn == "blr":
            rd(o[0]); do_call(None, True)
        elif mn == "br":
            rd(o[0])
            if do_call(None, True) is not None and alive:
                do_ret()
            alive = False
        elif mn == "b":
            if start <= tgt < end and tgt not in lib.starts:
                pending[tgt].append(cur.copy())
            else:
                if do_call(tgt) != ("noret",): do_ret()
            alive = False
        elif mn.startswith("b."):
            pending[tgt].append(cur.copy())
        elif mn in ("cbz", "cbnz", "tbz", "tbnz"):
            rd(o[0]); pending[tgt].append(cur.copy())
        elif mn in ("cmp", "cmn", "tst", "ccmp", "ccmn", "fcmp", "fcmpe", "fccmp", "prfm", "nop", "dmb",
                    "dsb", "isb", "brk", "udf", "hint", "svc", "msr", "paciasp", "autiasp", "bti"):
            for t in o:
                if preg(t): rd(t)
        elif re.match(r'(st|ld)(r|ur|tr|p|np|lr|lur|ar|apr|xr|axr|lxr|xp|axp|1|2|3|4|1r)[bhws]*$|'
                      r'(ldadd|ldset|ldclr|ldeor|swp|cas|ldsmax|ldumax|ldsmin|ldumin)(a|l|al)?[bh]?$',
                      mn) or mn.startswith(("ldr", "str", "ldp", "stp", "ldur", "stur", "st1", "ld1", "ld2",
                                           "st2", "ldxr", "stxr", "stlxr", "ldaxr", "stnp", "ldnp")):
            mem_i = next((i for i, t in enumerate(o) if t.startswith("[")), None)
            if mem_i is None:                       # literal load: ldr x0, 0x1234 <sym>
                if o and preg(o[0]): wr(o[0], ("ptr",))
            else:
                regs = [t for t in o[:mem_i]]
                base, imm, pre = mem_parse(o[mem_i])
                post = parse_imm(o[mem_i + 1]) if len(o) > mem_i + 1 else None
                is_atomic = bool(re.match(r'(ldadd|ldset|ldclr|ldeor|swp|cas|ldsmax|ldumax|ldsmin|ldumin)', mn))
                is_store = mn.startswith(("st", "stur")) and not is_atomic
                is_vec = mn.startswith(("st1", "ld1", "ld2", "st2", "ld3", "st3", "ld4", "st4"))
                lane = "[" in regs[-1] if (is_vec and regs) else False
                # access size
                sz = 0
                if is_vec:
                    if lane or mn == "ld1r":
                        m = re.search(r'\.(\d*)([bhsdq])', regs[0]); sz = FSIZE[m.group(2)] if m else 8
                    else:
                        sz = sum(vec_bytes(r) for r in regs if r.lstrip("{ ").startswith("v"))
                        if not sz: sz = 16
                else:
                    for r in regs:
                        pr = preg(r)
                        if pr and pr[0] in ("g", "z", "f") and not (mn.startswith(("stxr", "stlxr", "stxp", "stlxp")) and r is regs[0]):
                            w = 8 if (pr[0] == "z" and r.startswith("x")) or (pr[0] == "g" and pr[2] == "x") else 4
                            if pr[0] == "z" and r.startswith("w"): w = 4
                            if pr[0] == "f": w = FSIZE[pr[2]]
                            sz += w
                            if mn in ("ldrsw",): sz = 4
                    if re.search(r'(ldr|str|ldur|stur|stlr|ldar|ldapr|ldxr|stxr|ldaxr|stlxr|ldlar|stllr)[sa]?b$|^(ldrsb|ldursb|swp.*b|cas.*b|ldadd.*b)', mn) or mn.endswith("b") and not mn.startswith("b") and mn[:-1] in ("ldr", "str", "ldur", "stur", "stlr", "ldar", "ldxr", "stxr", "ldaxr", "stlxr", "ldrs", "ldurs", "ldapr", "stlur", "ldapur"):
                        sz = 1
                    elif mn.endswith("h") and mn[:-1] in ("ldr", "str", "ldur", "stur", "stlr", "ldar", "ldxr", "stxr", "ldaxr", "stlxr", "ldrs", "ldurs", "ldapr", "stlur", "ldapur"):
                        sz = 2
                    elif mn in ("ldrsw", "ldursw"): sz = 4
                    if is_atomic:
                        sz = 1 if mn.endswith("b") else 2 if mn.endswith("h") else 8 if regs and regs[0].startswith("x") else 4
                if pre and base and base[0] == "g":
                    bv = cur.regs.get(base[1], OTHER)
                    if bv[0] == "arg" and isinstance(imm, int) and isinstance(bv[2], int):
                        cur.regs[base[1]] = ("arg", bv[1], bv[2] + imm); imm = 0
                    elif bv[0] == "init":
                        pass
                if pre and base and base[0] == "sp":
                    cur.sp += imm; imm = 0
                # memory effect
                if is_atomic:
                    if mn.startswith("swp") or mn.startswith("cas"):
                        for r in regs[:-1]: rd(r)
                    else: rd(regs[0])
                    base_hit(base, imm, sz, True); base_hit(base, imm, sz, False)
                    if len(regs) >= 2 and mn.startswith("ld"): wr(regs[1], OTHER)
                    elif mn.startswith("swp") and len(regs) >= 2: wr(regs[1], OTHER)
                    elif mn.startswith("cas") and regs: wr(regs[0], OTHER)
                elif is_store:
                    stat = 1 if mn.startswith(("stxr", "stlxr", "stxp", "stlxp")) else 0
                    vals = []
                    for r in regs[stat:]:
                        rr = r.lstrip("{ ").split(".")[0] if is_vec else r
                        vals.append(peek(rr) if (base and base[0] == "sp") else rd(rr))
                    if stat: wr(regs[0], OTHER)
                    if base and base[0] == "sp":
                        so = cur.sp + (imm if isinstance(imm, int) else 0)
                        step = sz // max(1, len(regs) - stat) if mn in ("stp", "stnp") else sz
                        for i, v in enumerate(vals):
                            if v[0] in ("arg", "init", "const", "ptr") and step == 8:
                                cur.stack[so + i * step] = ("arg", v[1], 0) if v[0] == "init" else v
                            else:
                                cur.stack.pop(so + i * step, None)
                    else:
                        base_hit(base, imm, sz, True)
                else:                                   # load
                    if base and base[0] == "sp":
                        so = cur.sp + (imm if isinstance(imm, int) else 0)
                        step = sz // max(1, len(regs)) if mn in ("ldp", "ldnp") else sz
                        for i, r in enumerate(regs):
                            cur.stack.get(so + i * step)
                            sv = cur.stack.get(so + i * step, OTHER) if step == 8 and preg(r) and preg(r)[0] == "g" else OTHER
                            if sv[0] == "arg" and sv[2] == 0: info.used[sv[1]].add("x")
                            wr(r, sv)
                    else:
                        base_hit(base, imm, sz, False)
                        bv = cur.regs.get(base[1], OTHER) if base and base[0] == "g" else OTHER
                        for r in regs:
                            pr = preg(r.lstrip("{ ").split(".")[0] if is_vec else r)
                            if not pr or pr[0] not in ("g", "f"): continue
                            if pr[0] == "g":
                                if bv[0] == "ptr" and pr[2] == "x": val = ("ptr",)
                                else: val = ("load", 8 if pr[2] == "x" else 4)
                                wr(r, val)
                            else:
                                wr(r, OTHER)
                                cur.fregs[pr[1]] = ("fw", "q" if pr[2] == "v" else pr[2])
                if base and base[0] == "g" and post:
                    bv = cur.regs.get(base[1], OTHER)
                    if bv[0] == "arg" and isinstance(bv[2], int): cur.regs[base[1]] = ("arg", bv[1], bv[2] + post)
                if base and base[0] == "sp" and post: cur.sp += post
        elif mn in ("mov", "orr") and len(o) >= 2 and preg(o[0]) and preg(o[0])[0] == "g" and (
                mn == "mov" or (len(o) == 3 and preg(o[1]) == ("z",) and preg(o[2]))):
            src = o[-1]
            if src.startswith("#") or parse_imm(src) is not None:
                v = parse_imm(src); wr(o[0], ("const", v, 64) if v is not None else OTHER)
            else:
                sr = preg(src)
                if sr and sr[0] == "z": wr(o[0], ("const", 0, 64))
                elif sr and sr[0] == "g":
                    v = rd(src)
                    wr(o[0], ("arg", v[1], 0) if v[0] == "init" else v)
                else:
                    wr(o[0], OTHER)
        elif mn in ("movz", "movn", "movk") and len(o) >= 2:
            imm = parse_imm(o[1]); sh = 0
            if len(o) > 2:
                m = re.search(r'#(\d+)', o[2]); sh = int(m.group(1)) if m else 0
            if imm is None: wr(o[0], OTHER)
            elif mn == "movz": wr(o[0], ("const", imm << sh, 64))
            elif mn == "movn": wr(o[0], ("const", ~(imm << sh), 64))
            else:
                v = rd(o[0])
                wr(o[0], ("const", (v[1] & ~(0xffff << sh)) | (imm << sh), 64) if v[0] == "const" else OTHER)
        elif mn in ("add", "sub", "adds", "subs") and len(o) >= 3 and preg(o[0]):
            d = o[0]; sr = preg(o[1]); imm = parse_imm(o[2])
            if sr and sr[0] == "sp":
                if preg(d) == ("sp",) and imm is not None: cur.sp += imm if mn == "add" else -imm
                else: wr(d, OTHER)
            else:
                v = rd(o[1])
                for t in o[2:]:
                    if preg(t): rd(t)
                if preg(d) == ("sp",): pass
                elif v[0] in ("init", "arg", "init8", "sret") and imm is not None:
                    delta = imm if mn.startswith("add") else -imm
                    if v[0] in ("init", "arg"):
                        base = v[2] if v[0] == "arg" else 0
                        wr(d, ("arg", v[1], base + delta if isinstance(base, int) else "?"))
                    else:
                        base = v[1] if v[0] == "sret" else 0
                        wr(d, ("sret", base + delta))
                elif v[0] in ("init", "arg") and len(o) >= 3 and imm is None:
                    wr(d, ("arg", v[1], "?"))
                elif v[0] == "ptr" and imm is not None: wr(d, ("ptr",))
                elif v[0] == "const" and imm is not None:
                    wr(d, ("const", v[1] + (imm if mn.startswith("add") else -imm), 64))
                else: wr(d, OTHER)
        elif mn in ("adrp", "adr"):
            wr(o[0], ("ptr",))
        elif mn == "cset":
            wr(o[0], ("cset",))
        elif mn == "csinc" and len(o) >= 4 and preg(o[1]) == ("z",) and preg(o[2]) == ("z",):
            wr(o[0], ("cset",))
        elif mn in ("and", "ubfx", "eor") and len(o) >= 3 and preg(o[0]) and preg(o[0])[0] == "g" and (
                (mn != "ubfx" and parse_imm(o[-1]) == 1) or (mn == "ubfx" and o[-1] == "#1")):
            rd(o[1]); wr(o[0], ("and1",))
        elif mn in ("sxtw", "uxtw", "uxtb", "uxth", "sxtb", "sxth", "mov") and len(o) == 2 and preg(o[0]):
            v = rd(o[1]); wr(o[0], v if mn in ("sxtw", "uxtw", "mov") and v[0] in ("const", "call", "arg") else OTHER)
        else:
            # generic: first operand is the destination, the rest are reads
            rmw = mn in ("movk", "bfi", "bfxil", "bfm", "bfc", "bit", "bif", "bsl", "mla", "mls", "fmla", "fmls",
                         "ins") or (o and "[" in o[0] and o[0].startswith("v"))
            dest = o[0] if o and preg(o[0]) else None
            for i, t in enumerate(o):
                if i == 0 and dest and not rmw: continue
                if preg(t): rd(t)
                elif t.startswith("["): pass
            if dest:
                r = preg(dest)
                wr(dest, ("wide",) if (r and r[0] == "g" and r[2] == "x") else OTHER)
                if r and r[0] == "f":
                    cur.fregs[r[1]] = ("fw", "z" if mn == "movi" else ("q" if r[2] == "v" else r[2]))
        a += 4
    lib.busy.discard(key)
    lib.cache[key] = info
    return info

# ----------------------------------------------------------------------------
# classification -> signature
# ----------------------------------------------------------------------------
def ret_kind(lib, info, depth=0, seen=None):
    """-> (kind, conf, consts)"""
    consts, tags = set(), set()
    seen = seen or set()
    def collect(i, d):
        for v in i.ret_vals:
            if v[0] == "const": consts.add(v[1])
            elif v[0] == "call" and isinstance(v[1], int) and d < 3 and v[1] not in seen:
                seen.add(v[1])
                ci = lib.cache.get(v[1]) or analyze(lib, v[1], None, d + 1)
                tags.add("callee")
                collect(ci, d + 1)
            else: tags.add(v[0])
    collect(info, depth)
    if info.sret_used and info.sret_stores or info.sret_fwd:
        return "SRET", ("high" if info.sret_stores and not info.sret_fwd else "low"), consts
    def fl(i, d):
        """-> (n_rets, [float widths])"""
        n, ws = 0, []
        for r in i.rets:
            n += 1
            if r[0] == "f": ws.append(r[1])
            elif r[0] == "c" and d < 3:
                ci = lib.cache.get(r[1]) or analyze(lib, r[1], None, d + 1)
                cn, cw = fl(ci, d + 1)
                if cn and len(cw) * 2 >= cn: ws.append(max(cw, key=cw.count))
        return n, ws
    rn, rws = fl(info, depth)
    if rws and len(rws) * 2 >= rn:
        fw = set(rws)
        k = "F64" if "d" in fw else "F32"
        return k, ("high" if len(rws) == rn and fw != {"z"} else "medium"), consts
    if any(-2000 < c <= -1000 for c in consts):
        return "RESULT", "high", consts
    if consts and consts <= {0, 1} and (tags <= {"and1", "cset", "callee", "other", "load"} or not tags) and (
            "and1" in tags or "cset" in tags or consts == {0, 1}):
        return "BOOL", ("medium" if "callee" in tags else "high") if consts == {0, 1} or "and1" in tags or "cset" in tags else "medium", consts
    if tags & {"and1", "cset"} and not consts: return "BOOL", "medium", consts
    if tags & {"ptr", "arg"} and not consts: return "PTR", "medium", consts
    if consts == {0} and not tags: return "AMBIG01", "low", consts
    if consts == {1} and not tags: return "AMBIG01", "low", consts
    if consts and not tags: return "INT", "medium", consts
    if tags == {"passthru"}: return "VOID/ARG0", "low", consts
    if tags and tags <= {"wide", "other", "fcall"} and not consts: return ("I64/PTR" if "wide" in tags else "UNKNOWN"), "low", consts
    if "load" in tags and not consts: return "INT/PTR", "low", consts
    if "callee" in tags or "call" in tags:
        if 0 in consts: return "RESULT", "low", consts
        return "CALLEE", "low", consts
    return "UNKNOWN", "low", consts

# The dataflow reads x0 = (result of ovrp_GetInitialized) on the uninitialised early-out and calls it a
# BOOL return, although these two never set x0 on the path that matters: read off the disassembly of
# 1.89 / 1.94 / 1.110 (identical in all three). GetFloat zeroes d0 up front and returns s0;
# GetEyeFrustum zeroes d0..d3 when uninitialised and otherwise returns the callee's s0..s3 (an
# ovrpFrustumf, four floats = an HFA).
RET_OVERRIDE = {"ovrp_GetFloat": ("F32", "high"), "ovrp_GetEyeFrustum": ("HFA4", "high")}

def merge_ext(lst):
    """list of (off,size) -> (lo, hi, indexed, unknownsize)"""
    lo = hi = None; idx = False; unk = False
    for o, s in lst:
        if o == "?" or s == "?":
            idx = True if o == "?" else idx; unk = unk or s == "?"
            if o != "?" and s == "?": lo = o if lo is None else min(lo, o)
            continue
        lo = o if lo is None else min(lo, o); hi = o + s if hi is None else max(hi, o + s)
    return lo, hi, idx, unk

def signature(lib, name):
    start, size = lib.exports[name]
    info = analyze(lib, start, start + size, 0)
    rk, rc, consts = ret_kind(lib, info)
    if name in RET_OVERRIDE: rk, rc = RET_OVERRIDE[name]; consts = set()
    ints, floats = {}, {}
    for k in range(8):
        if k in info.used or k in info.ptr or k in info.fwd_unknown:
            u = info.used.get(k, set())
            typ = "ptr" if k in info.ptr else ("i64/ptr" if "x" in u else "i32" if u else "fwd")
            o_lo, o_hi, o_idx, o_unk = merge_ext(info.outs.get(k, ()))
            i_lo, i_hi, i_idx, i_unk = merge_ext(info.ins.get(k, ()))
            fw = sorted(info.fwd_unknown.get(k, ()))
            conf = "high"
            if fw: conf = "low"
            elif any(isinstance(x, tuple) and x for x in ()): pass
            ints[k] = dict(type=typ, out=[o_lo, o_hi] if o_hi is not None else None, out_idx=o_idx,
                           out_unk=o_unk, inp=[i_lo, i_hi] if i_hi is not None else None, fwd=fw, conf=conf)
    for k in range(8):
        if k in info.fused: floats[k] = sorted(info.fused[k])
    sret = None
    if info.sret_used:
        lo, hi, idx, unk = merge_ext(info.sret_stores)
        sret = dict(size=hi, fwd=info.sret_fwd, conf="high" if hi and not info.sret_fwd else "low")
    return dict(name=name, addr=start, size=size, argc=(max(ints) + 1) if ints else 0, ints=ints,
                floats=floats, ret=rk, ret_conf=rc, ret_consts=sorted(consts), sret=sret,
                ret_f=sorted({w for (w, j) in info.ret_fregs}), calls=len(info.calls), opaque=info.opaque)

def sig_str(s):
    a = []
    for k in range(s["argc"]):
        d = s["ints"].get(k)
        if not d: a.append("x%d:?" % k); continue
        t = d["type"]
        if d["out"]: t += "[w %d..%d%s]" % (d["out"][0], d["out"][1], "+idx" if d["out_idx"] else "")
        elif d["out_idx"]: t += "[w idx]"
        if d["inp"]: t += "[r %d..%d]" % (d["inp"][0], d["inp"][1])
        if d["fwd"]: t += "[fwd:%s]" % "/".join(d["fwd"])
        a.append("x%d:%s" % (k, t))
    for k, w in sorted(s["floats"].items()):
        a.append("v%d:%s" % (k, "".join(w)))
    r = s["ret"]
    if s["ret"] == "SRET": r = "SRET(%s)" % (s["sret"]["size"] if s["sret"] and s["sret"]["size"] else "?")
    if s["ret_consts"] and r in ("RESULT", "BOOL", "INT", "AMBIG01"): r += "{%s}" % ",".join(str(c) for c in s["ret_consts"][:4])
    return "%s(%s)" % (r, ", ".join(a))

SOLID = {"RESULT", "BOOL", "F32", "F64", "SRET", "PTR"}

def abi_diff(a, b):
    """reasons why two signatures of the same export are NOT interchangeable (only solid evidence compared)"""
    why = []
    ra, rb = a["ret"], b["ret"]
    fam = lambda k: "HFA" if k.startswith("HFA") else k
    if ra in SOLID and rb in SOLID and a["ret_conf"] != "low" and b["ret_conf"] != "low" and fam(ra) != fam(rb):
        why.append("ret %s -> %s" % (rb, ra))
    if ra == rb == "SRET" and a["sret"] and b["sret"] and a["sret"]["size"] and b["sret"]["size"] and a["sret"]["size"] != b["sret"]["size"] and a["sret"]["conf"] == b["sret"]["conf"] == "high":
        why.append("sret size %s -> %s" % (b["sret"]["size"], a["sret"]["size"]))
    for k in sorted(set(a["ints"]) | set(b["ints"])):
        da, db = a["ints"].get(k), b["ints"].get(k)
        if da and db:
            if da["conf"] == "high" and db["conf"] == "high":
                ta, tb = da["type"], db["type"]
                if {ta, tb} == {"ptr", "i32"}: why.append("x%d %s -> %s" % (k, tb, ta))
                elif da["out"] and db["out"] and da["out"][1] != db["out"][1] and not da["out_idx"] and not db["out_idx"]:
                    why.append("x%d writes %d -> %d bytes" % (k, db["out"][1], da["out"][1]))
        elif (da and da["conf"] == "high" and da["type"] != "fwd") != (db and db["conf"] == "high" and db["type"] != "fwd"):
            if not (a["opaque"] or b["opaque"]):
                why.append("x%d %s" % (k, "added" if da else "removed"))
    fa, fb = set(a["floats"]), set(b["floats"])
    if fa != fb and not (a["opaque"] and b["opaque"]):
        why.append("float args v%s -> v%s" % (",".join(map(str, sorted(fb))), ",".join(map(str, sorted(fa)))))
    return why

# ----------------------------------------------------------------------------
# emulation side (C sources)
# ----------------------------------------------------------------------------
PRIM = {"int8_t": 1, "uint8_t": 1, "char": 1, "unsigned char": 1, "bool": 1, "_Bool": 1, "int16_t": 2, "uint16_t": 2,
        "short": 2, "int": 4, "unsigned": 4, "unsigned int": 4, "int32_t": 4, "uint32_t": 4, "float": 4,
        "ovrpBool": 4, "ovrpResult": 4, "int64_t": 8, "uint64_t": 8, "double": 8, "size_t": 8, "long": 8,
        "unsigned long": 8, "long long": 8, "unsigned long long": 8, "intptr_t": 8, "uintptr_t": 8,
        "ovrpVector2f": 8, "ovrpVector3f": 12, "ovrpQuatf": 16, "ovrpPosef": 28, "ovrpSizei": 8}
FLOATS = {"float": ["f32"], "double": ["f64"], "ovrpVector2f": ["f32"] * 2, "ovrpVector3f": ["f32"] * 3,
          "ovrpQuatf": ["f32"] * 4}
STRUCT_SIZE = {}
STRUCT_HFA = {'ovrpVector2f': 2, 'ovrpVector3f': 3, 'ovrpQuatf': 4}

def load_typedefs(text):
    for m in re.finditer(r'typedef\s+struct\s*\w*\s*\{([^{}]*)\}\s*(\w+)\s*;', text):
        body, nm = m.group(1), m.group(2)
        off, al = 0, 1
        for fm in re.finditer(r'([\w\s]+?)\s+(\*?)(\w+)\s*(?:\[(\w+)\])?\s*;', body):
            ty = fm.group(1).strip().replace("const ", "")
            n = int(fm.group(4)) if fm.group(4) and fm.group(4).isdigit() else 1
            sz = 8 if fm.group(2) else PRIM.get(ty, STRUCT_SIZE.get(ty))
            if sz is None: off = None; break
            a = min(sz, 8) if sz in (1, 2, 4, 8) else 4
            off = (off + a - 1) // a * a + sz * n; al = max(al, a)
        if off is not None:
            STRUCT_SIZE[nm] = (off + al - 1) // al * al
            mem = re.findall(r'(\w+)\s+\w+\s*(?:\[\d+\])?\s*;', body)
            if mem and all(t == "float" for t in mem) and len(mem) <= 4 or re.search(r'float\s+\w+\s*\[\s*([234])\s*\]\s*;', body) and body.count(";") == 1:
                mm = re.search(r'float\s+\w+\s*\[\s*([234])\s*\]', body)
                STRUCT_HFA[nm] = int(mm.group(1)) if mm else len(mem)

def tsize(ty):
    ty = ty.replace("const", "").replace("volatile", "").strip()
    return PRIM.get(ty) or STRUCT_SIZE.get(ty)

class Ours:
    def __init__(self, name, how, fn=None, params=None, ret=None, body="", file="", line=0):
        self.name, self.how, self.fn, self.params, self.ret = name, how, fn, params or [], ret
        self.body, self.file, self.line = body, file, line
        self.sret_thunk = False
        self.sret_param = None

FDEF = re.compile(r'(?m)^(?:static\s+)?(?:inline\s+)?([A-Za-z_][\w\s\*]*?)\s*\b(\w+)\s*\(([^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)\s*\{')

def parse_param(p):
    p = p.strip()
    if p in ("void", ""): return None
    m = re.match(r'(.*?)(\w+)\s*(\[[^\]]*\])?$', p)
    if not m: return dict(raw=p, cls="?", ty=p, name="?")
    ty, nm = m.group(1).strip(), m.group(2)
    if m.group(3): ty += " *"
    ty = ty.replace("const ", "").replace("volatile ", "").replace("restrict ", "").strip()
    ptr = ty.count("*")
    base = ty.replace("*", "").strip()
    if ptr or "(*" in p:
        return dict(cls="ptr", ty=base, name=nm, psize=None if ptr > 1 or base in ("void", "") else tsize(base), ptr=ptr)
    if base in FLOATS: return dict(cls=FLOATS[base], ty=base, name=nm)
    sz = tsize(base)
    if base in STRUCT_SIZE or (base.startswith("ovrp") and base in PRIM and sz and sz > 4):
        return dict(cls="struct", ty=base, name=nm, ssize=sz)
    return dict(cls="i64" if sz == 8 else "i32", ty=base, name=nm)

def match_brace(text, i):
    d = 0
    for j in range(i, len(text)):
        if text[j] == "{": d += 1
        elif text[j] == "}":
            d -= 1
            if d == 0: return j
    return len(text) - 1

def ours_write_extent(o, p):
    """max bytes our body writes through pointer param p, or None if unknown. also 'untouched'"""
    nm = p["name"]; body = o.body
    best = None
    touched = bool(re.search(r'(\*\s*%s\b(?!\s*==)|\b%s\s*->|\b%s\s*\[|\bmemset\s*\(\s*%s\b|\bmemcpy\s*\(\s*%s\b)' % ((nm,) * 5), body))
    for m in re.finditer(r'mem(?:set|cpy)\s*\(\s*(?:\([^)]*\)\s*)?%s\s*,[^,]*,\s*([^;]+?)\)\s*;' % nm, body):
        e = m.group(1).strip()
        if re.fullmatch(r'\d+', e): best = max(best or 0, int(e))
        else:
            sm = re.fullmatch(r'sizeof\s*\(?\s*\*?\s*(\w+)\s*\)?', e)
            if sm:
                t = sm.group(1)
                s = p.get("psize") if t == nm else tsize(t)
                if s: best = max(best or 0, s)
    if re.search(r'\*\s*%s\s*=[^=]|\b%s\s*->\s*\w+(?:\.\w+|\[\w+\])*\s*=[^=]|\b%s\[\w+\]\s*=[^=]' % (nm, nm, nm), body):
        if p.get("psize"): best = max(best or 0, p["psize"])
    return best, touched

def parse_c(paths):
    ours = {}
    shared = {}
    refused = set()
    for path in paths:
        text = open(path, errors="replace").read()
        load_typedefs(text)
    for path in paths:
        text = open(path, errors="replace").read()
        base = os.path.basename(path)
        fns = {}
        for m in FDEF.finditer(text):
            rty, nm, prm = m.group(1).strip(), m.group(2), m.group(3)
            if rty.split()[-1:] and rty.split()[-1] in ("return", "else", "if", "while", "switch"): continue
            end = match_brace(text, m.end() - 1)
            line = text.count("\n", 0, m.start()) + 1
            plist = [parse_param(x) for x in split_ops(prm)]
            fns[nm] = Ours(None, "impl", nm, [x for x in plist if x], rty.replace("static", "").strip(),
                           text[m.end():end], base, line)
        # g_ovrp_impl / k_tab entries
        for m in re.finditer(r'\{\s*"(ovrp_\w+)"\s*,\s*\(void\s*\*\)\s*&?(\w+)\s*\}', text):
            nm, fn = m.group(1), m.group(2)
            o = fns.get(fn)
            if o is None and fn.endswith("_entry"):
                impl = fns.get(fn[:-6] + "_impl")
                if impl:
                    o = Ours(None, "impl", fn, impl.params, impl.ret, impl.body, impl.file, impl.line)
                    o.sret_thunk = True
            if o is None:
                o = Ours(None, "impl?", fn, [], None, "", base, text.count("\n", 0, m.start()) + 1)
            c = Ours(nm, o.how, o.fn, o.params, o.ret, o.body, o.file, text.count("\n", 0, m.start()) + 1)
            c.sret_thunk = o.sret_thunk; c.fnline = o.line
            ours[nm] = c
        # macro-table entries:  #define E(f) {"ovrp_" #f, (void *)klm_##f}
        for dm in re.finditer(r'#define\s+(\w+)\((\w+)\)\s*\{\s*"ovrp_"\s*#\2\s*,\s*\(void\s*\*\)\s*(\w*?)##\2\s*\}', text):
            mac, prefix = dm.group(1), dm.group(3)
            for em in re.finditer(r'\b%s\((\w+)\)' % mac, text):
                nm = "ovrp_" + em.group(1)
                fn = prefix + em.group(1)
                o = fns.get(fn)
                ours[nm] = Ours(nm, "impl", fn, o.params if o else [], o.ret if o else None, o.body if o else "",
                                base, text.count("\n", 0, em.start()) + 1)
        # OK(f) generator macro
        om = re.search(r'#define\s+OK\((\w+)\)\s*static\s+ovrpResult\s+(\w*?)##\1\s*\(void\)\s*\{\s*return\s+kl_ovrp_ok', text)
        if om:
            for em in re.finditer(r'(?m)^OK\((\w+)\)', text):
                nm = "ovrp_" + em.group(1)
                ours[nm] = Ours(nm, "ok-ignores-args", om.group(2) + em.group(1), [], "ovrpResult",
                                "return 0", base, text.count("\n", 0, em.start()) + 1)
        # shared lists
        for lm in re.finditer(r'static\s+const\s+char\s*\*\s*const\s+(g_ovrp_\w+|k_refused)\s*\[\s*\]\s*=\s*\{(.*?)\};', text, re.S):
            lname = lm.group(1)
            kind = {"g_ovrp_result_ok": "shared-ok(0)", "g_ovrp_result_unsupported": "shared-unsupported(-1004)",
                    "g_ovrp_bool_yes": "shared-yes(1)", "g_ovrp_bool_no": "shared-no(0)",
                    "k_refused": "refused(-1004)"}.get(lname)
            if not kind: continue
            body = re.sub(r'//[^\n]*', '', lm.group(2))
            for sm in re.finditer(r'"(\w+)"', body):
                shared.setdefault(sm.group(1), (kind, base, text.count("\n", 0, lm.start() + lm.group(0).find(sm.group(0))) + 1))
        # sret thunks from .S live next to kl_ovrp.c
    sdir = os.path.dirname(paths[0]) if paths else "."
    thunks = {}
    for sfile in sorted(glob.glob(os.path.join(sdir, "kl_ovrp*.S"))):   # kl_ovrp_sret.S, kl_ovrp_sret2.S, ...
        st = open(sfile, errors="replace").read()
        for m in re.finditer(r'_(\w+)_entry:\s*\n\s*mov\s+x(\d+),\s*x8', st):
            thunks[m.group(1)] = int(m.group(2))
    for o in ours.values():
        if o.sret_thunk and o.fn and o.fn.endswith("_entry"):
            o.sret_param = thunks.get(o.fn[:-6])
    return ours, shared

def ours_ret_kind(o):
    r = (o.ret or "").replace("const", "").strip()
    if "*" in r: return "PTR"
    if r in STRUCT_HFA: return "HFA%d" % STRUCT_HFA[r]
    if r in STRUCT_SIZE: return "STRUCT%d" % STRUCT_SIZE[r]
    if r in ("float",): return "F32"
    if r in ("double",): return "F64"
    if r == "void": return "VOID"
    body = o.body or ""
    rets = re.findall(r'return\s+([^;]+);', body)
    if r == "ovrpBool": return "BOOL"
    if r == "ovrpResult" and not rets: return "RESULT"
    res = bool(re.search(r'OVRP_(FAIL|SUCCESS)|KLOVRP_(SUCCESS|INVALID|NOT_INIT|UNSUPP|OPERATION)|-\s*100\d|kl_ovrp_(ok|unsupported)|OVRP_FAIL', " ".join(rets)))
    boo = bool(re.search(r'KLOVRP_TRUE|OVRP_TRUE|\breturn\s+[01]\s*$', " ".join(rets))) or any(re.fullmatch(r'\s*(1|0|!+\s*\w+|\(?.*\)?\s*[!=<>]=?.*)\s*', x) and re.fullmatch(r'\s*[01]\s*|.*(==|!=|<|>|&&|\|\|).*', x) for x in rets)
    if r == "ovrpResult": return "RESULT"
    if res and not boo: return "RESULT"
    if boo and not res: return "BOOL"
    if res and boo: return "MIXED"
    if rets and all(re.fullmatch(r'\s*[01]\s*', x) for x in rets): return "AMBIG01"
    return "INT/UNKNOWN"

# ----------------------------------------------------------------------------
# comparison
# ----------------------------------------------------------------------------
def real_float_count(s):
    return len(s["floats"])

def compare(name, sig, o, shared):
    """-> list of (severity, text). severity: GRAVE | WARN"""
    out = []
    if o is None:
        sh = shared.get(name)
        if sh:
            kind = sh[0]
            if sig["ret"] == "SRET" and sig["sret"] and sig["sret"]["conf"] == "high":
                out.append(("WARN", "%s ignores x8: real returns a %s-byte struct through sret, left unwritten" % (kind, sig["sret"]["size"])))
            outs = [k for k, d in sig["ints"].items() if d["out"] and d["conf"] == "high"]
            if outs:
                out.append(("WARN", "%s writes nothing but real writes out-param(s) x%s" % (kind, ",".join(map(str, outs)))))
            rk, rc = sig["ret"], sig["ret_conf"]
            if kind.startswith("shared-unsupported") or kind.startswith("refused"):
                if rk == "BOOL" and rc != "low":
                    out.append(("WARN", "%s answers -1004 (nonzero = TRUE) where the real returns ovrpBool" % kind))
            if kind in ("shared-ok(0)",) and rk == "BOOL" and rc != "low":
                out.append(("WARN", "shared-ok answers 0 = FALSE where the real returns ovrpBool"))
            if kind == "shared-yes(1)" and rk == "RESULT" and rc != "low":
                out.append(("GRAVE", "shared-yes returns 1 but the real returns ovrpResult (1 is not success)"))
            if kind == "shared-no(0)" and rk == "RESULT" and rc == "high":
                out.append(("WARN", "shared-no returns 0 = success without writing the real out-params"))
        return out
    if o.how == "ok-ignores-args":
        if sig["ret"] == "SRET" and sig["sret"] and sig["sret"]["conf"] == "high":
            out.append(("WARN", "returns without writing the %s-byte sret struct" % sig["sret"]["size"]))
        outs = [k for k, d in sig["ints"].items() if d["out"] and d["conf"] == "high"]
        if outs: out.append(("WARN", "OK() writes nothing; real writes x%s" % ",".join(map(str, outs))))
        if sig["ret"] == "BOOL" and sig["ret_conf"] != "low":
            out.append(("WARN", "OK() returns 0 = FALSE; real is ovrpBool (deliberate for absent hardware if documented)"))
        return out
    if o.how == "impl?": return out
    params = list(o.params)
    # sret thunk: one of our params is really x8
    has_sret = sig["ret"] == "SRET" and sig["sret"] and (sig["sret"]["conf"] == "high" or sig["sret"]["size"])
    if o.sret_thunk and o.sret_param is not None and o.sret_param < len(params):
        params.pop(o.sret_param)
        if not has_sret:
            out.append(("GRAVE", "sret thunk (x8 captured) but the real returns no struct via sret"))
    elif has_sret:
        out.append(("GRAVE", "real returns a %s-byte struct via sret (x8); ours has no x8 thunk" % sig["sret"]["size"]))
    elif sig["sret"] and sig["sret"]["fwd"] and sig["ret"] == "SRET":
        out.append(("WARN", "real forwards x8 (sret) to a helper; struct size unknown, ours has no x8 thunk"))
    # ours: int-class args in order, float list
    oi, of = [], []
    for p in params:
        c = p["cls"]
        if c == "ptr": oi.append(p)
        elif c in ("i32", "i64"): oi.append(p)
        elif c == "struct":
            ss = p["ssize"]
            if ss > 16: oi.append(dict(p, cls="ptr", psize=None))
            else:
                for _ in range((ss + 7) // 8): oi.append(dict(p, cls="i64"))
        elif isinstance(c, list): of.extend(c)
    rints = sig["ints"]
    # arity
    for j, p in enumerate(oi):
        d = rints.get(j)
        if d is None and j >= sig["argc"] and sig["calls"] == 0 and False:
            pass
    # kinds
    for j, p in enumerate(oi):
        d = rints.get(j)
        if not d: continue
        if p["cls"] == "ptr" and d["type"] in ("i32",) and d["conf"] == "high" and not d["fwd"]:
            out.append(("GRAVE", "arg%d: ours dereferences a pointer (%s %s) but the real uses w%d as a 32-bit value" % (j, p["ty"], p["name"], j)))
        if p["cls"] in ("i32", "i64") and d["type"] == "ptr" and d["conf"] == "high" and d["out"]:
            out.append(("WARN", "arg%d: real writes through it (out-param %d..%d bytes) but ours treats it as a value (%s)" % (j, d["out"][0], d["out"][1], p["ty"])))
        if p["cls"] == "ptr":
            real_w = d["out"][1] if d["out"] else 0
            if d["conf"] == "high" and d["type"] == "ptr":
                ext, touched = ours_write_extent(o, p)
                if ext and ext > real_w and not d["out_idx"]:
                    out.append(("GRAVE", "arg%d (%s): ours writes %d bytes, the real writes at most %d" % (j, p["name"], ext, real_w)))
                elif ext and real_w and ext < real_w:
                    out.append(("WARN", "arg%d (%s): ours writes %d bytes, the real writes %d (tail left unwritten)" % (j, p["name"], ext, real_w)))
                elif d["out"] and not touched and o.how == "impl":
                    out.append(("WARN", "arg%d (%s): real writes %d bytes, ours never writes it" % (j, p["name"], real_w)))
    # real pointer out args beyond ours
    for j, d in rints.items():
        if j >= len(oi) and d["out"] and d["conf"] == "high" and o.how == "impl":
            out.append(("WARN", "real writes out-param arg%d (%d bytes) but ours declares only %d integer args" % (j, d["out"][1], len(oi))))
    if len(oi) > sig["argc"] and not sig["fwd_any"] if "fwd_any" in sig else False:
        pass
    # floats
    if len(of) != len(sig["floats"]) and (sig["floats"] or of):
        if sig["floats"] and not of and o.how == "impl":
            out.append(("GRAVE" if len(sig["floats"]) < 8 else "WARN", "real reads float args v%s; ours declares none (the values are lost)" % ",".join(map(str, sig["floats"]))))
        elif of and not sig["floats"]:
            out.append(("WARN" if sig["opaque"] else "GRAVE", "ours declares %d float arg(s) but the real reads no v-registers%s" % (len(of), " (it calls an opaque/virtual function, so it may only forward them: unverifiable)" if sig["opaque"] else "")))
        else:
            out.append(("WARN", "float arg count: real %d (v%s) vs ours %d" % (len(sig["floats"]), ",".join(map(str, sig["floats"])), len(of))))
    # return
    rk, rc = sig["ret"], sig["ret_conf"]
    ok_ = ours_ret_kind(o)
    isf = lambda k: k in ("F32", "F64") or k.startswith("HFA")
    if isf(rk) and isf(ok_):
        pass
    elif rk in ("F32", "F64") and ok_ not in ("F32", "F64"):
        out.append(("GRAVE", "real returns %s in v0; ours returns %s in x0/w0" % (rk, o.ret)))
    if isf(ok_) and not isf(rk) and rk != "SRET":
        out.append(("GRAVE", "ours returns a float in v0; real returns %s in x0/w0" % rk))
    if rk == "BOOL" and ok_ == "RESULT" and rc != "low":
        out.append(("GRAVE", "Bool vs Result: real returns ovrpBool (0/1), ours returns ovrpResult (0 = success / negative)"))
    if rk == "RESULT" and ok_ == "BOOL" and rc != "low":
        out.append(("GRAVE", "Bool vs Result: real returns ovrpResult, ours returns ovrpBool (1 = success)"))
    if rk == "RESULT" and ok_ == "MIXED":
        out.append(("WARN", "returns both Result codes and TRUE: real is ovrpResult"))
    if rk == "PTR" and ok_ not in ("PTR", "INT/UNKNOWN"):
        out.append(("WARN", "real returns a pointer; ours returns %s" % ok_))
    return out

# ----------------------------------------------------------------------------
# report
# ----------------------------------------------------------------------------
def vlabel(l): return l.version

def main(argv):
    libs_p, srcs, report, jfile, games, strict, quiet = [], [], None, None, {}, False, False
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--report": report = argv[i + 1]; i += 1
        elif a == "--json": jfile = argv[i + 1]; i += 1
        elif a == "--game":
            v, n = argv[i + 1].split("=", 1); games.setdefault(v, []).append(n); i += 1
        elif a == "--strict": strict = True
        elif a == "--quiet": quiet = True
        elif a.endswith(".so"): libs_p.append(a)
        elif a.endswith(".c"): srcs.append(a)
        else: sys.exit("ovrp_abi: unknown argument " + a)
        i += 1
    if not libs_p or not srcs: sys.exit(__doc__)
    d = os.path.dirname(os.path.abspath(srcs[0]))
    for extra in ("render", "input", "spatial", "misc"):
        p = os.path.join(d, "kl_ovrp_%s.c" % extra)
        if os.path.exists(p) and os.path.abspath(p) not in map(os.path.abspath, srcs): srcs.append(p)
    libs, seen = [], {}
    for p in libs_p:
        l = Lib(p)
        if l.sha in seen:
            seen[l.sha].paths.append(p); continue
        l.paths = [p]; seen[l.sha] = l; libs.append(l)
    libs.sort(key=lambda l: l.vkey, reverse=True)
    ref = libs[0]
    ours, shared = parse_c(srcs)
    sigs = {}
    for l in libs:
        sigs[l.version] = {n: signature(l, n) for n in l.exports}
    refsig = sigs[ref.version]
    # --- version differences, relative to the reference -----------------------
    names_all = sorted(set().union(*[set(s) for s in sigs.values()]))
    diffs, only_old = {}, {}
    for n in names_all:
        if n in refsig:
            for l in libs[1:]:
                s = sigs[l.version].get(n)
                if s:
                    w = abi_diff(refsig[n], s)
                    if w: diffs.setdefault(n, []).append((l.version, w))
        else:
            only_old[n] = [l.version for l in libs if n in sigs[l.version]]
    missing_in = {l.version: sorted(set(refsig) - set(sigs[l.version])) for l in libs[1:]}
    # --- compare ours vs reference ---------------------------------------------
    findings = {}
    for n, s in refsig.items():
        f = compare(n, s, ours.get(n), shared)
        if f: findings[n] = f
    # also check old-only functions that we implement
    old_findings = {}
    for n in only_old:
        v = [l for l in libs if l.version in only_old[n]][0]
        f = compare(n, sigs[v.version][n], ours.get(n), shared)
        if f: old_findings[n] = (v.version, f)
    grave = [(n, t) for n, fs in findings.items() for sv, t in fs if sv == "GRAVE"]
    grave += [(n + " [only in " + v + "]", t) for n, (v, fs) in old_findings.items() for sv, t in fs if sv == "GRAVE"]
    warn = [(n, t) for n, fs in findings.items() for sv, t in fs if sv == "WARN"]
    # --- output ----------------------------------------------------------------
    line = "ovrp_abi: reference %s (%d exports); %d versions; %d grave, %d warn; %d signatures differ in older versions" % (
        ref.version, len(ref.exports), len(libs), len(grave), len(warn), len(diffs))
    if quiet:
        print(line)
    else:
        print(line)
        for n, t in grave:
            o = ours.get(n.split(" ")[0])
            loc = "%s:%s" % (o.file, getattr(o, "fnline", o.line)) if o else ""
            print("  GRAVE %s %s: %s" % (n, loc, t))
    if jfile:
        json.dump(dict(versions=[dict(version=l.version, sha=l.sha[:12], exports=len(l.exports), paths=[os.path.basename(os.path.dirname(os.path.abspath(p))) for p in l.paths]) for l in libs],
                       signatures={v: {n: dict(s, sig=sig_str(s)) for n, s in ss.items()} for v, ss in sigs.items()},
                       grave=grave, warn=warn), open(jfile, "w"), indent=1, default=str)
    if report:
        write_report(report, libs, ref, sigs, ours, shared, findings, old_findings, diffs, only_old, missing_in, games, srcs)
    if strict and grave: return 1
    return 0

def loc_of(ours, shared, n):
    o = ours.get(n)
    if o: return "%s:%s" % (o.file, getattr(o, "fnline", o.line)) if o.how.startswith("impl") else "%s:%s" % (o.file, o.line)
    s = shared.get(n)
    return "%s:%s" % (s[1], s[2]) if s else ""

def write_report(path, libs, ref, sigs, ours, shared, findings, old_findings, diffs, only_old, missing_in, games, srcs):
    L = []
    w = L.append
    w("# OVRPlugin ABI report (generated by klepton/tools/ovrp_abi.py)\n")
    w("Reference signature: **OVRPlugin SDK %s** (newest). Older versions are listed only where their ABI differs from it.\n" % ref.version)
    w("Method: static AArch64 dataflow over each ovrp_* export (helpers followed to depth 3). Nothing was executed. Confidence: high = seen directly, medium = through a helper / inferred from constants, low = forwarded to a virtual/external call (extent unknown).\n")
    w("## Versions (newest first)\n")
    w("| SDK | exports | sha256[:12] | games |\n|---|---|---|---|")
    for l in libs:
        w("| %s%s | %d | %s | %s |" % (l.version, " (reference)" if l is ref else "", len(l.exports), l.sha[:12], ", ".join(games.get(l.version, [])) or "-"))
    w("")
    w("## Emulation vs reference: GRAVE discrepancies\n")
    gr = [(n, t) for n, fs in findings.items() for sv, t in fs if sv == "GRAVE"]
    if not gr: w("None.\n")
    else:
        w("| function | where | problem |\n|---|---|---|")
        for n, t in gr: w("| %s | %s | %s |" % (n, loc_of(ours, shared, n), t))
        w("")
    og = [(n, v, t) for n, (v, fs) in old_findings.items() for sv, t in fs if sv == "GRAVE"]
    if og:
        w("### GRAVE on functions that exist only in older versions\n")
        w("| function | version | where | problem |\n|---|---|---|---|")
        for n, v, t in og: w("| %s | %s | %s | %s |" % (n, v, loc_of(ours, shared, n), t))
        w("")
    w("## Emulation vs reference: warnings\n")
    w("| function | where | note |\n|---|---|---|")
    for n, fs in sorted(findings.items()):
        for sv, t in fs:
            if sv == "WARN": w("| %s | %s | %s |" % (n, loc_of(ours, shared, n), t))
    w("")
    w("## Older versions: ABI differences from %s\n" % ref.version)
    w("### Signature changed (function exists in both)\n")
    if not diffs: w("None.\n")
    else:
        w("| function | reference %s | older (version: signature; what changed) |\n|---|---|---|" % ref.version)
        for n, vs in sorted(diffs.items()):
            alt = "<br>".join("%s: `%s` (%s)" % (v, sig_str(sigs[v][n]), "; ".join(ws)) for v, ws in vs)
            w("| %s | `%s` | %s |" % (n, sig_str(sigs[ref.version][n]), alt))
        w("")
    w("### Exists only in older versions (not in %s)\n" % ref.version)
    if not only_old: w("None.\n")
    else:
        w("| function | versions | implemented here? | signature (oldest-listed) |\n|---|---|---|---|")
        for n, vs in sorted(only_old.items()):
            how = ours[n].how if n in ours else (shared[n][0] if n in shared else "no (named stub)")
            w("| %s | %s | %s | `%s` |" % (n, ", ".join(vs), how, sig_str(sigs[vs[0]][n])))
        w("")
    w("### Missing from each older version (present in the reference)\n")
    for l in libs[1:]:
        m = missing_in[l.version]
        w("- %s: %d absent%s" % (l.version, len(m), "" if len(m) > 40 else ": " + ", ".join(m)))
    w("")
    w("## Per-function table (reference %s)\n" % ref.version)
    w("`xK` integer/pointer args in order, `vK` float args; `[w a..b]` = bytes written through the pointer; `[r a..b]` = bytes read; `[fwd:...]` = forwarded to an opaque callee (extent unknown, low confidence). Ret: BOOL = 0/1, RESULT = 0 success / -1xxx, SRET = struct via x8.\n")
    w("| function | present in | args / outs / ret | ret conf | arg conf | ours |\n|---|---|---|---|---|---|")
    vers = [l.version for l in libs]
    for n in sorted(sigs[ref.version]):
        s = sigs[ref.version][n]
        pres = [v for v in vers if n in sigs[v]]
        pstr = "all" if len(pres) == len(vers) else ",".join(pres)
        if n in diffs: pstr += " (sig differs: %s)" % ",".join(v for v, _ in diffs[n])
        confs = {d["conf"] for d in s["ints"].values()} or {"-"}
        arg_conf = "low" if "low" in confs else ("high" if "high" in confs else "-")
        o = ours.get(n)
        ostr = (o.how + " " + loc_of(ours, shared, n)) if o else (shared[n][0] if n in shared else "stub")
        w("| %s | %s | `%s` | %s | %s | %s |" % (n, pstr, sig_str(s), s["ret_conf"], arg_conf, ostr))
    open(path, "w").write("\n".join(L) + "\n")

if __name__ == "__main__":
    sys.exit(main(sys.argv))
