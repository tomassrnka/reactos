#!/usr/bin/env python3
# Generate a pinned layout table (w32klayout_<module>.inc) from three dump_layout.py outputs:
#   gen_layout.py I386_NATIVE.jsonl AMD64_NATIVE.jsonl I386_WIDE.jsonl OUT.inc
# A type is wide when its layout in the W32K_WIDE i386 dump differs from the native i386 one. The tool refuses
# to write a table when a dump holds an error record or lacks a type, or when a wide type does not reproduce
# the amd64 offsets, sizes and alignment.
import json, sys

def load(fn):
    d, err = {}, []
    for line in open(fn):
        j = json.loads(line)
        if "error" in j:
            err.append(f"{fn}: {j['type']}: {j['error']}")
        else:
            d[j["type"]] = j
    return d, err

def expr_of(path):
    segs = [s for s in path.split(".") if s and s != "<anon>"]
    return segs, ".".join(segs).replace("[]", "[0]")

def tops(j):
    return {t[0]: (int(t[4]), t[2]) for t in j["top"] if len(t) > 4 and t[0] and t[4] is not None}

def native_leaves(j):
    out = {}
    for f in j["fields"]:
        if f["kind"] == "bitfield":
            continue
        segs, e = expr_of(f["path"])
        if e:
            out[e] = (int(f["off"]), f["size"] * f["inner"])
    for k, v in tops(j).items():
        out.setdefault(k, v)
    return out

def wide_leaves(j):
    # leaf -> (offset, sizeof the leaf expression in the wide compile); twins: expression -> (offset, size)
    leaves, twins, elems = {}, {}, []
    for f in j["fields"]:
        if f["kind"] == "bitfield":
            continue
        segs, e = expr_of(f["path"])
        if not e:
            continue
        if segs[-1].endswith("_K64"):
            twins[e] = (int(f["off"]), f["size"] * f["inner"])
        elif segs[-1] == "v64" and len(segs) > 1 and segs[-2].endswith("[]"):
            continue
        elif segs[-1] == "v" and len(segs) > 1 and segs[-2].endswith("[]"):
            arr = ".".join(segs[:-1]).replace("[]", "[0]")[:-3]
            leaves[arr] = (int(f["off"]), 8 * f["n"])
            elems.append((arr + "[0]", int(f["off"]), f["size"]))
        else:
            leaves[e] = (int(f["off"]), f["size"] * f["inner"])
    for k, v in tops(j).items():
        leaves.setdefault(k, v)
    return leaves, twins, elems

(n32, e1), (n64, e2), (wd, e3) = load(sys.argv[1]), load(sys.argv[2]), load(sys.argv[3])
lines, problems = [], e1 + e2 + e3
if not n32:
    problems.append("empty dump")
for a, b, an, bn in ((n32, n64, "i386", "amd64"), (n32, wd, "i386", "wide"), (n64, wd, "amd64", "wide")):
    for t in sorted(set(a) ^ set(b)):
        problems.append(f"{t}: in the {an if t in a else bn} dump only")
for t in n32:
    if t not in n64:
        problems.append(f"{t}: missing in the amd64 dump")
        continue
    if t not in wd:
        problems.append(f"{t}: missing in the wide dump")
        continue
    if n32[t]["align"] is None or n64[t]["align"] is None or wd[t]["align"] is None:
        problems.append(f"{t}: no alignment")
        continue
    l32, l64 = native_leaves(n32[t]), native_leaves(n64[t])
    lw, twins, elems = wide_leaves(wd[t])
    wide = wd[t]["size"] != n32[t]["size"] or wd[t]["align"] != n32[t]["align"] or any(lw.get(k) != l32.get(k) for k in set(lw) | set(l32))
    if wide:
        if wd[t]["size"] != n64[t]["size"] or wd[t]["align"] != n64[t]["align"]:
            problems.append(f"{t}: wide size/align {wd[t]['size']}/{wd[t]['align']} != amd64 {n64[t]['size']}/{n64[t]['align']}")
    lines.append(f"W32K_LAYOUT_TYPE({t}, {n32[t]['size']}, {n64[t]['size']}, {n32[t]['align']}, {n64[t]['align']}, {int(wide)})")
    for k in sorted(set(l32) & set(l64), key=lambda k: (l32[k][0], k)):
        (o32, z32), (o64, z64) = l32[k], l64[k]
        zw, twin = z32, None
        if wide:
            if k not in lw and twins.get(k + "_K64") == (o64, z64):
                lines.append(f"W32K_LAYOUT_FIELD_K64ONLY({t}, {k}, {o32}, {o64}, {z32}, {z64}, {k}_K64)")
                continue
            if k not in lw:
                problems.append(f"{t}.{k}: no wide leaf")
                continue
            ow, zw = lw[k]
            if ow != o64:
                problems.append(f"{t}.{k}: wide offset {ow} != amd64 {o64}")
            if zw != z64:
                twin = k + "_K64" if (k + "_K64") in twins else None
                if twin is None or twins[twin] != (o64, z64):
                    problems.append(f"{t}.{k}: wide size {zw} != amd64 {z64} and no matching _K64 twin")
                    continue
        lines.append(f"W32K_LAYOUT_FIELD({t}, {k}, {o32}, {o64}, {z32}, {z64}, {zw}, {int(wide)})")
        if twin:
            lines.append(f"W32K_LAYOUT_K64({t}, {twin}, {o64}, {z64})")
    if wide:
        for arr, off, vsize in elems:
            lines.append(f"W32K_LAYOUT_K64({t}, {arr}.v, {off}, {vsize})")
            lines.append(f"W32K_LAYOUT_K64({t}, {arr}.v64, {off}, 8)")
    for k in sorted(set(l32) - set(l64), key=lambda k: (l32[k][0], k)):
        lines.append(f"W32K_LAYOUT_ONLY32({t}, {k}, {l32[k][0]}, {l32[k][1]})")
    for k in sorted(set(l64) - set(l32), key=lambda k: (l64[k][0], k)):
        lines.append(f"W32K_LAYOUT_ONLY64({t}, {k}, {l64[k][0]}, {l64[k][1]})")
if problems:
    sys.exit("layout table not written:\n  " + "\n  ".join(problems))
with open(sys.argv[4], "w", newline="\n") as f:
    f.write("/* Generated by gen_layout.py from compiler layouts; do not edit. Regenerate with regen.sh. */\n")
    f.write("\n".join(lines) + "\n")
print(f"{sys.argv[4]}: {sum(1 for l in lines if l.startswith('W32K_LAYOUT_TYPE'))} types, {sum(1 for l in lines if l.startswith('W32K_LAYOUT_FIELD'))} fields")
