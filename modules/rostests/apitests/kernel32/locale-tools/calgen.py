#!/usr/bin/env python3
"""calgen.py ORACLE-ALL.DUMP ORACLE.DUMP OUT.c FULLNAMES
Generate dll/win32/kernel32/winnls/string/calendar_data.c from ldump output measured on Windows 10:
 - the calendars of every locale (what EnumCalendarInfoExEx(ENUM_ALL_CALENDARS) lists, default calendar first)
 - GetCalendarInfo values that the LOCALE_* data cannot give (calendar and era names, era year offsets) and,
   for the locales named in FULLNAMES, every value of the non-default calendars
 - the lists that EnumCalendarInfoExEx hands out where Windows has more than one entry."""
import sys, re, collections
sys.path.insert(0, '.')
import lcdiff

LI_OF_CAL = {5:0x1f, 6:0x20, 47:0x1006, 56:0x78, 58:0x7c}
for i in range(7): LI_OF_CAL[7+i] = 0x2a+i; LI_OF_CAL[14+i] = 0x31+i; LI_OF_CAL[49+i] = 0x60+i
for i in range(12): LI_OF_CAL[21+i] = 0x38+i; LI_OF_CAL[34+i] = 0x44+i
LI_OF_CAL[33] = 0x100e; LI_OF_CAL[46] = 0x100f
LIGHT_TYPES = (2, 4, 57)       # name, era string, abbreviated era string
ENUM_TAGS = {'SMONTHDAY':56, 'SERASTRING':4, 'SABBREVERASTRING':57, 'SSHORTDATE':5, 'SLONGDATE':6, 'SYEARMONTH':47, 'IYEAROFFSETRANGE':3}
UNESC = lambda s: re.sub(r'\\u([0-9a-f]{4})', lambda m: chr(int(m.group(1), 16)), s)

