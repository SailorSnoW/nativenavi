#!/usr/bin/env python3
# c1_guard.py <dmsh_b version dir> <output tsv>: lists the shaders whose c1 reads are not proven to stay in the span a shared c1 bank gives them (a listed
# shader gets an exclusive 64 KiB of zeros). Proven: interval arithmetic gives 0 <= index and 16 * (index + 1) <= span (_m0 is float4[4096]).
import datetime
import hashlib
import json
import multiprocessing
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import romfs   # noqa: E402
import shader_lib   # noqa: E402


ACCESS = re.compile(r"\b[a-z]+_c1\)\._m0\[([^\]]+)\]")
DEFINITION = re.compile(r"\b(u?int|u?short|u?long|bool)\s+(_\d+)\s*=\s*([^;]+);")
ASSIGNMENT = re.compile(r"(?<![\w.])(_\d+)\s*(?:<<|>>|[-+*/%&|^])?=(?!=)|(?<![\w.])(_\d+)\s*(?:\+\+|--)|(?:\+\+|--)\s*(_\d+)\b")
TOKEN = re.compile(r"\s*(0[xX][0-9a-fA-F]+[uU]?|\d+\.\d*(?:[eE][-+]?\d+)?f?|\d+[uU]?|[A-Za-z_]\w*(?:\.\w+)*|\.\w+|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^~!?:(),<>\[\]])")
UNBOUNDED = (None, None)
CASTS = {"int", "uint", "short", "ushort", "long", "ulong"}
OUTPUT = re.compile(r"[0-9a-f]{16}_b\w*\.(metal|json)")
BINARY = {"||": 1, "&&": 2, "|": 3, "^": 4, "&": 5, "==": 6, "!=": 6, "<": 7, ">": 7, "<=": 7, ">=": 7, "<<": 8, ">>": 8, "+": 9, "-": 9, "*": 10, "/": 10, "%": 10}


def tokens(text):
    # A reinterpreting cast: an unsigned result is only known to be non-negative, a signed one is unbounded
    text = re.sub(r"\bas_type<u(?:int|short|long)>\s*\(", "as_type_u(", text)
    text = re.sub(r"\bas_type<\w+>\s*\(", "as_type(", text)
    out, i = [], 0
    while i < len(text):
        m = TOKEN.match(text, i)
        if not m:
            if text[i:].strip(): raise ValueError(text[i:])
            break
        out.append(m.group(1))
        i = m.end()
    return out


def known(*values):
    return all(v is not None for v in values)


