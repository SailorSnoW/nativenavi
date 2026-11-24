#!/usr/bin/env python3
# cb_extent.py <dmsh_b version dir> <output tsv>: `<key>\t<reg>:<bytes> ... s<reg> ...`: the cbuf slots whose reads all stay in their first <bytes>
# (a multiple of 16), and the storage slots the shader never writes. A slot left out may be read anywhere, or is written.
import datetime
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from c1_guard import Interval, definitions_of, known, memo_answer, memo_map, memo_reflection, outputs, read_bytes, text_of   # noqa: E402
import dmsh_abi as abi   # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import shader_lib   # noqa: E402

MAX = 65536


def extents(source, bindings):
    out, definitions = {}, None
    for b in bindings:
        if b.get("kind") != abi.KIND_CBUF or "reg" not in b: continue
        name = b["msl_name"]
        uses = len(re.findall(r"\b" + re.escape(name) + r"\b", source))
        reads = re.findall(r"\(\*spvDescriptorSet0\." + re.escape(name) + r"\)\._m0\[([^\]]+)\]", source)
        declared = len(re.findall(r"\b" + re.escape(name) + r"\s*\[\[id\(", source))
        if uses != len(reads) + declared: continue
        high = -1
        for index in set(reads):
            if definitions is None: definitions = definitions_of(source)
            low, top = Interval(definitions).expression(index)
            if not known(low, top) or low < 0:
                high = None
                break
            high = max(high, top)
        if high is None or 16 * (high + 1) >= MAX: continue
        out[b["reg"]] = 16 * (high + 1)
    return out


WRITE = re.compile(r"\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--)")


def unwritten(source, bindings):
    """Only reads of (*<set>.<name>)._m0[...] as a value count as unwritten: SPIRV-Cross types every storage buffer as writable, so the host takes a binding as written unless listed"""
    out = []
    for b in bindings:
        if b.get("kind") != abi.KIND_SBUF or "reg" not in b: continue
        name = "spvDescriptorSet0." + b["msl_name"]
        ok = True
        for m in re.finditer(r"\(\*" + re.escape(name) + r"\b", source):
            j = m.end()
            if source[m.start() - 1:m.start()] == "&" or not source.startswith(")._m0[", j):
                ok = False
                break
            j += len(")._m0")
            depth = 0
            while j < len(source):
                c = source[j]
                if c == "[": depth += 1
                elif c == "]": depth -= 1
                elif depth == 0 and not (c == "." or c.isalnum() or c == "_"): break
                j += 1
            if WRITE.match(source, j):
                ok = False
                break
        if ok and len(re.findall(re.escape(name) + r"\b", source)) == len(re.findall(r"\(\*" + re.escape(name) + r"\b", source)):
            out.append(b["reg"])
    return sorted(set(out))


def tokens(reflection, metal):
    """(reflection json bytes, MSL bytes; None: missing) → the table line's slots, or "" """
    if reflection is None: return ""
    try:
        bindings = json.loads(text_of(reflection)).get("bindings") or []
        if not any(b.get("kind") in (abi.KIND_CBUF, abi.KIND_SBUF) for b in bindings): return ""
        if metal is None: return ""
        source = text_of(metal)
    except ValueError:
        return ""
    return " ".join([f"{r}:{n}" for r, n in sorted(extents(source, bindings).items())] + [f"s{r}" for r in unwritten(source, bindings)])


def memo_row(path):
    reflection, metal = read_bytes(path), read_bytes(path[:-5] + ".metal")
    digest, answer = memo_answer((memo_reflection(reflection), metal), lambda: tokens(reflection, metal) or "-")
    return digest, answer, os.path.basename(path)[:-len("_b.json")] + "\t" + answer if answer != "-" else None


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} <dmsh_b version dir> <output tsv>")
    lib, output = map(os.path.abspath, sys.argv[1:])
    version_dir = str(shader_lib.view(lib))
    paths = [os.path.join(version_dir, n) for n in outputs(version_dir, "json") if n.endswith("_b.json")]
    rows = [r for r in memo_map("cb_extent", memo_row, paths) if r]
    slots, ro = sum(r.count(":") for r in rows), sum(len(re.findall(r"\bs\d+", r.split("\t", 1)[1])) for r in rows)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="") as stream:
        stamp = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
        stream.write(f"{shader_lib.table_head(lib)} {stamp}\n")
        stream.write("# shader key, then reg:bytes for each uniform slot whose reads stay inside its first bytes, and s<reg> for each storage slot it never writes (cb_extent.py)\n")
        for line in rows: stream.write(line + "\n")
    print(f"bounded uniform reads: {slots} slots; storage slots never written: {ro}; {len(rows)} shaders")


if __name__ == "__main__":
    main()
