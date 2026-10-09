/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel mode tests for Save/Restore FPU state API kernel support
 * COPYRIGHT:   Copyright 2022 George Bișoc <george.bisoc@reactos.org>
 */

#include <kmt_test.h>

#ifdef _M_IX86

#define NPX_RUNDOWN_THREADS 64

static
VOID
NTAPI
NpxRundownThread(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    /* Use the NPX, so that this thread's state gets loaded */
#ifdef _MSC_VER
    __asm fld1
    __asm fstp st(0)
#else
    __asm__ __volatile__("fld1\n\tfstp %%st(0)" : : : "memory");
#endif

    /* Set CR0.TS under the loaded state, as the task switches of an NMI do,
       then exit: the kernel must discard the state without a bugcheck */
    __writecr0(__readcr0() | 0x8);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
TestNpxRundown(VOID)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle;
    ULONG i;

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    for (i = 0; i < NPX_RUNDOWN_THREADS; i++)
    {
        Status = PsCreateSystemThread(&Handle, SYNCHRONIZE, &ObjectAttributes, NULL, NULL, NpxRundownThread, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            break;

        Status = ZwWaitForSingleObject(Handle, FALSE, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
    }
}

#endif

START_TEST(KeFloatPointState)
{
    NTSTATUS Status;
    KFLOATING_SAVE FloatSave;
    KIRQL Irql;

    /* Save the state under normal conditions */
    Status = KeSaveFloatingPointState(&FloatSave);
    ok_irql(PASSIVE_LEVEL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Restore the FPU state back */
    KeRestoreFloatingPointState(&FloatSave);

    /* Try to raise the IRQL to APC and do some operations again */
    KeRaiseIrql(APC_LEVEL, &Irql);

    /* Save the state under APC_LEVEL interrupt */
    Status = KeSaveFloatingPointState(&FloatSave);
    ok_irql(APC_LEVEL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Restore the FPU state back */
    KeRestoreFloatingPointState(&FloatSave);

    /* Try to raise the IRQL to dispatch this time */
    KeLowerIrql(Irql);
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    /* Save the state under DISPATCH_LEVEL interrupt */
    Status = KeSaveFloatingPointState(&FloatSave);
    ok_irql(DISPATCH_LEVEL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* We're done */
    KeRestoreFloatingPointState(&FloatSave);
    KeLowerIrql(Irql);

#ifdef _M_IX86
    if (!skip(is_reactos(), "The NPX rundown test writes CR0 and runs on ReactOS only\n"))
        TestNpxRundown();
#endif
}
