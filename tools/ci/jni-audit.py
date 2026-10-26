#!/usr/bin/env python3
"""jni-audit.py - read-only audit of the synthetic JNI surface (klepton/runtime/jni/*.c).

  jni-audit.py [--class-prefix P ...] [--log FILE ...]

Without --log: the inventory from the source alone. Every binding row {"cls","name","sig",fn} is
classified as REAL (a handler of its own) or STUB (a shared no-op / constant-false handler), and the
AndroidThunkJava_* table of kl_jni_ue4.c is listed with its S/H/O class.
With --log: the game's own log is read for what the guest actually reached and found NOT served:
  "no host implementation for C.m(sig)"   -> UNLINKED (SIMULATED 0/false/null)
  "NO-OP game Java void C.m(sig)"         -> UNLINKED (APK-owned void, no body)
  "noop_hit name=C.m(sig)"                -> reached a no-op (stub row, table H/O row, or unlinked)
  "RegisterNatives C.m(sig)"              -> natives the guest registered; a class with natives and no
                                             row at all is listed (its Java side is entirely absent)
--class-prefix keeps only classes starting with P (default: all; e.g. com/unity3d com/oculus).
Needs no APK: the unlinked list for a game comes from a log of that game. Nothing is written.
"""
import argparse, glob, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
JNI = os.path.join(ROOT, "klepton", "runtime", "jni")
STUBS = {"klj_void_noop", "klj_GA_void_noop", "klj_false", "klj_fmod_void", "klj_ue4_receiver_stop", "klj_Sentry_init"}
ROW = re.compile(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*((?:"[^"]*"\s*)+),\s*(\w+)\s*\}')
THUNK = re.compile(r"\{\"(\w+)\",\s*'(.)',\s*'(.)'")

def rows():
    out = []
    for f in sorted(glob.glob(os.path.join(JNI, "*.c"))):
        for m in ROW.finditer(open(f, encoding="utf-8").read()):
            sig = "".join(re.findall(r'"([^"]*)"', m.group(3)))
            out.append((m.group(1).replace(".", "/"), m.group(2), sig, m.group(4), os.path.basename(f)))
    return out

def thunks():
    s = open(os.path.join(JNI, "kl_jni_ue4.c"), encoding="utf-8").read()
    a, b = s.index("KL-TEST-BEGIN ue4thunk"), s.index("KL-TEST-END ue4thunk")
    return [(m.group(1), m.group(2), m.group(3)) for m in THUNK.finditer(s[a:b])]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--class-prefix", nargs="*", default=[""])
    ap.add_argument("--log", nargs="*", default=[])
    o = ap.parse_args()
    keep = lambda c: any(c.startswith(p) for p in o.class_prefix)
    R = [r for r in rows() if keep(r[0])]
    bound = {(c, n) for c, n, *_ in R}
    bound_cls = {c for c, *_ in R}
    print("== binding rows: %d (%d REAL, %d STUB)" % (len(R), sum(r[3] not in STUBS for r in R), sum(r[3] in STUBS for r in R)))
    for c in sorted(bound_cls):
        rs = [r for r in R if r[0] == c]
        print("  %s: %d REAL, %d STUB%s" % (c, sum(r[3] not in STUBS for r in rs), sum(r[3] in STUBS for r in rs),
              "  [stubs: %s]" % " ".join(sorted({r[1] for r in rs if r[3] in STUBS})) if any(r[3] in STUBS for r in rs) else ""))
    if keep("com/epicgames/ue4/GameActivity"):
        T = thunks()
        print("== AndroidThunkJava_* table (kl_jni_ue4.c): %d rows" % len(T))
        for cls in "SHO":
            print("  %s: %s" % (cls, " ".join(n for n, k, c in T if c == cls)))
    if not o.log:
        return 0
    unl, noop, nat = {}, {}, {}
    for f in o.log:
        for line in open(f, encoding="utf-8", errors="replace"):
            m = re.search(r"no host implementation for (\S+?)\.([^.(\s]+)(\(\S*)", line) or \
                re.search(r"NO-OP game Java void (\S+?)\.([^.(\s]+)(\(\S*)", line)
            if m: unl[(m.group(1), m.group(2), m.group(3))] = 1
            m = re.search(r"noop_hit name=(\S+?)\.([^.(\s]+)(\(\S*)", line)
            if m: noop[(m.group(1), m.group(2), m.group(3))] = 1
            m = re.search(r"RegisterNatives\s+(\S+?)\.([^.(\s]+)(\(\S*)", line)
            if m: nat[(m.group(1), m.group(2), m.group(3))] = 1
    print("== UNLINKED in the logs (SIMULATED or APK-owned void): %d" % sum(keep(c) for c, _, _ in unl))
    for c, n, s in sorted(k for k in unl if keep(k[0])): print("  %s.%s%s" % (c, n, s))
    print("== noop_hit in the logs: %d" % sum(keep(c) for c, _, _ in noop))
    for c, n, s in sorted(k for k in noop if keep(k[0])):
        why = "unlinked" if (c, n, s) in unl else "STUB row" if (c, n) in bound else "table/other"
        print("  %s.%s%s  [%s]" % (c, n, s, why))
    # UE5 com/epicgames/unreal/X is served by the ue4 rows (kl_jni.c klj_resolve_binding aliases it).
    ue = lambda c: "com/epicgames/ue4/" + c[len("com/epicgames/unreal/"):] if c.startswith("com/epicgames/unreal/") else c
    bare = sorted({c for c, _, _ in nat if keep(c) and c not in bound_cls and ue(c) not in bound_cls})
    print("== classes with registered natives and NO binding row: %d" % len(bare))
    for c in bare: print("  " + c)
    return 0

sys.exit(main())
