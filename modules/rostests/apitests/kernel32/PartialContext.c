/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NtContinue and RtlRestoreContext with a context that names only some register groups
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * A context captured with RtlCaptureContext (or built by InitializeContext, for the extended state) gets known
 * values in registers of every group and a subset of the groups in its ContextFlags. NtContinue and
 * RtlRestoreContext restore the groups the flags name and no other:
 *  - without CONTEXT_CONTROL they return to their caller instead of jumping to the context's IP;
 *  - NtContinue leaves STATUS_SUCCESS in RAX when CONTEXT_INTEGER is absent (x64);
 *  - the nonvolatile registers, the XMM registers and MXCSR, and the direction flag are restored with their groups.
 * RtlRestoreContext with CONTEXT_INTEGER but not CONTEXT_CONTROL is not exercised: Windows 10 crashes there.
 * The expected results were measured on Windows 10 22H2 (x64 and WOW64).
 */

#include "precomp.h"

#define PC_XSTATE_BIT 0x40UL

typedef LONG (NTAPI *PFN_NtContinue)(PCONTEXT, BOOLEAN);
typedef VOID (NTAPI *PFN_RtlRestoreContext)(PCONTEXT, PEXCEPTION_RECORD);
typedef BOOL (WINAPI *PFN_InitializeContext)(PVOID, DWORD, PCONTEXT *, PDWORD);

static PFN_NtContinue pNtContinue;
static PFN_RtlRestoreContext pRtlRestoreContext;
static PFN_InitializeContext pInitializeContext;

#if defined(__x86_64__) || defined(__i386__)

#define PC_CONTROL  1
#define PC_INTEGER  2
#define PC_FLOAT    8

typedef struct _PC_CASE { const char *Name; DWORD Groups; BOOL XState; } PC_CASE;

#if defined(__x86_64__)

#define PC_ARCH     0x100000UL
#define PC_MARK_A   0xA0A0A0A0A0A0A0A0ULL
#define PC_MARK_B   0xB0B0B0B0B0B0B0B0ULL
#define PC_MARK_D   0xD0D0D0D0D0D0D0D0ULL
#define PC_MARK_12  0x1212121212121212ULL
#define PC_MARK_X6  0x6666666666666666ULL
#define PC_MARK_MXC 0x3f80      /* the default control word with the rounding control changed */

/* Record: 0 RAX, 1 RBX, 2 RDI, 3 R12, 4 low half of XMM6, 5 MXCSR, 6 RFLAGS, 7 1 if the context's RIP was reached,
 * 8 and 9 in: addresses of the context's RSP and RIP, which the stub sets to its own frame and landing label */
extern void PartialContextRun(void *Function, PCONTEXT Context, ULONG_PTR Arg2, ULONG64 *Record);
__asm__(
".intel_syntax noprefix\n"
".globl PartialContextRun\n"
"PartialContextRun:\n"
" push rbx\n push rbp\n push rdi\n push rsi\n push r12\n push r13\n push r14\n push r15\n"
" sub rsp,0x60\n"
" movups [rsp+0x40],xmm6\n"
" stmxcsr [rsp+0x50]\n"
" mov [rsp+0x30],r9\n"
" mov qword ptr [rsp+0x38],0\n"
" mov r10,[r9+0x40]\n mov [r10],rsp\n"
" mov r10,[r9+0x48]\n lea r11,[rip+PartialContextLand]\n mov [r10],r11\n"
" mov r10,rcx\n mov rcx,rdx\n mov rdx,r8\n"
" call r10\n"
" jmp PartialContextRecord\n"
"PartialContextLand:\n"
" mov qword ptr [rsp+0x38],1\n"
"PartialContextRecord:\n"
" mov r10,[rsp+0x30]\n"
" mov [r10],rax\n mov [r10+8],rbx\n mov [r10+16],rdi\n mov [r10+24],r12\n"
" movq [r10+32],xmm6\n"
" mov qword ptr [r10+40],0\n stmxcsr [r10+40]\n"
" pushfq\n pop r11\n mov [r10+48],r11\n"
" mov r11,[rsp+0x38]\n mov [r10+56],r11\n"
" cld\n"
" ldmxcsr [rsp+0x50]\n movups xmm6,[rsp+0x40]\n"
" add rsp,0x60\n"
" pop r15\n pop r14\n pop r13\n pop r12\n pop rsi\n pop rdi\n pop rbp\n pop rbx\n"
" ret\n"
".att_syntax\n");