class Interval:
    """Definitions are evaluated once each, at most 32 deep."""
    def __init__(self, definitions):
        self.definitions, self.memo, self.depth = definitions, {}, 0

    def variable(self, name):
        if name in self.memo: return self.memo[name]
        if name not in self.definitions or self.depth > 32: return UNBOUNDED
        self.memo[name] = UNBOUNDED   # a cycle is unbounded
        self.depth += 1
        kind, text = self.definitions[name]
        try: value = self.expression(text)
        finally: self.depth -= 1
        if kind == "bool": value = (0, 1)
        elif kind.startswith("u") and not (known(value[0]) and value[0] >= 0): value = (0, None)   # a possibly negative value wraps to a huge unsigned one
        self.memo[name] = value
        return value

    def expression(self, text):
        try:
            self.toks, self.pos = tokens(text), 0
            value = self.ternary()
            if self.pos != len(self.toks): return UNBOUNDED
            return value
        except (ValueError, IndexError, RecursionError):
            return UNBOUNDED

    def peek(self): return self.toks[self.pos] if self.pos < len(self.toks) else None

    def take(self, want=None):
        t = self.toks[self.pos]
        if want is not None and t != want: raise ValueError(t)
        self.pos += 1
        return t

    def ternary(self):
        cond = self.binary(1)
        if self.peek() != "?": return cond
        self.take("?")
        a = self.ternary()
        self.take(":")
        b = self.ternary()
        return (min(a[0], b[0]) if known(a[0], b[0]) else None, max(a[1], b[1]) if known(a[1], b[1]) else None)

    def binary(self, level):
        left = self.unary()
        while self.peek() in BINARY and BINARY[self.peek()] >= level:
            op = self.take()
            right = self.binary(BINARY[op] + 1)
            left = self.combine(op, left, right)
        return left

    @staticmethod
    def combine(op, a, b):
        (al, ah), (bl, bh) = a, b
        if op in ("||", "&&", "==", "!=", "<", ">", "<=", ">="): return (0, 1)
        if op == "+": return (al + bl if known(al, bl) else None, ah + bh if known(ah, bh) else None)
        if op == "-": return (al - bh if known(al, bh) else None, ah - bl if known(ah, bl) else None)
        if op == "*":
            if not known(al, ah, bl, bh): return UNBOUNDED
            p = [al * bl, al * bh, ah * bl, ah * bh]
            return (min(p), max(p))
        if op in ("/", ">>", "<<"):
            if not known(al, ah, bl, bh) or bl != bh or bl < 0 or (op == "/" and bl == 0): return UNBOUNDED
            if op == "<<": return (al << bl, ah << bl)
            if op == ">>": return (al >> bl, ah >> bl)
            return (al // bl, ah // bl) if al >= 0 else UNBOUNDED
        if op == "%":
            if not known(bl, bh) or bl != bh or bl <= 0: return UNBOUNDED
            return (0, bl - 1) if known(al) and al >= 0 else (-(bl - 1), bl - 1)
        if op == "&":   # x & K with K >= 0 lies in [0, K], whatever the sign of x
            caps = [h for l, h in (a, b) if known(l, h) and l >= 0]
            return (0, min(caps)) if caps else UNBOUNDED
        if op in ("|", "^"):
            if not known(al, ah, bl, bh) or al < 0 or bl < 0: return UNBOUNDED
            return (0, (1 << max(ah, bh).bit_length()) - 1)
        return UNBOUNDED

    def unary(self):
        t = self.peek()
        if t == "-":
            self.take()
            l, h = self.unary()
            return (-h if known(h) else None, -l if known(l) else None)
        if t == "+":
            self.take()
            return self.unary()
        if t == "!":
            self.take()
            self.unary()
            return (0, 1)
        if t in ("~", "*"):   # a dereference reads memory
            self.take()
            self.unary()
            return UNBOUNDED
        return self.postfix(self.primary())

    def postfix(self, value):
        while self.peek() is not None and (self.peek() == "[" or self.peek().startswith(".")):
            if self.take() == "[":
                self.ternary()
                self.take("]")
            value = UNBOUNDED
        return value

    def primary(self):
        t = self.take()
        if t == "(":
            v = self.ternary()
            self.take(")")
            return v
        if re.fullmatch(r"0[xX][0-9a-fA-F]+[uU]?|\d+[uU]?", t):
            v = int(t.rstrip("uU"), 0)
            return (v, v)
        if re.fullmatch(r"\d+\.\d*(?:[eE][-+]?\d+)?f?", t): return UNBOUNDED
        if not re.fullmatch(r"[A-Za-z_]\w*(?:\.\w+)*", t): raise ValueError(t)
        if self.peek() == "(":
            self.take("(")
            args = []
            if self.peek() != ")":
                args.append(self.ternary())
                while self.peek() == ",":
                    self.take(",")
                    args.append(self.ternary())
            self.take(")")
            return self.call(t, args)
        if re.fullmatch(r"_\d+", t):
            save = (self.toks, self.pos)
            try: return self.variable(t)
            finally: self.toks, self.pos = save
        if t in ("true", "false"): return (0, 1)
        return UNBOUNDED

    @staticmethod
    def call(name, args):
        if name == "as_type_u" and len(args) == 1: return (0, None)
        if name in CASTS and len(args) == 1:
            l, h = args[0]
            if name.startswith("u") and not (known(l) and l >= 0): return (0, None)
            return (l, h)
        if name == "min" and len(args) == 2:
            (al, ah), (bl, bh) = args
            hs = [h for h in (ah, bh) if h is not None]
            return (min(al, bl) if known(al, bl) else None, min(hs) if hs else None)
        if name == "max" and len(args) == 2:
            (al, ah), (bl, bh) = args
            ls = [l for l in (al, bl) if l is not None]
            return (max(ls) if ls else None, max(ah, bh) if known(ah, bh) else None)
        if name == "clamp" and len(args) == 3:
            return (args[1][0], args[2][1])
        return UNBOUNDED


def definitions_of(source):
    """Integer temporaries written only where declared. One assigned again, or declared twice with differing text (vs_depth's main0_z copy), is left out: unbounded."""
    writes, decls = {}, {}
    for m in ASSIGNMENT.finditer(source):
        name = m.group(1) or m.group(2) or m.group(3)
        writes[name] = writes.get(name, 0) + 1
    for kind, name, text in DEFINITION.findall(source): decls.setdefault(name, set()).add((kind, text.strip()))
    counts = {}
    for _, name, _ in DEFINITION.findall(source): counts[name] = counts.get(name, 0) + 1
    return {name: next(iter(d)) for name, d in decls.items() if len(d) == 1 and writes.get(name, 0) == counts[name]}


def proven(index, definitions, span):
    low, high = Interval(definitions).expression(index)
    return span is not None and known(low, high) and low >= 0 and 16 * (high + 1) <= span


def outputs(version_dir, ext):
    """Sorted; skips the AppleDouble `._<name>` files macOS writes on exFAT / FAT drives"""
    return [n for n in sorted(romfs.os.listdir(version_dir)) if (m := OUTPUT.fullmatch(n)) and m.group(1) == ext]


# With DMSH_TABLE_MEMO set to a folder, c1_guard.py and cb_extent.py keep each shader's answer in <tool>.memo, keyed by the SHA-256 of the files
# it read; the first line is MEMO_CODE's fingerprint, and the memo holds the last run's answers only
MEMO_ENV = "DMSH_TABLE_MEMO"
MEMO_CODE = ("c1_guard.py", "cb_extent.py", "dmsh_abi.py", os.path.join("..", "nvn", "dmsh_abi.h"))
# dmsh_abi.py reads its kinds from the header, so the header changes cb_extent.py's answers
_memo = None   # None: no memo, nothing is hashed


def memo_path(tool):
    folder = os.environ.get(MEMO_ENV)
    return os.path.join(folder, tool + ".memo") if folder else None


def memo_fingerprint(tool):
    h = hashlib.sha256(tool.encode())
    for name in MEMO_CODE:
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), name), "rb") as stream: h.update(b"\0" + stream.read())
    return h.hexdigest()


