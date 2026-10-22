/*
 * PROJECT:     ReactOS kernel tests
 * LICENSE:     MIT
 * PURPOSE:     Native i386 NtContinue/NtRaiseException must preserve frame links
 */
#include <kmt_test.h>
#include <ndk/kefuncs.h>

#ifdef _M_IX86
static volatile LONG Handled;

static VOID SendCommand(ULONG Code)
{
    DWORD Error = KmtSendToDriver(Code);
    ok_eq_ulong(Error, ERROR_SUCCESS);
}

VOID __cdecl CaptureTrapContext(PCONTEXT Context);

static LONG CALLBACK
ExceptionHandler(PEXCEPTION_POINTERS Pointers)
{
    if (Pointers->ExceptionRecord->ExceptionCode != 0xe1234567)
        return EXCEPTION_CONTINUE_SEARCH;
    /* Observe NtRaiseException's unlink before the dispatcher calls NtContinue. */
    trace("Checking link in NtRaiseException handler\n");
    SendCommand(2);
    ++Handled;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static VOID
TestContextService(BOOLEAN Raise)
{
    CONTEXT Context;
    EXCEPTION_RECORD Record = {0};
    volatile LONG Resumed = 0;
    NTSTATUS Status;

    SendCommand(1);
    CaptureTrapContext(&Context);
    if (!Resumed)
    {
        Resumed = 1;
        Context.ContextFlags = CONTEXT_FULL;
        Context.Edx = Raise ? 0x2468ace0 : 0x13579bdf;
        if (Raise)
        {
            Record.ExceptionCode = 0xe1234567;
            Record.ExceptionAddress = (PVOID)Context.Eip;
            Status = NtRaiseException(&Record, &Context, TRUE);
        }
        else
            Status = NtContinue(&Context, FALSE);
        ok(FALSE, "Context service returned %lx\n", Status);
    }
    trace("Checking link after %s\n", Raise ? "NtRaiseException" : "NtContinue");
    SendCommand(2);
}
#endif

START_TEST(KeTrapFrame)
{
#ifdef _M_IX86
    DWORD Error;
    PVOID Handler;
    Error = KmtLoadAndOpenDriver(L"KeTrapFrame", FALSE);
    ok_eq_ulong(Error, ERROR_SUCCESS);
    if (Error) return;
    Handler = AddVectoredExceptionHandler(1, ExceptionHandler);
    ok(Handler != NULL, "Cannot register exception handler\n");
    if (Handler)
    {
        TestContextService(FALSE);
        TestContextService(TRUE);
        ok_eq_long(Handled, 1);
        RemoveVectoredExceptionHandler(Handler);
    }
    KmtCloseDriver();
    KmtUnloadDriver();
#else
    skip(FALSE, "This frame-link regression is specific to native i386\n");
#endif
}