static void PartialContextFill(PCONTEXT Context, ULONG64 *Record)
{
    Context->Rax = PC_MARK_A; Context->Rbx = PC_MARK_B; Context->Rdi = PC_MARK_D; Context->R12 = PC_MARK_12;
    Context->Xmm6.Low = PC_MARK_X6;
    Context->MxCsr = PC_MARK_MXC;
    Context->FltSave.MxCsr = PC_MARK_MXC;
    Context->EFlags |= 0x400;
    Record[8] = (ULONG64)(ULONG_PTR)&Context->Rsp;
    Record[9] = (ULONG64)(ULONG_PTR)&Context->Rip;
}

static void PartialContextCheck(const PC_CASE *Case, BOOL Rtl, const ULONG64 *Record)
{
    DWORD Groups = Case->Groups;
    const char *Api = Rtl ? "RtlRestoreContext" : "NtContinue";

    ok(Record[7] == ((Groups & PC_CONTROL) ? 1 : 0), "%s %s: %s the context's RIP\n", Api, Case->Name,
       Record[7] ? "reached" : "did not reach");
    /* RtlRestoreContext returns nothing: RAX is only meaningful when it jumps */
    if (!Rtl || (Groups & PC_CONTROL))
        ok(Record[0] == ((Groups & PC_INTEGER) ? PC_MARK_A : 0), "%s %s: RAX is %I64x\n", Api, Case->Name, Record[0]);
    ok((Record[1] == PC_MARK_B) == !!(Groups & PC_INTEGER), "%s %s: RBX is %I64x\n", Api, Case->Name, Record[1]);
    ok((Record[2] == PC_MARK_D) == !!(Groups & PC_INTEGER), "%s %s: RDI is %I64x\n", Api, Case->Name, Record[2]);
    ok((Record[3] == PC_MARK_12) == !!(Groups & PC_INTEGER), "%s %s: R12 is %I64x\n", Api, Case->Name, Record[3]);
    ok((Record[4] == PC_MARK_X6) == !!(Groups & PC_FLOAT), "%s %s: XMM6 is %I64x\n", Api, Case->Name, Record[4]);
    ok(((Record[5] & 0xffff) == PC_MARK_MXC) == !!(Groups & PC_FLOAT), "%s %s: MXCSR is %I64x\n", Api, Case->Name,
       Record[5]);
    ok(!!(Record[6] & 0x400) == !!(Groups & PC_CONTROL), "%s %s: RFLAGS is %I64x\n", Api, Case->Name, Record[6]);
}

#else /* __i386__ */

#define PC_ARCH     0x10000UL
#define PC_MARK_A   0xA0A0A0A0UL
#define PC_MARK_B   0xB0B0B0B0UL
#define PC_MARK_D   0xD0D0D0D0UL
#define PC_MARK_X6  0x66666666UL
#define PC_MARK_MXC 0x3f80
#define PC_MARK_FCW 0x17f
#define PC_EXTENDED 32

/* Record: 0 EAX, 1 EBX, 2 EDI, 3 x87 control word, 4 MXCSR, 5 EFLAGS, 6 1 if the context's EIP was reached,
 * 7 low half of XMM6, 8 and 9 in: addresses of the context's ESP and EIP, which the stub sets to its own frame and
 * landing label */
