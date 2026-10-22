"""Register caching: keeps a function's guest registers in C++ locals instead of the CPU context.

The emitter writes every register access as `c.x[n]`. Guest loads and stores go through pointers made from
integers, which the host compiler cannot prove do not point into the CPU context, so after every guest store it
reloads the registers it needs from memory (SP most of all). Locals whose address is never taken live in host
registers instead.

The context stays the calling convention between recompiled functions and the runtime: it is brought up to date
around everything that can read or write it (calls, syscalls, setjmp, traps, returns).
  - Spill: before such a point, the registers that may have been written since the last one (forward "may be
    dirty" dataflow over the function's gotos). A clean local equals its context slot, so it needs no store.
  - Fill: after a call, every register the function uses. Fills whose value is never read are dead loads that the
    host compiler deletes.
No calling-convention assumption is made (outlined functions and tail-called fragments read any register), so this
is exact: between two sync points nothing else can observe the context.

Flags (NZCV) and the vector registers are cached the same way. Measured on the I3DL2 reverb (x86-64, the costliest
guest function): 9.7 us per call with everything in the context, 7.1 with the flags cached, 4.1-4.7 with general,
vector and flag registers cached.

NX_REGCACHE=0 at recompile time turns it off; NX_REGCACHE_REGS=<n,n,...> caches only those general registers,
NX_REGCACHE_FLAGS=0 / NX_REGCACHE_V=0 leave the flags / vector registers in the context.
"""
import os
import re

REG = re.compile(r"\bc\.x\[(\d+)\]")


def _sub(m):
    return f"x{m.group(1)}" if int(m.group(1)) in CACHED else m.group(0)
WRITE = re.compile(r"\bx(\d+)\s*(?:[-+|&^]?=(?!=)|\+\+|--)")
GOTO = re.compile(r"\bgoto L_([0-9A-F]+);")
LABEL = re.compile(r"^L_([0-9A-F]+):;$")
# Helpers that receive the context but only touch flags, the exclusive monitor or system registers.
PURE = {"nx::subs32", "nx::subs64", "nx::adds32", "nx::adds64", "nx::logic32", "nx::logic64", "nx::set_nzcv",
        "nx::get_nzcv", "nx::clrex", "nx::ldxr", "nx::stxr", "nx::ldxp", "nx::stxp", "nx::fcmp", "nx::fcmpe",
        "nx::mrs", "nx::msr"}
# A call that passes the context: name(c) or name(c, ...), name possibly templated or a js_[k] member.
CALL = re.compile(r"(?<![\w.>])((?:js_\[\w+\]\.)?[A-Za-z_][\w:]*)(<[^<>()]*>)?\(c([,)])")
RESTORE = "js_[jr_].restore(c);"
# NZCV: kept in a local nx::Flags (bit 32 of the dirty masks) and passed to the flag helpers instead of the context.
FLAG = re.compile(r"\bc\.([nzcv]f)\b")
FLAG_HELPER = re.compile(r"\b(nx::(?:subs|adds|logic)(?:32|64)|nx::set_nzcv|nx::get_nzcv|nx::fcmpe?)(<[^<>()]*>)?\(c([,)])")
FLAG_WRITE = re.compile(r"\bnx::(?:subs|adds|logic)(?:32|64)\(fl_|\bnx::set_nzcv\(fl_|\bnx::fcmpe?(?:<[^<>()]*>)?\(fl_|\bfl_\.[nzcv]f\s*=(?!=)")
FL = 32
# Vector registers: c.v[n] -> local nx::V128 vr<n>, bit 33 + n of the dirty masks.
VREG = re.compile(r"\bc\.v\[(\d+)\]")
VWRITE = re.compile(r"\bvr(\d+)(?:\.\w+\[[^\]]*\])?\s*(?:[-+*/|&^]?=(?!=)|\+\+|--)|memcpy\(&vr(\d+)\b")
VB = 33

enabled = os.environ.get("NX_REGCACHE", "1") != "0"
# Which registers to cache (comma list, "all" for x0..x30 and SP).
_regs = os.environ.get("NX_REGCACHE_REGS", "all")
CACHED = set(range(32)) if _regs == "all" else {int(r) for r in _regs.split(",") if r}
FLAGS = os.environ.get("NX_REGCACHE_FLAGS", "1") != "0"
VECTORS = os.environ.get("NX_REGCACHE_V", "1") != "0"


def _call_end(s, start):
    """Index just past the ');' that closes the call whose '(' is at start."""
    depth = 0
    k = start
    while k < len(s):
        ch = s[k]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                k += 1
                break
        elif ch == '"':
            k = s.index('"', k + 1)
        k += 1
    if s.startswith(";", k):
        k += 1
    return k


def _calls(line):
    """[(start, end)] of the context-escaping calls in a statement."""
    res = []
    pos = 0
    for m in CALL.finditer(line):
        if m.start() < pos or m.group(1) in PURE:
            continue
        paren = m.start() + len(m.group(1)) + len(m.group(2) or "")
        end = _call_end(line, paren)
        if line[end - 1] != ";":
            raise ValueError(f"regcache: call inside an expression: {line.strip()}")
        res.append((m.start(), end))
        pos = end
    return res


