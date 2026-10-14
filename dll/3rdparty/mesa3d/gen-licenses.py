#!/usr/bin/env python3
"""Writes the licence and notice file of the Mesa3D driver from the files that were compiled.

usage: gen-licenses.py WORKDIR > LICENSE.txt     (WORKDIR is the one given to build-mesa.sh, after the build)

Every source file of the two builds (compile_commands.json) and every header they included (ninja -t deps)
that lies in the Mesa tree, in zlib or in the generated files is read. The first comment block of each file
gives its copyright lines and its licence text; files are grouped by licence text, each text is printed once
with all copyright lines found for it. A file with an SPDX tag and no text gets the text of that licence from
Mesa's licenses/ directory. The copyright files of the toolchain runtime that is linked statically are added at the end.
"""
import glob, hashlib, json, os, re, subprocess, sys

W = os.path.realpath(sys.argv[1])
SRC = glob.glob(os.path.join(W, 'src', 'mesa-*'))[0]


def compiled_files():
    files = set()
    for build in sorted(glob.glob(os.path.join(W, 'build-*'))):
        for e in json.load(open(os.path.join(build, 'compile_commands.json'))):
            files.add(os.path.realpath(os.path.join(e['directory'], e['file'])))
        out = subprocess.run(['ninja', '-C', build, '-t', 'deps'], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if line.startswith('    '):
                files.add(os.path.realpath(os.path.join(build, line.strip())))
    return sorted(f for f in files if (f.startswith(SRC + '/') or f.startswith(W + '/build-')) and os.path.isfile(f))


def header_block(path):
    try:
        text = open(path, errors='replace').read(6000)
    except OSError:
        return ''
    m = re.search(r'/\*.*?\*/', text, re.S)
    if m and m.start() < 400:
        raw, end = m.group(0), m.end()
        # a first comment that says nothing of licences (Bison: "A Bison parser, made by ...") is followed by the one that does
        while not re.search(r'permission|redistribut|licen[sc]e|warrant', raw, re.I):
            nxt = re.match(r'\s*(/\*.*?\*/)', text[end:], re.S)
            if not nxt:
                break
            raw += '\n' + nxt.group(1)
            end += nxt.end()
        lines = []
        for l in raw.splitlines():
            l = l.strip()
            l = re.sub(r'^/\*+', '', l)
            l = re.sub(r'\*+/$', '', l)
            l = re.sub(r'^\*+ ?', '', l)
            lines.append(l.rstrip())
        return '\n'.join(lines)
    lines = []
    for l in text.splitlines()[:60]:
        s = l.strip()
        if s.startswith('//') or (s.startswith('#') and not re.match(r'#\s*(include|if|define|pragma|ifndef|ifdef|endif)', s)):
            lines.append(re.sub(r'^(//+|#+) ?', '', s).rstrip())
        elif lines:
            break
    return '\n'.join(lines)


COPY = re.compile(r'^\s*(copyright\b|\(c\)|\xa9|SPDX-FileCopyrightText:)', re.I)
COPY_NOT = re.compile(r'^\s*copyright\s+(?!\(c\)|\xa9|by\b)[a-z]', re.I | re.S)  # "copyright notice", "copyright holders": prose
AUTH = re.compile(r'^\s*(Authors?\b|@author\b|File originally authored by\b)', re.I)
SPDX = re.compile(r'SPDX-License-Identifier:\s*(.+)')
PROSE = re.compile(r'permission|redistribut|warrant|liabil|licen[sc]e|notice|disclaimer|conditions', re.I)


def attributions(lines):
    """The attribution entries of a block (a copyright line with the line it wraps onto, an authors line with
    its names) and the indexes of the lines they use. Nothing else is taken: the licence text stays whole."""
    entries, used = [], set()
    i = 0
    while i < len(lines):
        l = lines[i]
        if COPY.match(l) and not COPY_NOT.match(l):
            parts, used_here = [l.strip()], [i]
            while (not re.search(r'[.;)]\s*$', parts[-1]) and i + 1 < len(lines) and lines[i + 1].strip()
                   and not COPY.match(lines[i + 1]) and not AUTH.match(lines[i + 1])
                   and re.match(r'\s*[A-Z]', lines[i + 1]) and len(lines[i + 1].strip()) < 80
                   and not PROSE.search(lines[i + 1])):
                i += 1
                parts.append(lines[i].strip())
                used_here.append(i)
            entries.append(re.sub(r'\s+', ' ', ' '.join(parts)))
            used.update(used_here)
        elif AUTH.match(l):
            parts, used_here = [l.strip()], [i]
            while (len(parts) < 6 and i + 1 < len(lines) and lines[i + 1].strip() and lines[i + 1][:1] in ' \t'
                   and not PROSE.search(lines[i + 1])):
                i += 1
                parts.append(lines[i].strip())
                used_here.append(i)
            entries.append(re.sub(r'\s+', ' ', ' '.join(parts)))
            used.update(used_here)
        i += 1
    return entries, used


def classify(path):
    block = header_block(path)
    spdx = SPDX.search(block)
    lines = block.splitlines()
    copyrights, used = attributions(lines)
    # the key leaves out the attribution and the SPDX tag; the text that is printed leaves out nothing
    body = [l for n, l in enumerate(lines) if n not in used and not SPDX.search(l)]
    text = block.strip()
    words = re.sub(r'all rights reserved\.?', '', '\n'.join(body), flags=re.I)
    words = re.sub(r'[^a-z0-9]+', ' ', words.lower()).strip()
    licensed = bool(re.search(r'permission|redistribut|provided .as-is|license|licence', words))
    if licensed and len(words) > 150:
        return hashlib.sha1(words.encode()).hexdigest(), text, copyrights, None
    if spdx:
        return 'spdx:' + spdx.group(1).strip(), None, copyrights, spdx.group(1).strip()
    return 'none', None, copyrights, None


def spdx_text(ident):
    p = os.path.join(SRC, 'licenses', ident)
    if not os.path.isfile(p):
        return '(SPDX identifier %s; the text is in the licenses directory of the Mesa tarball)' % ident
    t = open(p).read()
    return t.split('License-Text:', 1)[-1].strip() if 'License-Text:' in t else t.strip()


groups = {}
for f in compiled_files():
    key, text, cr, ident = classify(f)
    g = groups.setdefault(key, {'text': text, 'ident': ident, 'cr': set(), 'files': 0})
    if g['text'] is None and text:
        g['text'] = text
    g['cr'].update(cr)
    g['files'] += 1

out = []
out.append('Licences and notices of the Mesa3D software OpenGL driver (libgallium_wgl.dll)\n'
           'Generated by gen-licenses.py from the source and header files that the two builds compiled.\n'
           'Each licence text is printed once, followed by the copyright lines of the files that carry it.\n')
order = sorted(groups.items(), key=lambda kv: (kv[0] == 'none', -kv[1]['files']))
for n, (key, g) in enumerate(order, 1):
    if key == 'none':
        out.append('=' * 78 + '\nFiles without a licence notice (%d files)\n' % g['files'] + '=' * 78 +
                   '\nThese files carry no licence text of their own; Mesa states that its code is licensed under the\n'
                   'MIT licence unless a file says otherwise (docs/license.rst of the Mesa tarball).\n')
    else:
        title = ('Licence text %d: %s (%d files)' % (n, g['ident'], g['files'])) if g['ident'] and g['text'] is None \
            else ('Licence text %d (%d files)' % (n, g['files']))
        out.append('=' * 78 + '\n' + title + '\n' + '=' * 78 + '\n\n' +
                   (g['text'] if g['text'] else spdx_text(g['ident'])) + '\n')
    if g['cr']:
        out.append('Copyright lines of these files:\n' + '\n'.join('  ' + c for c in sorted(g['cr'])) + '\n')

def doc(path):
    try:
        return open(path, errors='replace').read().strip()
    except OSError:
        sys.exit('gen-licenses.py: %s is missing; the notices of the toolchain runtime come from the packages of the build host' % path)


out.append('=' * 78 + '\nStatically linked toolchain runtime\n' + '=' * 78 + """

The DLL contains the GCC runtime libraries (libgcc, libstdc++), licensed under the GNU General Public License
version 3 with the GCC Runtime Library Exception, and the mingw-w64 runtime. Both are reproduced here as the
packages of the build host give them (Ubuntu 24.04: mingw-w64-common 11.0.1, gcc-mingw-w64-base 13.2.0): the
first block is /usr/share/doc/mingw-w64-common/copyright, which names the terms and holders of every file of the
mingw-w64 runtime, the second is /usr/share/doc/gcc-mingw-w64-base/copyright, which holds the GCC licences with
the Runtime Library Exception.
""")
out.append('-' * 78 + '\nmingw-w64-common: /usr/share/doc/mingw-w64-common/copyright\n' + '-' * 78 + '\n\n' +
           doc('/usr/share/doc/mingw-w64-common/copyright') + '\n')
out.append('-' * 78 + '\ngcc-mingw-w64-base: /usr/share/doc/gcc-mingw-w64-base/copyright\n' + '-' * 78 + '\n\n' +
           doc('/usr/share/doc/gcc-mingw-w64-base/copyright') + '\n')
sys.stdout.write('\n'.join(out))