extern void __cdecl PartialContextRun(void *Function, PCONTEXT Context, ULONG Arg2, ULONG *Record);
__asm__(
".intel_syntax noprefix\n"
".globl _PartialContextRun\n"
"_PartialContextRun:\n"
" push ebx\n push ebp\n push esi\n push edi\n"
" sub esp,0x40\n"
" mov eax,[esp+0x60]\n"
" mov [esp+0x10],eax\n"
" mov dword ptr [esp+0x14],0\n"
" stmxcsr [esp+0x18]\n"
" fnstcw [esp+0x1c]\n"
" movups [esp+0x20],xmm6\n"
" mov ecx,[eax+0x20]\n mov [ecx],esp\n"
" mov ecx,[eax+0x24]\n lea edx,PartialContextLand\n mov [ecx],edx\n"
" push dword ptr [esp+0x5c]\n"
" push dword ptr [esp+0x5c]\n"
" call dword ptr [esp+0x5c]\n"
" jmp PartialContextRecord\n"
"PartialContextLand:\n"
" mov dword ptr [esp+0x14],1\n"
"PartialContextRecord:\n"
" mov ecx,[esp+0x10]\n"
" mov [ecx],eax\n mov [ecx+4],ebx\n mov [ecx+8],edi\n"
" fnstcw [ecx+12]\n"
" stmxcsr [ecx+16]\n"
" pushfd\n pop edx\n mov [ecx+20],edx\n"
" mov edx,[esp+0x14]\n mov [ecx+24],edx\n"
" movd [ecx+28],xmm6\n"
" cld\n"
" ldmxcsr [esp+0x18]\n fldcw [esp+0x1c]\n movups xmm6,[esp+0x20]\n"
" add esp,0x40\n"
" pop edi\n pop esi\n pop ebp\n pop ebx\n"
" ret\n"
".att_syntax\n");

static void PartialContextFill(PCONTEXT Context, ULONG *Record)
{
    static BYTE Fx[512] __attribute__((aligned(16)));

    /* A well-formed x87 image (what FNSAVE stores is the layout of FLOATING_SAVE_AREA) and a FXSAVE one */
    __asm__ __volatile__("fnsave %0\n fwait\n frstor %0" : "=m"(Context->FloatSave) : : "memory");
    __asm__ __volatile__("fxsave %0" : "=m"(Fx) : : "memory");
    memcpy(Context->ExtendedRegisters, Fx, sizeof(Fx));
    Context->Eax = PC_MARK_A; Context->Ebx = PC_MARK_B; Context->Edi = PC_MARK_D;
    Context->EFlags |= 0x400;
    Context->FloatSave.ControlWord = PC_MARK_FCW;
    *(ULONG *)&Context->ExtendedRegisters[24] = PC_MARK_MXC;
    *(ULONG *)&Context->ExtendedRegisters[160 + 96] = PC_MARK_X6;
    Record[8] = (ULONG)(ULONG_PTR)&Context->Esp;
    Record[9] = (ULONG)(ULONG_PTR)&Context->Eip;
}

static void PartialContextCheck(const PC_CASE *Case, BOOL Rtl, const ULONG *Record)
{
    DWORD Groups = Case->Groups;
    const char *Api = Rtl ? "RtlRestoreContext" : "NtContinue";

    ok(Record[6] == ((Groups & PC_CONTROL) ? 1 : 0), "%s %s: %s the context's EIP\n", Api, Case->Name,
       Record[6] ? "reached" : "did not reach");
    ok((Record[1] == PC_MARK_B) == !!(Groups & PC_INTEGER), "%s %s: EBX is %lx\n", Api, Case->Name, Record[1]);
    ok((Record[2] == PC_MARK_D) == !!(Groups & PC_INTEGER), "%s %s: EDI is %lx\n", Api, Case->Name, Record[2]);
    ok(((Record[3] & 0xffff) == PC_MARK_FCW) == !!(Groups & PC_FLOAT), "%s %s: x87 control word is %lx\n", Api,
       Case->Name, Record[3]);
    ok(((Record[4] & 0xffff) == PC_MARK_MXC) == !!(Groups & PC_EXTENDED), "%s %s: MXCSR is %lx\n", Api, Case->Name,
       Record[4]);
    ok((Record[7] == PC_MARK_X6) == !!(Groups & PC_EXTENDED), "%s %s: XMM6 is %lx\n", Api, Case->Name, Record[7]);
}

#endif

