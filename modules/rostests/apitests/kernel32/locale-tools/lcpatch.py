#!/usr/bin/env python3
"""lcpatch.py ORACLE.DUMP NLSDIR [locale ...]
Rewrite the LOCALE_* entries of NLSDIR/*.nls so that GetLocaleInfoEx(LOCALE_NOUSEROVERRIDE) returns the values
measured by ldump on Windows 10. Only the locales named on the command line (default: every locale in the dump)."""
import sys, re, os, glob, unicodedata, collections

SKIP = {0x58}                       # LOCALE_FONTSIGNATURE: binary, not text
def load_dump(p):
    d = collections.defaultdict(dict)
    global gen
    gen = collections.defaultdict(dict)
    for ln in open(p, encoding='utf-8'):
        f = ln.rstrip('\r\n').split('\t')
        if f[0] == 'LG' and len(f) == 5:
            v = re.sub(r'\\u([0-9a-f]{4})', lambda m: chr(int(m.group(1), 16)), f[4])
            gen[f[1]][int(f[2], 16)] = v
        if f[0] == 'LI' and len(f) == 5 and f[3] != 'ERR':
            v = re.sub(r'\\u([0-9a-f]{4})', lambda m: chr(int(m.group(1), 16)), f[4])
            d[f[1]][int(f[2], 16)] = v
    return d

def macro_table(nlsdir, winnls):
    cnt = collections.defaultdict(collections.Counter)
    vals = {}
    for m in re.finditer(r'#define\s+(LOCALE_\w+)\s+(0x[0-9a-fA-F]+|\d+)\b', open(winnls).read()):
        vals.setdefault(m.group(1), int(m.group(2), 0))
    vals.setdefault('LOCALE_ICONSTRUCTEDLOCALE', 0x7d)
    for p in glob.glob(nlsdir + '/*.nls'):
        for m in re.finditer(r'^\s+(LOCALE_\w+)\s', open(p, encoding='utf-8').read(), re.M):
            n = m.group(1)
            if n in vals: cnt[vals[n]][n] += 1
    tab = {}
    for v, c in cnt.items(): tab[v] = c.most_common(1)[0][0]
    for n, v in vals.items():
        if v not in tab and n not in ('LOCALE_ALL', 'LOCALE_NOUSEROVERRIDE') and v < 0x2000 and n not in (
                'LOCALE_WINDOWS', 'LOCALE_SUPPLEMENTAL', 'LOCALE_ALTERNATE_SORTS', 'LOCALE_REPLACEMENT',
                'LOCALE_NEUTRALDATA', 'LOCALE_SPECIFICDATA'):
            tab[v] = n
    return tab, vals

def parse_lit(lit):
    lit = lit.strip()
    if lit.startswith('L'): lit = lit[1:]
    if not (lit.startswith('"') and lit.endswith('"')): return None
    body = lit[1:-1]
    body = re.sub(r'\\0', '\x00', body)
    return re.sub(r'\\x([0-9a-fA-F]{1,4})', lambda m: chr(int(m.group(1), 16)), body)

def fmt(v, utf8):
    """Resource-compiler literal for v; an embedded NUL (genitive month names) is written as \\0"""
    if '"' in v or '\\' in v: raise ValueError('quote/backslash in value %r' % v)
    parts = v.split('\x00')
    allascii = all(0x20 <= ord(c) < 0x7f for c in v if c != '\x00')
    if allascii: return '"%s"' % '\\0'.join(parts)
    safe = utf8 and all(ord(c) > 0x20 and (ord(c) < 0x7f or unicodedata.category(c)[0] in 'LMNPS') for c in v if c != '\x00')
    if safe: return '"%s"' % '\\0'.join(parts)
    esc = lambda s: ''.join(c if 0x20 <= ord(c) < 0x7f else '\\x%04x' % ord(c) for c in s)
    return 'L"%s"' % '\\x0000'.join(esc(x) for x in parts)

def main():
    dump = load_dump(sys.argv[1]); nlsdir = sys.argv[2]
    only = set(sys.argv[3:])
    tab, vals = macro_table(nlsdir, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../../../sdk/include/psdk/winnls.h'))
    changed = 0
    for p in sorted(glob.glob(nlsdir + '/*.nls')):
        txt = open(p, encoding='utf-8').read()
        m = re.search(r'^\s+LOCALE_SNAME\s+"([^"]*)"', txt, re.M)
        name = m.group(1) if m else None
        if name is None or name not in dump or (only and name not in only): continue
        utf8 = 'code_page(65001)' in txt
        lines = txt.split('\n')
        # locate the LOCALE_ block
        idx = {}      # value -> line index
        first = last = None
        for i, l in enumerate(lines):
            mm = re.match(r'^(\s+)(LOCALE_\w+)\s+(.*)$', l)
            if mm and mm.group(2) in vals:
                idx[vals[mm.group(2)]] = i
                first = i if first is None else first
                last = i
        nchg = 0
        newl = []
        for t, v in sorted(dump[name].items()):
            if t in SKIP: continue
            if t not in tab: continue
            g = gen[name].get(t)
            if g is not None and g != v: v = v + '\x00' + g
            new = '  %s %s' % (tab[t], fmt(v, utf8))
            if t in idx:
                mm = re.match(r'^\s+LOCALE_\w+\s+(.*)$', lines[idx[t]])
                if parse_lit(mm.group(1)) != v:
                    lines[idx[t]] = new; nchg += 1
            else:
                newl.append((tab[t], new)); nchg += 1
        # insert new entries in alphabetical position
        for nm, new in sorted(newl):
            pos = None
            for i in range(first, last + 1):
                mm = re.match(r'^\s+(LOCALE_\w+)\s', lines[i])
                if mm and mm.group(1) > nm: pos = i; break
            if pos is None: pos = last + 1
            lines.insert(pos, new); last += 1
        if nchg:
            open(p, 'w', encoding='utf-8', newline='').write('\n'.join(lines)); changed += 1
            print('%s %s: %d entries' % (os.path.basename(p), name, nchg))
if __name__ == '__main__': main()
