#!/usr/bin/env python3
"""Deterministic schedules for Wow64ReviewThreads (ReactOS source only).

Run --instrument ROOT on a disposable source/build tree. Build ntkrnlmp, wow64,
wow64cpu and kernel32_wow64_apitest on the permitted build host. Run the native
controller with the PE32 fixture and exit/startup/transition modes. Restore the
sources and rebuild before running ordinary suites or publishing artifacts.
The probes apply unchanged to the reviewed head and the fixed head. They add
observations/barriers, not alternate stack-release or context implementations.
"""
import argparse
from collections import Counter
import re
from pathlib import Path

EVENT = r'''
static HANDLE ReviewEvent(PCWSTR Name)
{
    UNICODE_STRING String;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Handle = NULL;
    RtlInitUnicodeString(&String, Name);
    InitializeObjectAttributes(&Attributes, &String, OBJ_CASE_INSENSITIVE, NULL, NULL);
    NtOpenEvent(&Handle, (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 3), &Attributes);
    return Handle;
}
'''
STARTUP = r'''
static VOID ReviewStartup(VOID)
{
    static LONG Entered;
    HANDLE Ready, Release;
    if (InterlockedCompareExchange(&Entered, 1, 0)) return;
    Ready = ReviewEvent(L"\\BaseNamedObjects\\wow64-r1-init-entered");
    Release = ReviewEvent(L"\\BaseNamedObjects\\wow64-r1-init-release");
    if (Ready && Release)
    {
        NtSetEvent(Ready, NULL);
        NtWaitForSingleObject(Release, FALSE, NULL);
    }
    if (Ready) NtClose(Ready);
    if (Release) NtClose(Release);
}
'''
TRANSITION = r'''
static HANDLE ReviewArrived, ReviewProceed;
static ULONG ReviewCount;
static LONG NTAPI ReviewTransition(PEXCEPTION_POINTERS Exception)
{
    PVOID Address;
    SIZE_T Size;
    ULONG Old;
    if (Exception->ExceptionRecord->ExceptionCode != STATUS_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    Address = (PVOID)Exception->ContextRecord->Rip;
    if (ReviewArrived && ReviewProceed && ReviewCount < 160 &&
        NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU])
    {
        DbgPrint("WOW64-R1-BOUNDARY %lu %p\n", ++ReviewCount, Address);
        NtSetEvent(ReviewArrived, NULL);
        NtWaitForSingleObject(ReviewProceed, FALSE, NULL);
    }
    else
    {
        /* Remove each probe after the bounded schedule, or if disabled. */
        Size = 1;
        if (NT_SUCCESS(NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size,
                                              PAGE_EXECUTE_READWRITE, &Old)))
        {
            *(UCHAR *)(ULONG_PTR)Exception->ContextRecord->Rip = 0x90;
            NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, Old, &Old);
        }
    }
    Exception->ContextRecord->Rip++;
    return EXCEPTION_CONTINUE_EXECUTION;
}
'''

APC_TRANSITION = r'''
static VOID ReviewApcBoundary(VOID)
{
    HANDLE Arrived = ReviewEvent(L"\\BaseNamedObjects\\wow64-r1-apc-transition");
    HANDLE Proceed = ReviewEvent(L"\\BaseNamedObjects\\wow64-r1-apc-continue");
    if (Arrived && Proceed)
    {
        NtSetEvent(Arrived, NULL);
        NtWaitForSingleObject(Proceed, FALSE, NULL);
    }
    if (Arrived) NtClose(Arrived);
    if (Proceed) NtClose(Proceed);
}
'''

def instrument_apc(root):
    p = root / 'dll/wow64/exception.c'
    s = p.read_text()
    assert 'ReviewApcBoundary' not in s
    s = s.replace('#include "wow64p.h"', '#include "wow64p.h"\n' + EVENT + APC_TRANSITION)
    s = s.replace('    Guest->Context = Context;', '    Guest->Context = Context;\n    ReviewApcBoundary();')
    s = s.replace('        Wow64ContextToCpu(Apc->Previous, Context);',
                  '        Wow64ContextToCpu(Apc->Previous, Context);\n        ReviewApcBoundary();')
    p.write_text(s)