def _terminal(stmt):
    """The statement never falls through to the next line."""
    s = stmt.split(" // ")[0].strip()
    return (s.startswith("goto ") or s == "return;" or s.startswith("throw") or
            (s.startswith("{") and s.endswith("return; }") and not s.startswith("{ if")) or
            (s.startswith("switch (") and "default: nx::call" in s) or s.startswith("nx::unimpl("))


def apply(lines, header_index):
    """lines: the function's source lines (header at header_index). Returns the rewritten lines."""
    if not enabled:
        return lines
    used = sorted({int(n) for ln in lines for n in REG.findall(ln)} & CACHED)
    flags = FLAGS and any(FLAG.search(ln) or FLAG_HELPER.search(ln) for ln in lines)
    vused = sorted({int(n) for ln in lines for n in VREG.findall(ln)}) if VECTORS else []
    if not used and not flags and not vused:
        return lines
    fill_all = "".join(f" x{n} = c.x[{n}];" for n in used)
    fill_all += "".join(f" vr{n} = c.v[{n}];" for n in vused)
    if flags:
        fill_all += " fl_ = {c.nf, c.zf, c.cf, c.vf};"

    def rewrite(ln):
        if RESTORE in ln:
            return ln
        ln = REG.sub(_sub, ln)
        if vused:
            ln = VREG.sub(r"vr\1", ln)
        if flags:
            ln = FLAG.sub(r"fl_.\1", ln)
            ln = FLAG_HELPER.sub(lambda m: f"{m.group(1)}{m.group(2) or ''}(fl_{m.group(3)}", ln)
        return ln

    lines = [rewrite(ln) for ln in lines]
    n = len(lines)

    # Per line: events in order (('w', reg) | ('sync', start, end)), whether calls are conditional, successors.
    info = []
    labels = {}
    for k, ln in enumerate(lines):
        m = LABEL.match(ln.strip())
        if m:
            labels[m.group(1)] = k
    for k, ln in enumerate(lines):
        if k == header_index or RESTORE in ln:
            info.append(([], False, [k + 1] if k + 1 < n else []))
            continue
        code = ln.split(" // ")[0]
        ev = [(m.start(), "w", int(m.group(1))) for m in WRITE.finditer(code)]
        ev += [(m.start(), "w", FL) for m in FLAG_WRITE.finditer(code)]
        ev += [(m.start(), "w", VB + int(m.group(1) or m.group(2))) for m in VWRITE.finditer(code)]
        ev += [(s, "sync", e) for s, e in _calls(code)]
        ev.sort()
        cond = code.strip().startswith(("if ", "if(", "switch"))
        succ = [labels[t] for t in GOTO.findall(code) if t in labels]
        if not _terminal(code) and k + 1 < n:
            succ.append(k + 1)
        info.append((ev, cond, succ))

    # Forward may-dirty dataflow (bitmasks), iterated to a fixed point.
    din = [0] * n  # the entry, and lines nothing reaches except by exception (the catch handler), start clean

    def transfer(k, d):
        ev, cond, _ = info[k]
        for _, kind, val in ev:
            if kind == "w":
                d |= 1 << val
            elif not cond:
                d = 0
        return d

    changed = True
    while changed:
        changed = False
        for k in range(n):
            o = transfer(k, din[k])
            for s in info[k][2]:
                nd = din[s] | o
                if nd != din[s]:
                    din[s] = nd
                    changed = True

    def spill_of(mask):
        sp = "".join(f"c.x[{r}] = x{r}; " for r in used if mask >> r & 1)
        sp += "".join(f"c.v[{r}] = vr{r}; " for r in vused if mask >> (VB + r) & 1)
        if flags and mask >> FL & 1:
            sp += "c.nf = fl_.nf; c.zf = fl_.zf; c.cf = fl_.cf; c.vf = fl_.vf; "
        return sp

    res = []
    for k, ln in enumerate(lines):
        if k == header_index:
            res.append(ln)
            if used:
                res.append("    uint64_t " + ", ".join(f"x{r} = c.x[{r}]" for r in used) + ";")
            if flags:
                res.append("    nx::Flags fl_{c.nf, c.zf, c.cf, c.vf};")
            if vused:
                res.append("    nx::V128 " + ", ".join(f"vr{r} = c.v[{r}]" for r in vused) + ";")
            continue
        if RESTORE in ln:
            res.append(ln + fill_all)
            continue
        ev, cond, _ = info[k]
        code, sep, comment = ln.partition(" // ")
        d = din[k]
        parts, pos = [], 0
        for start, kind, val in ev:
            if kind == "w":
                d |= 1 << val
                continue
            parts.append(code[pos:start])
            parts.append(f"{{ {spill_of(d)}{code[start:val]}{fill_all} }}")
            pos = val
            if not cond:
                d = 0
        parts.append(code[pos:])
        code = "".join(parts)
        if "return;" in code:
            code = re.sub(r"\breturn;", "{ " + spill_of(d) + "return; }", code)
        res.append(code + sep + comment)
    return res