def memo_load(tool):
    path = memo_path(tool)
    if not path: return None
    try:
        with open(path, encoding="utf-8") as stream:
            if stream.readline().rstrip("\n") != "# " + memo_fingerprint(tool): return {}
            return dict(line.rstrip("\n").split("\t", 1) for line in stream if "\t" in line)
    except (OSError, ValueError):
        return {}


def memo_save(tool, answers):
    path = memo_path(tool)
    if not path: return
    tmp = f"{path}.tmp-{os.getpid()}"
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(tmp, "w", encoding="utf-8", newline="") as stream:
            stream.write("# " + memo_fingerprint(tool) + "\n")
            stream.writelines(f"{digest}\t{answer}\n" for digest, answer in dict(answers).items())
        os.replace(tmp, path)
    except OSError:
        try: os.unlink(tmp)
        except OSError: pass


def _memo_start(memo):
    global _memo
    _memo = memo


def memo_map(tool, worker, jobs):
    """worker returns (digest or None, answer, its own value); answers hold no tabs or newlines. → the workers' own values"""
    memo = memo_load(tool)
    with multiprocessing.Pool(initializer=_memo_start, initargs=(memo,)) as pool:   # map keeps the order: the same list as one core's
        results = pool.map(worker, jobs, chunksize=64)
    if memo is not None: memo_save(tool, [(digest, answer) for digest, answer, _ in results if digest])
    return [value for _, _, value in results]


def memo_digest(*files):
    h = hashlib.sha256()
    for data in files: h.update(b"none\0" if data is None else b"%d\0" % len(data) + data)
    return h.hexdigest()


CHAIN_LABEL = re.compile(rb'("chain"\s*:\s*")[^"]*"')


def memo_reflection(data):
    """Without the chain version it names: neither tool reads it, and every digest would miss after a chain change"""
    return None if data is None else CHAIN_LABEL.sub(rb'\1"', data, count=1)


def memo_answer(files, compute):
    if _memo is None: return None, compute()
    digest = memo_digest(*files)
    answer = _memo.get(digest)
    return digest, compute() if answer is None else answer


def read_bytes(path):
    try:
        with romfs.open(path, "rb") as stream: return stream.read()
    except OSError:
        return None


def text_of(data):   # as romfs.open(path, encoding="utf-8") reads: universal newlines
    return data.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")


def c1_span_of(data):
    """its data rounded up to 256 bytes plus the 256-byte zero gap (nvn_mtl.m c1_store); None when missing"""
    if data is None: return None
    try: length = len(json.loads(text_of(data)).get("c1") or "") // 2
    except ValueError: return None
    return (length + 255) // 256 * 256 + 256


def unproven_in(source, reflection):
    accesses = set(ACCESS.findall(source))
    if not accesses: return False
    definitions, span = definitions_of(source), c1_span_of(reflection)
    return not all(proven(index, definitions, span) for index in accesses)


def unproven(job):
    """→ (memo digest, answer, the shader's key when a c1 read is unproven, else None)"""
    version_dir, name = job
    key = name[:-len("_b.metal")] if name.endswith("_b.metal") else name[:-6]
    with romfs.open(os.path.join(version_dir, name), "rb") as stream: metal = stream.read()
    reflection = read_bytes(os.path.join(version_dir, key + "_b.json"))
    digest, answer = memo_answer((metal, memo_reflection(reflection)), lambda: "u" if unproven_in(text_of(metal), reflection) else "-")
    return digest, answer, key if answer == "u" else None


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} <dmsh_b version dir> <output tsv>")
    lib, output = map(os.path.abspath, sys.argv[1:])
    version_dir = str(shader_lib.view(lib))
    jobs = [(version_dir, name) for name in outputs(version_dir, "metal")]
    keys = [key for key in memo_map("c1_guard", unproven, jobs) if key]
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="") as stream:
        stamp = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
        stream.write(f"{shader_lib.table_head(lib)} {stamp}\n")
        for key in keys:
            stream.write(key + "\n")
    print(f"not proven bounded: {len(keys)} shaders")


if __name__ == "__main__":
    main()