def main():
    oa = lcdiff.load(sys.argv[1]); on = lcdiff.load(sys.argv[2]); out = sys.argv[3]
    full = set(open(sys.argv[4]).read().split())
    names = sorted({k[1] for k in oa if k[0] == 'CALS'})
    lcid = {}
    for n in names:
        v = on.get(('ALL', n))
        if not v or v[0] in ('0x1000', '0x0'): continue
        lcid[n] = int(v[0], 16) & 0xffff
    pool = ['']                      # offset 0 = empty string
    offs = {'': 0}
    size = [1]
    def intern(s):
        if s not in offs:
            offs[s] = size[0]; pool.append(s); size[0] += len(s) + 1
        return offs[s]
    rows = {}; enums = {}; lists = {}
    for n in names:
        if n not in lcid: continue
        lid = lcid[n]
        cals = [int(c) for c in oa[('CALS', n)][0].split(',') if c]
        lists[lid] = cals
        defcal = int(oa[('LN', n, '0x1009')][0])
        for cal in cals:
            for t in range(1, 59):
                k = ('CI', n, str(cal), str(t))
                v = oa.get(k)
                if v is None or v[0] == 'ERR': continue
                val = UNESC(v[1])
                if t == 1: continue                     # ICALINTVALUE is the calendar itself
                if t == 48 and val == '2049': continue
                if t == 3 and val == '0': continue
                if n in full:
                    li = LI_OF_CAL.get(t)
                    if li is not None:
                        lv = oa.get(('LI', n, '0x%x' % li))
                        if lv and UNESC(lv[1]) == val: continue
                else:
                    if t not in LIGHT_TYPES and not (t == 3 and val != '0') and not (t == 48 and val != '2049'):
                        # light locales: only the names and the offset/two-digit-year values
                        continue
                    if t in LI_OF_CAL: continue
                rows[(lid, cal, t)] = intern(val)
            for tag, t in ENUM_TAGS.items():
                items = []
                i = 0
                while True:
                    v = oa.get(('ECI', n, tag, str(cal), str(i)))
                    if v is None: break
                    items.append(UNESC(v[0])); i += 1
                if len(items) > 1 and (n in full or tag in ('SERASTRING', 'SABBREVERASTRING', 'IYEAROFFSETRANGE')):
                    enums[(lid, cal, t)] = intern('\0'.join(items) + '\0')
    # EnumDateFormatsExEx and EnumTimeFormatsEx: the entries of all calendars, each led by the calendar
    for n in names:
        if n not in lcid or n not in full: continue
        lid = lcid[n]
        for tag, t in (('SHORT', 5), ('LONG', 6), ('YEARMONTH', 47)):
            items = []; i = 0
            ks = sorted((k for k in oa if k[0] == 'EDF' and k[1] == n and k[2] == tag and k[3] != 'ERR'), key=lambda k: int(k[4]))
            for k in ks: items.append(chr(int(k[3])) + UNESC(oa[k][0]))
            if items: enums[(lid, 0xFF, t)] = intern('\0'.join(items) + '\0')
        ks = sorted((k for k in oa if k[0] == 'ETF' and k[1] == n and k[3] != 'ERR'), key=lambda k: int(k[3]))
        if ks: enums[(lid, 0xFE, 0)] = intern('\0'.join(UNESC(oa[k][0]) for k in ks) + '\0')
    def lit(s):
        """Wide C string literal of a pool string, NULs written as \\0 and ending with one"""
        o = []; cur = ''
        hexd = '0123456789abcdefABCDEF'
        for i, ch in enumerate(s):
            nxt = s[i + 1] if i + 1 < len(s) else ''
            if ch == '\0':
                cur += '\\0'
                o.append(cur); cur = ''
            elif 0x20 <= ord(ch) < 0x7f and ch not in '"\\':
                cur += ch
            else:
                cur += '\\x%04x' % ord(ch)
                if nxt and nxt in hexd:
                    o.append(cur); cur = ''
        o.append(cur + '\\0')
        return ' '.join('L"%s"' % p for p in o)
    with open(out, 'w', encoding='ascii') as f:
        f.write('/*\n * PROJECT:     ReactOS system libraries\n * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)\n'
                ' * PURPOSE:     Calendar data of the locales (generated by calgen.py from values measured on Windows 10 22H2)\n'
                ' * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)\n */\n\n#include "calendar_data.h"\n\n')
        f.write('/* Strings of the tables below, NUL-terminated and concatenated; offset 0 is the empty string. */\n')
        f.write('static const WCHAR NlsCalPool[] =\n')
        for s in pool:
            f.write('    %s\n' % lit(s))
        f.write(';\n\n')
        f.write('/* Keys are (LANGID << 16) | (calendar << 8) | CALTYPE, sorted */\n')
        f.write('static const NLS_CAL_ROW NlsCalRows[] =\n{\n')
        for (lid, cal, t), o in sorted(rows.items()):
            f.write('    { 0x%08x, %d },\n' % ((lid << 16) | (cal << 8) | t, o))
        f.write('};\n\n')
        f.write('/* Lists of EnumCalendarInfoExEx: NUL-separated strings in the order of Windows */\n')
        f.write('static const NLS_CAL_ROW NlsCalEnumRows[] =\n{\n')
        for (lid, cal, t), o in sorted(enums.items()):
            f.write('    { 0x%08x, %d },\n' % ((lid << 16) | (cal << 8) | t, o))
        f.write('};\n\n')
        f.write('/* Calendars of a locale, the default one first */\nstatic const NLS_CAL_LIST NlsCalLists[] =\n{\n')
        for lid, cals in sorted(lists.items()):
            f.write('    { 0x%04x, %d, { %s } },\n' % (lid, len(cals), ', '.join(str(c) for c in cals)))
        f.write('};\n\n')
        f.write('const WCHAR *NlsCalPoolBase(void) { return NlsCalPool; }\n')
        f.write('const NLS_CAL_ROW *NlsCalRowTable(ULONG *Count) { *Count = ARRAYSIZE(NlsCalRows); return NlsCalRows; }\n')
        f.write('const NLS_CAL_ROW *NlsCalEnumRowTable(ULONG *Count) { *Count = ARRAYSIZE(NlsCalEnumRows); return NlsCalEnumRows; }\n')
        f.write('const NLS_CAL_LIST *NlsCalListTable(ULONG *Count) { *Count = ARRAYSIZE(NlsCalLists); return NlsCalLists; }\n')
    print('rows', len(rows), 'enum', len(enums), 'locales', len(lists), 'pool chars', size[0])
main()
