/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/except.c
 * PURPOSE:         Platform independent exception handling
 * PROGRAMMERS:     ReactOS Portable Systems Group
 *                  Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

#ifdef _M_AMD64
/* Captures the extended state of a user mode context with CONTEXT_XSTATE, to be loaded into the current thread with
   KiSetXStateContext. Raises the status of a context whose extended state description is invalid. Returns FALSE when
   there is nothing to load: no CONTEXT_XSTATE, or a system without extended state, which ignores the flag. */
static
BOOLEAN
KiCaptureContinueXState(
    _In_ PCONTEXT UserContext,
    _In_ ULONG ContextFlags,
    _Out_ PKI_XSTATE_CONTEXT XState)
{
    NTSTATUS Status;

    if ((ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE)
        return FALSE;

    Status = KiCaptureXStateContext(UserContext, UserMode, FALSE, XState);
    if (Status == STATUS_NOT_SUPPORTED)
        return FALSE;
    if (!NT_SUCCESS(Status))
        ExRaiseStatus(Status);
    return TRUE;
}
#endif

VOID
NTAPI
KiContinuePreviousModeUser(
    _In_ PCONTEXT Context,
    _Out_ PKEXCEPTION_FRAME ExceptionFrame,
    _Out_ PKTRAP_FRAME TrapFrame)
{
    CONTEXT LocalContext;
#ifdef _M_AMD64
    KI_XSTATE_CONTEXT XState;
    BOOLEAN HaveXState;
#endif

    /* We'll have to make a copy and probe it */
    ProbeForRead(Context, sizeof(CONTEXT), sizeof(ULONG));
    RtlCopyMemory(&LocalContext, Context, sizeof(CONTEXT));

#ifdef _M_AMD64
    /* The extended state the context carries (the one of an exception, for instance) is captured and checked before
       anything changes: an invalid one fails the call with the thread untouched */
    HaveXState = KiCaptureContinueXState(Context, LocalContext.ContextFlags, &XState);
#endif
    Context = &LocalContext;

    /* Convert the context into Exception/Trap Frames */
    KeContextToTrapFrame(&LocalContext,
                         ExceptionFrame,
                         TrapFrame,
                         LocalContext.ContextFlags,
                         UserMode);

#ifdef _M_AMD64
    if (HaveXState)
    {
        KiSetXStateContext(&XState);
        KiFreeXStateContext(&XState);
    }
#endif
}

NTSTATUS
NTAPI
KiContinue(IN PCONTEXT Context,
           IN PKEXCEPTION_FRAME ExceptionFrame,
           IN PKTRAP_FRAME TrapFrame)
{
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql = APC_LEVEL;
    KPROCESSOR_MODE PreviousMode = KeGetPreviousMode();

    /* Raise to APC_LEVEL, only if needed */
    if (KeGetCurrentIrql() < APC_LEVEL) KeRaiseIrql(APC_LEVEL, &OldIrql);

    /* Set up SEH to validate the context */
    _SEH2_TRY
    {
        /* Check the previous mode */
        if (PreviousMode != KernelMode)
        {
            /* Validate from user-mode */
            KiContinuePreviousModeUser(Context,
                                       ExceptionFrame,
                                       TrapFrame);
        }
        else
        {
            /* Convert the context into Exception/Trap Frames */
            KeContextToTrapFrame(Context,
                                 ExceptionFrame,
                                 TrapFrame,
                                 Context->ContextFlags,
                                 KernelMode);
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        /* Save the exception code */
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    /* Lower the IRQL if needed */
    if (OldIrql < APC_LEVEL) KeLowerIrql(OldIrql);

    /* Return status */
    return Status;
}

NTSTATUS
NTAPI
KiRaiseException(
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PCONTEXT Context,
    _Out_ PKEXCEPTION_FRAME ExceptionFrame,
    _Out_ PKTRAP_FRAME TrapFrame,
    _In_ BOOLEAN SearchFrames)
{
    KPROCESSOR_MODE PreviousMode = KeGetPreviousMode();
    CONTEXT LocalContext;
    EXCEPTION_RECORD LocalExceptionRecord;
    ULONG ParameterCount, Size;
#ifdef _M_AMD64
    PCONTEXT UserContext = Context;
    KI_XSTATE_CONTEXT XState;
#endif

    /* Check if we need to probe */
    if (PreviousMode != KernelMode)
    {
        /* Set up SEH */
        _SEH2_TRY
        {
            /* Probe the context */
            ProbeForRead(Context, sizeof(CONTEXT), sizeof(ULONG));

            /* Probe the Exception Record */
            ProbeForRead(ExceptionRecord,
                         FIELD_OFFSET(EXCEPTION_RECORD, NumberParameters) +
                         sizeof(ULONG),
                         sizeof(ULONG));

            /* Validate the maximum parameters */
            if ((ParameterCount = ExceptionRecord->NumberParameters) >
                EXCEPTION_MAXIMUM_PARAMETERS)
            {
                /* Too large */
                _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
            }

            /* Probe the entire parameters now*/
            Size = (sizeof(EXCEPTION_RECORD) -
                    ((EXCEPTION_MAXIMUM_PARAMETERS - ParameterCount) * sizeof(ULONG_PTR)));
            ProbeForRead(ExceptionRecord, Size, sizeof(ULONG));

            /* Now make copies in the stack. The unused parameters of the
               local record are dispatched too, so they must not hold stack
               contents. */
            RtlCopyMemory(&LocalContext, Context, sizeof(CONTEXT));
            RtlZeroMemory(&LocalExceptionRecord, sizeof(LocalExceptionRecord));
            RtlCopyMemory(&LocalExceptionRecord, ExceptionRecord, Size);
            Context = &LocalContext;
            ExceptionRecord = &LocalExceptionRecord;

            /* Update the parameter count */
            ExceptionRecord->NumberParameters = ParameterCount;

#ifdef _M_AMD64
            /* The exception is dispatched with the extended state of the context */
            if (KiCaptureContinueXState(UserContext, LocalContext.ContextFlags, &XState))
            {
                KiSetXStateContext(&XState);
                KiFreeXStateContext(&XState);
            }
#endif
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* Don't fail silently */
            DPRINT1("KiRaiseException: Failed to Probe\n");

            /* Return the exception code */
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    /* Convert the context record */
    KeContextToTrapFrame(Context,
                         ExceptionFrame,
                         TrapFrame,
                         Context->ContextFlags,
                         PreviousMode);

    /* Dispatch the exception */
    ExceptionRecord->ExceptionCode &= ~KI_EXCEPTION_INTERNAL;
    KiDispatchException(ExceptionRecord,
                        ExceptionFrame,
                        TrapFrame,
                        PreviousMode,
                        SearchFrames);

    /* We are done */
    return STATUS_SUCCESS;
}

/* SYSTEM CALLS ***************************************************************/

NTSTATUS
NTAPI
#ifdef _M_AMD64
KiRaiseExceptionService(
#else
NtRaiseException(
#endif
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PCONTEXT Context,
    _In_ BOOLEAN FirstChance
#ifdef _M_AMD64
    , _Inout_ PKEXCEPTION_FRAME ExceptionFrame
#endif
    )
{
    NTSTATUS Status;
    PKTHREAD Thread;
    PKTRAP_FRAME TrapFrame, LinkedTrapFrame;
#ifdef _M_IX86
    PKEXCEPTION_FRAME ExceptionFrame = NULL;
#elif !defined(_M_AMD64)
    KEXCEPTION_FRAME LocalExceptionFrame;
    PKEXCEPTION_FRAME ExceptionFrame = &LocalExceptionFrame;
#endif

    /* Get trap frame and link previous one */
    Thread = KeGetCurrentThread();
    TrapFrame = Thread->TrapFrame;
    /* i386 stores this link in Edx, which context restoration overwrites. */
    LinkedTrapFrame = KiGetLinkedTrapFrame(TrapFrame);

    /* Set exception list */
#ifdef _M_IX86
    __writefsdword(FIELD_OFFSET(KPCR, NtTib.ExceptionList),
                   (ULONG_PTR)TrapFrame->ExceptionList);
#endif

    /* Raise the exception */
    Status = KiRaiseException(ExceptionRecord,
                              Context,
                              ExceptionFrame,
                              TrapFrame,
                              FirstChance);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("KiRaiseException failed. Status = 0x%lx\n", Status);
        return Status;
    }

    Thread->TrapFrame = LinkedTrapFrame;

    /* It was handled, so exit restoring all state */
    KiExceptionExit(TrapFrame, ExceptionFrame);
}

NTSTATUS
NTAPI
#ifdef _M_AMD64
KiContinueService(
#else
NtContinue(
#endif
    _In_ PCONTEXT Context,
    _In_ BOOLEAN TestAlert
#ifdef _M_AMD64
    , _Inout_ PKEXCEPTION_FRAME ExceptionFrame
#endif
    )
{
    PKTHREAD Thread;
    NTSTATUS Status;
    PKTRAP_FRAME TrapFrame, LinkedTrapFrame;
#ifdef _M_IX86
    PKEXCEPTION_FRAME ExceptionFrame = NULL;
#elif !defined(_M_AMD64)
    KEXCEPTION_FRAME LocalExceptionFrame;
    PKEXCEPTION_FRAME ExceptionFrame = &LocalExceptionFrame;
#endif

    /* Get trap frame and link previous one*/
    Thread = KeGetCurrentThread();
    TrapFrame = Thread->TrapFrame;
    /* i386 stores this link in Edx, which context restoration overwrites. */
    LinkedTrapFrame = KiGetLinkedTrapFrame(TrapFrame);

    /* Continue from this point on */
    Status = KiContinue(Context, ExceptionFrame, TrapFrame);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("KiContinue failed. Status = 0x%lx\n", Status);
        return Status;
    }

    Thread->TrapFrame = LinkedTrapFrame;

    /* Check if alert was requested */
    if (TestAlert)
    {
        KeTestAlertThread(Thread->PreviousMode);
    }

    /* Exit to new context */
    KiExceptionExit(TrapFrame, ExceptionFrame);
}

/* EOF */
