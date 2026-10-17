"""Maintainer regression test; run on a build host with TMPDIR set."""
import pathlib,subprocess,tempfile,shutil,re,csv,sys,os
root=pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='thunk-test-',dir=os.environ['TMPDIR']) as td:
    copy=pathlib.Path(td)
    shutil.copytree(root/'dll/wow64win',copy/'dll/wow64win')
    for f in ['sdk/cmake/wow64handles.cmake','win32ss/w32ksvc32.h']:
        dst=copy/f;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/f,dst)
    gen=copy/'dll/wow64win/generate.py'
    subprocess.run([sys.executable,str(gen)],check=True)
    outputs=['generated.inc','failure.inc','coverage.tsv','messages-unsupported.inc']
    for name in outputs:assert (copy/'dll/wow64win'/name).read_bytes()==(root/'dll/wow64win'/name).read_bytes(),name
    generated=(copy/'dll/wow64win/generated.inc').read_text()
    rows={r['name']:r for r in csv.DictReader((copy/'dll/wow64win/coverage.tsv').open(),delimiter='\t')}
    assert rows['NtUserCallNextHookEx']['implementation']=='unsupported'
    assert rows['NtUserSetClipboardData']['implementation']=='manual'
    clip=next(l for l in generated.splitlines() if l.startswith('case W64_NtUserCreateLocalMemHandle:'))
    assert '(LONG_PTR)(LONG)a[0]' in clip
    assert 'case W64_NtUserBuildHwndList:' in (copy/'dll/wow64win/failure.inc').read_text()
    assert 'WM_COPYDATA' in (copy/'dll/wow64win/messages-unsupported.inc').read_text()
    blocked=(copy/'dll/wow64win/messages-unsupported.inc').read_text()
    assert all(int(v,16)<0x400 for v in re.findall(r'case 0x([0-9a-f]+):',blocked))
    # Changing the authoritative handle policy must change the typed call.
    policy=copy/'sdk/cmake/wow64handles.cmake';text=policy.read_text();assert 'HKL|LAYOUT|SIGN' in text
    policy.write_text(text.replace('HKL|LAYOUT|SIGN','HKL|LAYOUT|ZERO'))
    subprocess.run([sys.executable,str(gen)],check=True)
    changed=(copy/'dll/wow64win/generated.inc').read_text()
    before=next(l for l in generated.splitlines() if l.startswith('case W64_NtUserActivateKeyboardLayout:'))
    after=next(l for l in changed.splitlines() if l.startswith('case W64_NtUserActivateKeyboardLayout:'))
    assert '(LONG_PTR)(LONG)a[0]' in before and '(ULONG_PTR)a[0]' in after
    # A measured field that changes width removes pass-through eligibility.
    layout=copy/'dll/wow64win/catalog/layouts-real.tsv';text=layout.read_text()
    old='RECT\tleft\t0\t4\t0\t4\t';assert old in text
    layout.write_text(text.replace(old,'RECT\tleft\t0\t4\t0\t8\t'))
    subprocess.run([sys.executable,str(gen)],check=True)
    cov={r['name']:r for r in csv.DictReader((copy/'dll/wow64win/coverage.tsv').open(),delimiter='\t')}
    names=[n for n,r in cov.items() if 'measured RECT.left differs'==r['detail']]
    assert names, 'layout drift was not rejected'
    assert all(rows[n]['implementation']=='generated' for n in names)
    print('PASS deterministic generation; authoritative HKL policy; measured layout drift rejected; unsupported return and message metadata')
