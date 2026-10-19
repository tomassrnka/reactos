/*
 * PROJECT:     ReactOS kernel tests
 * LICENSE:     MIT
 * PURPOSE:     Ring-3 compatibility-mode SYSCALL round trip, without a WoW64 loader
 */

#include <kmt_test.h>

#ifdef _M_AMD64
extern const UCHAR CstarCode[], CstarCodeEnd[], CstarCode64[], CstarAfterSyscall[];
VOID CstarInvoke(PVOID Code, PVOID State, PVOID Stack);

typedef struct
{
    ULONG64 NativeRsp, NativeReturn;
    ULONG Number, UserEsp, Status, ReturnedEsp, ReturnedEcx, Eflags;
    ULONG Ebx, Ebp, Esi, Edi;
    ULONG ReturnOffset;
    USHORT ReturnSelector, Padding;
    ULONG SafeEsp;
} CSTAR_STATE;
C_ASSERT(FIELD_OFFSET(CSTAR_STATE, SafeEsp) == 64);
#endif

START_TEST(KeSystemCall32)
{
#ifdef _M_AMD64
    int Cpu[4];
    PUCHAR Buffer = NULL;
    CSTAR_STATE *State;
    ULONG_PTR Address;
    ULONG i, j;
    ULONG Stacks[4];
    static const ULONG Numbers[] = {0, 0x1000, 0xffffffff};

    KmtRunKernelTest("KeSystemCall32");
    __cpuid(Cpu, 0);
    if (Cpu[1] != 0x68747541 || Cpu[3] != 0x69746e65 || Cpu[2] != 0x444d4163)
    {
        skip(FALSE, "Compatibility-mode SYSCALL is an AMD extension; requires AuthenticAMD\n");
        return;
    }

    /* Do not depend on WoW64 allocation support in the kernel under test. */
    for (Address = 0x10000000; Address < 0x20000000 && !Buffer; Address += 0x10000)
        Buffer = VirtualAlloc((PVOID)Address, 0x2000, MEM_RESERVE | MEM_COMMIT,
                              PAGE_EXECUTE_READWRITE);
    ok(Buffer != NULL, "Cannot allocate low code and stack\n");
    if (!Buffer) return;
    RtlCopyMemory(Buffer, CstarCode, CstarCodeEnd - CstarCode);
    FlushInstructionCache(GetCurrentProcess(), Buffer, CstarCodeEnd - CstarCode);
    State = (CSTAR_STATE *)(Buffer + 0x800);
    Stacks[0] = (ULONG)(ULONG_PTR)(Buffer + 0x1f00);
    Stacks[1] = 0;
    Stacks[2] = 1;
    Stacks[3] = 0xfffffff0;

    for (i = 0; i < RTL_NUMBER_OF(Numbers); ++i)
    for (j = 0; j < RTL_NUMBER_OF(Stacks); ++j)
    {
        RtlZeroMemory(State, sizeof(*State));
        State->Number = Numbers[i];
        State->UserEsp = Stacks[j];
        State->SafeEsp = Stacks[0];
        State->ReturnOffset = (ULONG)(ULONG_PTR)(Buffer + (CstarCode64 - CstarCode));
        State->ReturnSelector = 0x33;
        trace("CSTAR number=%08lx esp=%08lx\n", State->Number, State->UserEsp);
        CstarInvoke(Buffer, State, (PVOID)(ULONG_PTR)Stacks[0]);
        ok_eq_hex(State->Status, STATUS_INVALID_SYSTEM_SERVICE);
        ok_eq_hex(State->ReturnedEsp, Stacks[j]);
        ok_eq_hex(State->ReturnedEcx, (ULONG)(ULONG_PTR)(Buffer + (CstarAfterSyscall - CstarCode)));
        ok_eq_hex(State->Eflags & 0x601, 0x601); /* CF, IF and DF survive. */
        ok_eq_hex(State->Ebx, 0x12345678);
        ok_eq_hex(State->Ebp, 0x23456789);
        ok_eq_hex(State->Esi, 0x3456789a);
        ok_eq_hex(State->Edi, (ULONG)(ULONG_PTR)State);
    }
    ok(GetCurrentThreadId() != 0, "Native TEB unavailable after compatibility return\n");
    VirtualFree(Buffer, 0, MEM_RELEASE);
#else
    skip(FALSE, "Compatibility-mode syscall test requires amd64\n");
#endif
}
