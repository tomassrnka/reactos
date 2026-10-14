# gdb -batch -x dump_layout.py <object>: dumps the leaf-field layouts of the probe types as JSON lines.
# Environment: LAYOUT_OUT (jsonl file), LAYOUT_PROBE (probe source whose T(x) entries name the types).
import gdb, json, os, re
out = open(os.environ["LAYOUT_OUT"], "w")
PTRINT = {"ULONG_PTR","LONG_PTR","UINT_PTR","INT_PTR","DWORD_PTR","SIZE_T","SSIZE_T","WPARAM","LPARAM","LRESULT","KAFFINITY","HANDLE_PTR"}
def kind(t):
    # classify a leaf by its outermost typedef name chain
    name = None
    tt = t
    while tt.code == gdb.TYPE_CODE_TYPEDEF:
        n = str(tt.name)
        if n in PTRINT: return "ptrint"
        if name is None: name = n
        tt = tt.target()
    if tt.code == gdb.TYPE_CODE_PTR:
        if name and (name.startswith("H") or name.endswith("HANDLE") or name in ("HDC","HWND","HKL","HIMC")):
            return "handle"
        tgt = tt.target().strip_typedefs()
        if tgt.code == gdb.TYPE_CODE_FUNC: return "funcptr"
        return "ptr"
    if tt.code == gdb.TYPE_CODE_FLT: return "float"
    return "scalar"
def walk(t, base, path, res):
    s = t.strip_typedefs()
    if s.code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
        for f in s.fields():
            if not hasattr(f, "bitpos"): continue
            p = path + "." + (f.name if f.name else "<anon>")
            if f.bitsize:
                res.append({"path": p, "off": (base*8 + f.bitpos)/8.0, "size": 0, "bits": f.bitsize, "kind": "bitfield", "n": 1})
                continue
            walk(f.type, base + f.bitpos//8, p, res)
            if s.code == gdb.TYPE_CODE_UNION and f.name is None: pass
    elif s.code == gdb.TYPE_CODE_ARRAY:
        el = s.target()
        n = s.sizeof // el.sizeof if el.sizeof else 0
        es = el.strip_typedefs()
        if es.code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
            # expand first element, record count
            sub = []
            walk(el, base, path + "[]", sub)
            for x in sub: x["n"] = x.get("n",1) * n
            res.extend(sub)
        else:
            res.append({"path": path, "off": base, "size": el.sizeof, "kind": kind(el), "n": n, "inner": n})
    else:
        res.append({"path": path, "off": base, "size": s.sizeof, "kind": kind(t), "n": 1, "inner": 1})
names = [n for n in re.findall(r"\bT\((\w+)\)", open(os.environ["LAYOUT_PROBE"]).read()) if n != "x"]
for n in names:
    try:
        t = gdb.lookup_type(n)
    except gdb.error as e:
        out.write(json.dumps({"type": n, "error": str(e)}) + "\n"); continue
    res = []
    walk(t, 0, "", res)
    top = []
    for f in t.strip_typedefs().fields():
        if f.bitsize: top.append([f.name, "bitfield", 0, "bitfield"]); continue
        tn = str(f.type); s2 = f.type.strip_typedefs()
        if s2.code == gdb.TYPE_CODE_ARRAY: k = "array:" + kind(s2.target())
        elif s2.code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION): k = "struct"
        else: k = kind(f.type)
        top.append([f.name, tn, f.type.sizeof, k, f.bitpos // 8 if f.name else None])
    al = None
    try:
        for f in gdb.lookup_type("probe_al_" + n).strip_typedefs().fields():
            if f.name == "t": al = f.bitpos // 8
    except gdb.error:
        pass
    out.write(json.dumps({"type": n, "size": t.sizeof, "align": al, "fields": res, "top": top}) + "\n")
out.close()
