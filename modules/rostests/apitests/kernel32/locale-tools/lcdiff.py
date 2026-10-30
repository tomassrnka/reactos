#!/usr/bin/env python3
"""lcdiff.py oracle.dump nitro.dump [-s]  : compare two ldump outputs, print summary + per-record diffs."""
import sys, collections
def load(p):
    d = collections.OrderedDict()
    for ln in open(p, encoding='utf-8', errors='replace'):
        ln = ln.rstrip('\r\n')
        if not ln or ln.startswith('=====') or ln.startswith('[') or ln.startswith('exit='):
            continue
        f = ln.split('\t')
        if len(f) < 2: continue
        kind = f[0]
        if kind in ('LI','LG'):    key, val = tuple(f[:3]), tuple(f[3:])
        elif kind in ('LN',):  key, val = tuple(f[:3]), tuple(f[3:])
        elif kind == 'CI':     key, val = tuple(f[:4]), tuple(f[4:])
        elif kind == 'CN':     key, val = tuple(f[:4]), tuple(f[4:])
        elif kind in ('ECI','EDF','ETF'):
            # enumerations: key by tag/cal/idx; value rest
            key, val = tuple(f[:5]) if kind != 'ETF' else tuple(f[:4]), tuple(f[5:] if kind != 'ETF' else f[4:])
        elif kind in ('GUDLN','GSDLN','GUDLCID','GSDLCID','GTL'): key, val = (kind,), tuple(f[1:])
        elif kind in ('LN2L','RLN','VALID','CALS','ALL','ALL2'): key, val = tuple(f[:2]), tuple(f[2:])
        elif kind == 'L2LN': key, val = tuple(f[:2]), tuple(f[2:])
        else: key, val = tuple(f), ()
        d[key] = val
    return d
if __name__ == '__main__':
    o, n = load(sys.argv[1]), load(sys.argv[2])
    diffs = []
    for k in o:
        if k not in n: diffs.append((k, o[k], None))
        elif o[k] != n[k]: diffs.append((k, o[k], n[k]))
    for k in n:
        if k not in o: diffs.append((k, None, n[k]))
    if '-s' in sys.argv:
        c = collections.Counter((k[0], k[1] if k[0] in ('LI','LN','CI','CN','ECI','EDF','ETF','CALS') else '') for k, _, _ in diffs)
        for (a, b), v in sorted(c.items()): print(v, a, b)
        sys.exit(0)
    for k, a, b in diffs:
        print('\t'.join(k), '| oracle:', a, '| nitro:', b)
