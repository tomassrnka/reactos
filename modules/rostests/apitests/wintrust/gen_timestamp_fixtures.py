#!/usr/bin/env python3
# PROJECT:     ReactOS API tests
# LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
# PURPOSE:     Generates timestamp_fixtures.h for the wintrust time stamp test
# COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
#
# Usage: gen_timestamp_fixtures.py OUT_HEADER
#
# Needs python3, openssl 3.4 or later (x509/req -not_before) and a mingw gcc. All keys and
# certificates are created here, from scratch, for the test only: a throwaway root, an expired
# code signing certificate, time stamp authority (TSA) certificates, and an untrusted
# self-signed TSA. Nothing real is used. The keys are discarded, the output is not
# reproducible byte for byte but is equivalent every run.
#
# Every signature is an Authenticode PKCS #7 over the same tiny unsigned PE file. Only the
# unauthenticated attribute that carries the time stamp differs between them.

import base64, datetime, hashlib, os, subprocess, sys, tempfile

# ---------------------------------------------------------------- DER

def L(n):
    if n < 0x80: return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b

def tlv(t, c): return bytes([t]) + L(len(c)) + c
def seq(*a): return tlv(0x30, b''.join(a))
def sset(*a): return tlv(0x31, b''.join(sorted(a)))      # DER SET OF: sorted
def ctx(n, c, cons=True): return tlv((0xA0 if cons else 0x80) | n, c)
def octets(b): return tlv(0x04, b)
def null(): return b'\x05\x00'
def integer(n):
    b = n.to_bytes(n.bit_length() // 8 + 1, 'big')
    return tlv(0x02, b)
def oid(s):
    p = [int(x) for x in s.split('.')]
    out = bytes([p[0] * 40 + p[1]])
    for v in p[2:]:
        t = [v & 0x7f]
        v >>= 7
        while v:
            t.append(0x80 | (v & 0x7f)); v >>= 7
        out += bytes(reversed(t))
    return tlv(0x06, out)
def utctime(d): return tlv(0x17, d.strftime('%y%m%d%H%M%SZ').encode())
def gentime(d): return tlv(0x18, d.strftime('%Y%m%d%H%M%SZ').encode())

def read(d, off=0):
    """-> tag, content start, content end"""
    t = d[off]; l = d[off + 1]; p = off + 2
    if l & 0x80:
        n = l & 0x7f; l = int.from_bytes(d[p:p + n], 'big'); p += n
    return t, p, p + l

def children(d, off=0):
    t, s, e = read(d, off)
    out = []
    while s < e:
        _, _, ce = read(d, s)
        out.append(d[s:ce]); s = ce
    return out

OID_SHA256 = '2.16.840.1.101.3.4.2.1'
OID_RSA = '1.2.840.113549.1.1.1'
OID_SIGNED = '1.2.840.113549.1.7.2'
OID_DATA = '1.2.840.113549.1.7.1'
OID_SPC_INDIRECT = '1.3.6.1.4.1.311.2.1.4'
OID_SPC_PE = '1.3.6.1.4.1.311.2.1.15'
OID_CT, OID_MD, OID_ST, OID_CS = ('1.2.840.113549.1.9.%d' % i for i in (3, 4, 5, 6))
OID_RFC3161 = '1.3.6.1.4.1.311.3.3.1'
OID_TSTINFO = '1.2.840.113549.1.9.16.1.4'

def alg(o): return seq(oid(o), null())
def sha256(b): return hashlib.sha256(b).digest()

# ---------------------------------------------------------------- openssl

W = tempfile.mkdtemp(prefix='wtfix')

def sh(*a, **k): return subprocess.run(a, check=True, capture_output=True, **k).stdout

def mkkey(name):
    sh('openssl', 'genrsa', '-out', f'{W}/{name}.key', '2048')

def mkcert(name, cn, nb, na, ca=None, eku=None, serial=1, ekucrit=True):
    """nb/na: YYYYMMDDHHMMSSZ.  ca None: self-signed CA."""
    cfg = ['basicConstraints=critical,CA:TRUE', 'keyUsage=critical,keyCertSign,cRLSign'] if ca is None else \
          ['basicConstraints=critical,CA:FALSE', 'keyUsage=critical,digitalSignature']
    if eku:
        cfg.append(('extendedKeyUsage=critical,' if ekucrit else 'extendedKeyUsage=') + eku)
    open(f'{W}/{name}.cnf', 'w').write('\n'.join(cfg) + '\n')
    csr = sh('openssl', 'req', '-new', '-key', f'{W}/{name}.key', '-subj', f'/CN={cn}')
    cmd = ['openssl', 'x509', '-req', '-in', '-', '-not_before', nb, '-not_after', na, '-sha256',
           '-set_serial', str(serial), '-extfile', f'{W}/{name}.cnf']
    if ca is None:
        cmd += ['-signkey', f'{W}/{name}.key']
    else:
        cmd += ['-CA', f'{W}/{ca}.crt', '-CAkey', f'{W}/{ca}.key']
    pem = sh(*cmd, input=csr)
    open(f'{W}/{name}.crt', 'wb').write(pem)
    return sh('openssl', 'x509', '-in', f'{W}/{name}.crt', '-outform', 'DER')

def rsasign(name, data):
    return sh('openssl', 'dgst', '-sha256', '-sign', f'{W}/{name}.key', input=data)

def issuer_and_serial(cert):
    f = children(children(cert)[0])
    if f[0][0] == 0xA0: f = f[1:]       # skip [0] version
    # f: serial, sigalg, issuer, validity, subject, spki, ...
    return seq(f[2], f[0])

# ---------------------------------------------------------------- the unsigned PE

def build_pe():
    src = f'{W}/pe.c'
    open(src, 'w').write('void __attribute__((stdcall)) start(void) { for (;;) ; }\n')
    out = f'{W}/pe.exe'
    sh('i686-w64-mingw32-gcc', '-nostdlib', '-nostartfiles', '-Wl,-e,_start@0', '-s',
       '-Wl,--file-alignment,512', '-Wl,--section-alignment,4096', '-Wl,--subsystem,console',
       '-Wl,--disable-reloc-section', '-Wl,--no-insert-timestamp', '-Wl,-s', '-o', out, src)
    pe = open(out, 'rb').read()
    assert len(pe) % 8 == 0
    return pe

def pe_digest(pe):
    lfanew = int.from_bytes(pe[0x3c:0x40], 'little')
    opt = lfanew + 24
    assert int.from_bytes(pe[opt:opt + 2], 'little') == 0x10b
    chk = opt + 64
    sec = opt + 0x60 + 4 * 8
    assert pe[sec:sec + 8] == bytes(8)
    h = hashlib.sha256()
    h.update(pe[:chk]); h.update(pe[chk + 4:sec]); h.update(pe[sec + 8:])
    return h.digest()

# ---------------------------------------------------------------- CMS pieces

def spc_indirect(digest):
    data = seq(oid(OID_SPC_PE),
               seq(tlv(0x03, b'\x00\x00'),                       # flags: empty BIT STRING
                   ctx(0, ctx(2, tlv(0x80, '<<<Obsolete>>>'.encode('utf-16-be'))))))
    return seq(data, seq(alg(OID_SHA256), octets(digest)))

def attr(o, *vals): return seq(oid(o), sset(*vals))

def signer_info(key, cert, auth_attrs, unauth=None, bad_sig=False):
    """auth_attrs: list of encoded attributes (already built).  -> (SignerInfo DER, signature)"""
    setd = sset(*auth_attrs)
    sig = rsasign(key, setd)
    if bad_sig:
        sig = sig[:-1] + bytes([sig[-1] ^ 1])
    parts = [integer(1), issuer_and_serial(cert), alg(OID_SHA256), ctx(0, b''.join(sorted(auth_attrs))),
             alg(OID_RSA), octets(sig)]
    if unauth is not None:
        parts.append(ctx(1, b''.join(unauth)))
    return seq(*parts), sig

def signed_data(content_oid, content_der, certs, signers, version=1, octet=False):
    c = content_der
    ci = seq(oid(content_oid), ctx(0, c)) if c is not None else seq(oid(content_oid))
    sd = seq(integer(version), sset(alg(OID_SHA256)), ci,
             ctx(0, b''.join(certs)), sset(*signers) if signers else sset())
    return seq(oid(OID_SIGNED), ctx(0, sd))

# ---------------------------------------------------------------- the time stamp token

def tstinfo(imprint, gen):
    return seq(integer(1), oid('1.2.3.4.1'), seq(alg(OID_SHA256), octets(imprint)), integer(0x1234567),
               gentime(gen))

def tsa_token(key, cert, imprint, gen, mode='ok'):
    """mode: ok | badsig | unsigned | tamper"""
    content_info = tstinfo(imprint, gen)
    signed_info = content_info
    if mode == 'tamper':
        # a genuine signature over a later genTime, carried with a genTime inside the signer's
        # validity: the signed attributes still verify, the content they cover does not
        signed_info = tstinfo(imprint, gen.replace(year=gen.year + 5))
    attrs = [attr(OID_CT, oid(OID_TSTINFO)),
             attr(OID_ST, utctime(gen)),
             attr(OID_MD, octets(sha256(signed_info)))]
    signers = [] if mode == 'unsigned' else [signer_info(key, cert, attrs, bad_sig=(mode == 'badsig'))[0]]
    return signed_data(OID_TSTINFO, octets(content_info), [cert], signers, version=3)

# ---------------------------------------------------------------- main

def b64(name, data, out):
    s = base64.b64encode(data).decode()
    out.write(f'static const char {name}[] =\n')
    for i in range(0, len(s), 76):
        out.write(f'    "{s[i:i + 76]}"\n')
    out.write('    ;\n\n')

def main(outpath):
    for k in ('root', 'leaf', 'tsa', 'tsaold', 'tsanoeku', 'tsaevil'):
        mkkey(k)
    root = mkcert('root', 'ReactOS test time stamp root', '20200101000000Z', '20991231000000Z')
    leaf = mkcert('leaf', 'ReactOS test expired signer', '20200101000000Z', '20210101000000Z', 'root',
                  '1.3.6.1.5.5.7.3.3', 2)
    tsa = mkcert('tsa', 'ReactOS test TSA', '20200101000000Z', '20950101000000Z', 'root',
                 '1.3.6.1.5.5.7.3.8', 3)
    tsaold = mkcert('tsaold', 'ReactOS test TSA, expired', '20200101000000Z', '20220101000000Z', 'root',
                    '1.3.6.1.5.5.7.3.8', 4)
    tsanoeku = mkcert('tsanoeku', 'ReactOS test TSA without time stamping usage', '20200101000000Z',
                      '20950101000000Z', 'root', '1.3.6.1.5.5.7.3.3', 5)
    tsaevil = mkcert('tsaevil', 'ReactOS test self-signed TSA', '20200101000000Z', '20950101000000Z', None,
                     '1.3.6.1.5.5.7.3.8', 6)

    pe = build_pe()
    indirect = spc_indirect(pe_digest(pe))
    # Authenticode digests the content of the SpcIndirectDataContent, not its header
    t, s, e = read(indirect)
    attrs = [attr(OID_CT, oid(OID_SPC_INDIRECT)), attr(OID_MD, octets(sha256(indirect[s:e])))]

    def build(unauth=None, extra=(), content=indirect):
        si, sig = signer_info('leaf', leaf, attrs, unauth)
        return signed_data(OID_SPC_INDIRECT, content, [leaf] + list(extra), [si]), sig

    _, sig = build()
    imprint = sha256(sig)
    other = sha256(b'not the signature')

    def rfc(token): return [attr(OID_RFC3161, token)]

    gen = datetime.datetime(2020, 6, 1, 12, 0, 0)
    fut = datetime.datetime(2090, 1, 1, 12, 0, 0)
    fx = {}
    fx['NoTimeStamp'] = build()[0]
    fx['Rfc3161Good'] = build(rfc(tsa_token('tsa', tsa, imprint, gen)))[0]
    fx['Rfc3161ExpiredTsa'] = build(rfc(tsa_token('tsaold', tsaold, imprint, gen)))[0]
    fx['Rfc3161BadSignature'] = build(rfc(tsa_token('tsa', tsa, imprint, gen, 'badsig')))[0]
    fx['Rfc3161Unsigned'] = build(rfc(tsa_token('tsa', tsa, imprint, gen, 'unsigned')))[0]
    fx['Rfc3161TamperedTime'] = build(rfc(tsa_token('tsa', tsa, imprint, gen, 'tamper')))[0]
    fx['Rfc3161UntrustedTsa'] = build(rfc(tsa_token('tsaevil', tsaevil, imprint, gen)))[0]
    fx['Rfc3161WrongImprint'] = build(rfc(tsa_token('tsa', tsa, other, gen)))[0]
    fx['Rfc3161FutureTime'] = build(rfc(tsa_token('tsa', tsa, imprint, fut)))[0]
    fx['Rfc3161NoEku'] = build(rfc(tsa_token('tsanoeku', tsanoeku, imprint, gen)))[0]

    def counter(key, cert, digest, when, bad=False, tamper=False):
        def attrs_at(t):
            return [attr(OID_CT, oid(OID_DATA)), attr(OID_ST, utctime(t)), attr(OID_MD, octets(digest))]
        if tamper:
            # signed for a later time, carried with a signing time inside the signer's validity
            si, _ = signer_info(key, cert, attrs_at(when.replace(year=when.year + 5)))
            old, new = b''.join(sorted(attrs_at(when.replace(year=when.year + 5)))), b''.join(sorted(attrs_at(when)))
            assert len(old) == len(new) and si.count(old) == 1
            si = si.replace(old, new)
        else:
            si, _ = signer_info(key, cert, attrs_at(when), bad_sig=bad)
        return [seq(oid(OID_CS), sset(si))]

    fx['CounterGood'] = build(counter('tsa', tsa, imprint, gen), [tsa])[0]
    fx['CounterBadSignature'] = build(counter('tsa', tsa, imprint, gen, bad=True), [tsa])[0]
    fx['CounterTamperedTime'] = build(counter('tsa', tsa, imprint, gen, tamper=True), [tsa])[0]
    fx['CounterWrongDigest'] = build(counter('tsa', tsa, other, gen), [tsa])[0]
    fx['CounterNoEku'] = build(counter('tsanoeku', tsanoeku, imprint, gen), [tsanoeku])[0]
    fx['CounterUntrustedTsa'] = build(counter('tsaevil', tsaevil, imprint, gen), [tsaevil])[0]

    # a signature carried over from another file: it covers the content of an
    # SpcIndirectDataContent with another PE's digest, while the content shipped with it is the
    # one that matches this PE
    pe2 = pe[:-1] + bytes([pe[-1] ^ 0x5a])
    t2, s2, e2 = read(spc_indirect(pe_digest(pe2)))
    other_attrs = [attr(OID_CT, oid(OID_SPC_INDIRECT)),
                   attr(OID_MD, octets(sha256(spc_indirect(pe_digest(pe2))[s2:e2])))]
    si, _ = signer_info('leaf', leaf, other_attrs, rfc(tsa_token('tsa', tsa, imprint, gen)))
    fx['ContentSwapped'] = signed_data(OID_SPC_INDIRECT, indirect, [leaf], [si])

    with open(outpath, 'w') as o:
        o.write('/* Generated by gen_timestamp_fixtures.py, do not edit. Test data only: throwaway\n'
                ' * keys and certificates, an unsigned PE and Authenticode signatures of it. */\n\n')
        b64('TsRootCert', root, o)
        b64('TsPeImage', pe, o)
        for k, v in fx.items():
            b64('TsSig' + k, v, o)
    print('wrote', outpath, sum(len(v) for v in fx.values()), 'bytes of signatures')

if __name__ == '__main__':
    main(sys.argv[1])