def instrument(root):
    p = root / 'dll/wow64/init.c'
    s = p.read_text()
    assert 'ReviewStartup' not in s
    s = s.replace('#include "wow64p.h"', '#include "wow64p.h"\n' + EVENT + STARTUP)
    s = s.replace('    Status = Wow64InitializeRedirection();', '    ReviewStartup();\n    Status = Wow64InitializeRedirection();')
    p.write_text(s)
    p = root / 'ntoskrnl/ps/kill.c'
    s = p.read_text()
    start, end = s.index('    /* Check if we have a TEB */', s.index('PspExitThread(')), s.index('    /* Free LPC Data */', s.index('PspExitThread('))
    body = s[start:end]
    body = re.sub(r'ZwFreeVirtualMemory\(NtCurrentProcess\(\),\s*&(\w+),', r'ReviewFreeStack(\n                                    &\1,', body)
    wrapper = '''
#if defined(_WIN64) && defined(BUILD_WOW64_ENABLED)
static NTSTATUS ReviewFreeStack(PVOID *Base, PSIZE_T Size, ULONG Type)
{
    PTEB Teb = PsGetCurrentThread()->Tcb.Teb;
    if (PsGetCurrentProcess()->Wow64Process && Teb && Teb->FreeStackOnTermination)
        DbgPrint("WOW64-R1-RELEASE %p %p %p\\n", PsGetCurrentProcessId(), PsGetCurrentThreadId(), *Base);
    return ZwFreeVirtualMemory(NtCurrentProcess(), Base, Size, Type);
}
#else
#define ReviewFreeStack(Base, Size, Type) ZwFreeVirtualMemory(NtCurrentProcess(), Base, Size, Type)
#endif
'''
    s = s[:start] + body + s[end:]
    s = s.replace('#include <debug.h>', '#include <debug.h>\n' + wrapper)
    p.write_text(s)
    p = root / 'dll/wow64cpu/cpu.c'
    s = p.read_text().replace('#include <reactos/wow64.h>', '#include <reactos/wow64.h>\n' + EVENT + TRANSITION)
    s = s.replace('    /* A compatibility far jump', '''    ReviewArrived = ReviewEvent(L"\\\\BaseNamedObjects\\\\wow64-r1-transition");
    ReviewProceed = ReviewEvent(L"\\\\BaseNamedObjects\\\\wow64-r1-continue");
    RtlAddVectoredExceptionHandler(1, ReviewTransition);
    /* A compatibility far jump''')
    # Break on both sides of the low trampoline's MOV before dispatch entry.
    s = s.replace('Code + 32', 'Code + 64').replace('Code + 36', 'Code + 68')
    s = s.replace('Code[16] = 0x49; Code[17] = 0xbb;', 'Code[16] = 0xcc; Code[17] = 0x49; Code[18] = 0xbb;')
    s = s.replace('Code + 18', 'Code + 19')
    s = s.replace('Code[26] = 0x41; Code[27] = 0xff; Code[28] = 0xe3;', 'Code[27] = 0xcc; Code[28] = 0x41; Code[29] = 0xff; Code[30] = 0xe3;')
    p.write_text(s)
    p = root / 'dll/wow64cpu/cpu.S'
    s = p.read_text()
    before, body = s.split('    .endprolog', 1)
    lines = body.splitlines(True)
    body = ''.join(('    int 3 /* ReviewTransition: instruction boundary */\n' if re.match(r'    (mov|fxsave|fxrstor|pushfq|pop|cld|add|call|iretq)\b', line) else '') + line for line in lines)
    p.write_text(before + '    .endprolog' + body)


def releases(serial, test):
    expected = Counter((int(p, 16), int(t, 16)) for p, t in re.findall(r'R1 native exit pid=([\da-f]+) tid=([\da-f]+)', test.read_text()))
    assert sum(expected.values()) == 128, f'native exits observed: {sum(expected.values())}/128'
    calls = {key: [] for key in expected}
    for p, t, b in re.findall(r'WOW64-R1-RELEASE ([\da-fA-F]+) ([\da-fA-F]+) ([\da-fA-F]+)', serial.read_text(errors='replace')):
        key = int(p, 16), int(t, 16)
        if key in calls: calls[key].append(int(b, 16))
    bad = {key: value for key, value in calls.items() if len(value) != expected[key]}
    print(f'native stack release observations: exits={sum(expected.values())}, releases={sum(map(len, calls.values()))}, invalid_thread_ids={len(bad)}')
    assert not bad, repr(bad)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--instrument', type=Path)
    parser.add_argument('--apc-instrument', type=Path)
    parser.add_argument('--releases', nargs=2, type=Path, metavar=('SERIAL', 'TEST_OUTPUT'))
    args = parser.parse_args()
    if args.instrument: instrument(args.instrument)
    elif args.apc_instrument: instrument_apc(args.apc_instrument)
    elif args.releases: releases(*args.releases)
    else: parser.error('select --instrument or --releases')