static const PC_CASE Cases[] =
{
    { "CONTROL", PC_CONTROL },
    { "INTEGER", PC_INTEGER },
    { "SEGMENTS", 4 },
    { "FLOATING_POINT", PC_FLOAT },
    { "DEBUG_REGISTERS", 0x10 },
#ifdef __i386__
    { "EXTENDED_REGISTERS", 0x20 },
    { "CONTROL|EXTENDED_REGISTERS", PC_CONTROL | 0x20 },
    { "INTEGER|EXTENDED_REGISTERS", PC_INTEGER | 0x20 },
#else
    { "XSTATE", 0, TRUE },
    { "XSTATE|CONTROL", PC_CONTROL, TRUE },
    { "XSTATE|FLOATING_POINT", PC_FLOAT, TRUE },
#endif
    { "CONTROL|INTEGER", PC_CONTROL | PC_INTEGER },
    { "CONTROL|SEGMENTS", PC_CONTROL | 4 },
    { "CONTROL|FLOATING_POINT", PC_CONTROL | PC_FLOAT },
    { "INTEGER|FLOATING_POINT", PC_INTEGER | PC_FLOAT },
    { "INTEGER|SEGMENTS", PC_INTEGER | 4 },
#ifdef __i386__
    /* No ALL: with both the floating point and the extended registers WOW64 takes the x87 state from the
       latter on ReactOS and from the former on Windows, whole contexts included, which this test is not about */
    { "FULL", 7 },
#else
    { "FULL", 0xb },
    { "ALL", 0x1f },
#endif
};

static void RunCase(const PC_CASE *Case, BOOL Rtl)
{
    static BYTE Raw[8192] __attribute__((aligned(64)));
    static CONTEXT Plain __attribute__((aligned(16)));
#ifdef __x86_64__
    ULONG64 Record[10];
#else
    ULONG Record[12];
#endif
    PCONTEXT Context = &Plain;
    DWORD Length = sizeof(Raw), Flags = PC_ARCH | Case->Groups;

    memset(Record, 0, sizeof(Record));
    if (Case->XState)
    {
        Context = NULL;
        if (!pInitializeContext(Raw, PC_ARCH | 0x7 | PC_XSTATE_BIT, &Context, &Length))
        {
            skip("InitializeContext failed for %s\n", Case->Name);
            return;
        }
        Flags |= PC_XSTATE_BIT;
    }
    RtlCaptureContext(Context);
    Context->ContextFlags = Flags;
    PartialContextFill(Context, Record);
    PartialContextRun(Rtl ? (void *)pRtlRestoreContext : (void *)pNtContinue, Context, 0, Record);
    PartialContextCheck(Case, Rtl, Record);
}

static BOOL XStateAvailable(void)
{
    PCONTEXT Context = NULL;
    DWORD Length = 0;

    if (!pInitializeContext)
        return FALSE;
    /* A system without extended state fails the sizing call with another error */
    SetLastError(0);
    pInitializeContext(NULL, PC_ARCH | 0x7 | PC_XSTATE_BIT, &Context, &Length);
    return GetLastError() == ERROR_INSUFFICIENT_BUFFER;
}

#endif /* x86 */

START_TEST(PartialContext)
{
#if defined(__x86_64__) || defined(__i386__)
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    BOOL XState;
    ULONG i;

    pNtContinue = (PFN_NtContinue)GetProcAddress(hNtdll, "NtContinue");
    pRtlRestoreContext = (PFN_RtlRestoreContext)GetProcAddress(hNtdll, "RtlRestoreContext");
    pInitializeContext = (PFN_InitializeContext)GetProcAddress(GetModuleHandleA("kernel32.dll"), "InitializeContext");
    if (!pNtContinue)
    {
        skip("NtContinue is not exported\n");
        return;
    }
    XState = XStateAvailable();

    for (i = 0; i < sizeof(Cases) / sizeof(Cases[0]); i++)
    {
        if (Cases[i].XState && !XState)
        {
            skip("No extended state: %s\n", Cases[i].Name);
            continue;
        }
        RunCase(&Cases[i], FALSE);
#ifdef __x86_64__
        /* Windows 10 crashes on INTEGER without CONTROL */
        if (pRtlRestoreContext && !((Cases[i].Groups & PC_INTEGER) && !(Cases[i].Groups & PC_CONTROL)))
            RunCase(&Cases[i], TRUE);
#endif
    }
#else
    skip("Only x86 and x64 are covered\n");
#endif
}
