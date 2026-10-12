/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Test for SetUnhandledExceptionFilter
 * PROGRAMMER:      Mike "tamlin" Nordell
 */

#include "precomp.h"

#include <xmmintrin.h>

/*
 * Keep these returning different values, to prevent compiler folding
 * them into a single function, thereby voiding the test
 */
LONG WINAPI Filter1(LPEXCEPTION_POINTERS p) { return 0; }
LONG WINAPI Filter2(LPEXCEPTION_POINTERS p) { return 1; }


/*
 * Verify that SetUnhandledExceptionFilter actually returns the
 * _previous_ handler.
 */
static
VOID
TestSetUnhandledExceptionFilter(VOID)
{
	LPTOP_LEVEL_EXCEPTION_FILTER p1, p2;
	p1 = SetUnhandledExceptionFilter(Filter1);
	p2 = SetUnhandledExceptionFilter(Filter2);
	ok(p1 != Filter1, "SetUnhandledExceptionFilter returned what was set, not prev\n");
	ok(p2 != Filter2, "SetUnhandledExceptionFilter returned what was set, not prev\n");
	ok(p2 == Filter1, "SetUnhandledExceptionFilter didn't return previous filter\n");
	ok(p1 != p2, "SetUnhandledExceptionFilter seems to return random stuff\n");

    p1 = SetUnhandledExceptionFilter(NULL);
    ok(p1 == Filter2, "SetUnhandledExceptionFilter didn't return previous filter\n");
}

static LONG WINAPI ExceptionFilterSSESupport(LPEXCEPTION_POINTERS exp)
{
    PEXCEPTION_RECORD rec = exp->ExceptionRecord;
    PCONTEXT ctx = exp->ContextRecord;

    trace("Exception raised while using SSE instructions.\n");

    ok(rec->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION, "Exception code is 0x%08x.\n", (unsigned int)rec->ExceptionCode);

    if(rec->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION)
    {
        trace("Unexpected exception code, terminating!\n");
        return EXCEPTION_EXECUTE_HANDLER;
    }

    ok((ctx->ContextFlags & CONTEXT_CONTROL) == CONTEXT_CONTROL, "Context does not contain control register.\n");

#ifdef _M_IX86
    ctx->Eip += 3;
#elif defined(_M_AMD64)
    ctx->Rip += 3;
#else
#error Architecture not handled
#endif

    return EXCEPTION_CONTINUE_EXECUTION;
}

static BOOL ExceptionCaught = FALSE;

static LONG WINAPI ExceptionFilterSSEException(LPEXCEPTION_POINTERS exp)
{
    PEXCEPTION_RECORD rec = exp->ExceptionRecord;
    PCONTEXT ctx = exp->ContextRecord;
#ifdef _M_AMD64
    ULONG ExpectedExceptionCode = STATUS_FLOAT_DIVIDE_BY_ZERO;
#else
    ULONG ExpectedExceptionCode = STATUS_FLOAT_MULTIPLE_TRAPS;
#endif

    trace("Exception raised while dividing by 0.\n");

    ok(rec->ExceptionCode == ExpectedExceptionCode, "Exception code is 0x%08x.\n", (unsigned int)rec->ExceptionCode);

    if(rec->ExceptionCode != ExpectedExceptionCode)
    {
        trace("Unexpected exception code, terminating!\n");
        return EXCEPTION_EXECUTE_HANDLER;
    }

    ok((ctx->ContextFlags & CONTEXT_CONTROL) == CONTEXT_CONTROL, "Context does not contain control register.\n");

    ExceptionCaught = TRUE;

#ifdef _M_IX86
    ctx->Eip += 3;
#elif defined(_M_AMD64)
    ctx->Rip += 3;
#else
#error Architecture not handled
#endif

    return EXCEPTION_CONTINUE_EXECUTION;
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((__target__("sse")))
#endif
static
VOID TestSSEExceptions(VOID)
{
    LPTOP_LEVEL_EXCEPTION_FILTER p;
    unsigned int csr;

    /* Test SSE support for the CPU */
    p = SetUnhandledExceptionFilter(ExceptionFilterSSESupport);
    ok(p == NULL, "Previous filter should be NULL\n");

#if !defined(_M_AMD64)
    {
        BOOL supportsSSE = FALSE;
#ifdef _MSC_VER
            __asm
            {
                xorps xmm0, xmm0
                mov supportsSSE, 0x1
            }
#else
            __asm__(
                "xorps %%xmm0, %%xmm0\n"
                "movl $1, %0\n"
                : "=r"(supportsSSE)
            );
#endif /* _MSC_VER */

        if(!supportsSSE)
        {
            skip("CPU doesn't support SSE instructions.\n");
            SetUnhandledExceptionFilter(NULL);
            return;
        }
    }
#endif /* !defined(_M_AMD64) */

    /* Deliberately throw a divide by 0 exception */
    p = SetUnhandledExceptionFilter(ExceptionFilterSSEException);
    ok(p == ExceptionFilterSSESupport, "Unexpected old filter : 0x%p", p);

    /* Unmask divide by 0 exception */
    csr = _mm_getcsr();
    _mm_setcsr(csr & 0xFFFFFDFF);

    /* We can't use _mm_div_ps, as it masks the exception before performing anything*/
#if defined(_MSC_VER)
#if defined(_M_AMD64)
    {
        __m128 xmm1 = { { 1., 1. } }, xmm2 = { { 0 } };
        /* Wait, aren't exceptions masked? Yes, but actually no. */
        xmm1 = _mm_div_ps(xmm1, xmm2);
    }
#else
    __asm
    {
        xorps xmm0, xmm0
        push 0x3f800000
        push 0x3f800000
        push 0x3f800000
        push 0x3f800000

        movups xmm1, [esp]

        /* Divide by 0 */
        divps xmm1, xmm0

        /* Clean up */
        add esp, 16
    }
#endif
#else
    ULONG zeros[4] = {0, 0, 0, 0};
    ULONG ones[4] = {0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000};
    __asm__ (
        "movups (%0), %%xmm0\n"
        "movups (%1), %%xmm1\n"
        /* Divide by 0 */
        "divps %%xmm0, %%xmm1\n"

        : : "r"(&zeros), "r"(&ones) : "xmm0", "xmm1"
    );
#endif /* _MSC_VER */

    /* Restore mxcsr */
    _mm_setcsr(csr);

    ok(ExceptionCaught, "The exception was not caught.\n");

    p = SetUnhandledExceptionFilter(NULL);
    ok(p == ExceptionFilterSSEException, "Unexpected old filter : 0x%p", p);
}

#define CUSTOM_CODE 0xE0012345
#define NONCONTINUABLE_CODE 0xE0054321
#define FILTER_EXIT_CODE 0x12345678

static
DWORD
WINAPI
CrashThread(
    _In_ PVOID Parameter)
{
    *(volatile LONG *)Parameter = 1;
    return 0;
}

static
LONG
WINAPI
ExecuteHandlerFilter(
    _In_ PEXCEPTION_POINTERS ExceptionInfo)
{
    return EXCEPTION_EXECUTE_HANDLER;
}

static
LONG
WINAPI
ExitProcessFilter(
    _In_ PEXCEPTION_POINTERS ExceptionInfo)
{
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == CUSTOM_CODE)
        ExitProcess(FILTER_EXIT_CODE);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Child process: die of an unhandled exception */
static
VOID
RunCrashChild(
    _In_ PCSTR Mode)
{
    HANDLE hThread;

    /* No error box: the default filter lets the startup handler end the process */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (!strcmp(Mode, "av"))
    {
        *(volatile LONG *)NULL = 1;
    }
    else if (!strcmp(Mode, "raise"))
    {
        RaiseException(CUSTOM_CODE, 0, 0, NULL);
    }
    else if (!strcmp(Mode, "noncontinuable"))
    {
        RaiseException(NONCONTINUABLE_CODE, EXCEPTION_NONCONTINUABLE, 0, NULL);
    }
    else if (!strcmp(Mode, "thread"))
    {
        hThread = CreateThread(NULL, 0, CrashThread, NULL, 0, NULL);
        if (hThread)
            WaitForSingleObject(hThread, 30000);
    }
    else if (!strcmp(Mode, "filter"))
    {
        SetUnhandledExceptionFilter(ExecuteHandlerFilter);
        RaiseException(CUSTOM_CODE, 0, 0, NULL);
    }
    else if (!strcmp(Mode, "filterexit"))
    {
        SetUnhandledExceptionFilter(ExitProcessFilter);
        RaiseException(CUSTOM_CODE, 0, 0, NULL);
    }
#if defined(__GNUC__) || defined(__clang__)
    else if (!strcmp(Mode, "badstack"))
    {
        /* No usable stack: the exception cannot be dispatched in user mode */
#ifdef _M_AMD64
        __asm__ __volatile__("movq $0x8000, %%rsp\n\tpushq %%rax" : : : "memory");
#else
        __asm__ __volatile__("movl $0x8000, %%esp\n\tpushl %%eax" : : : "memory");
#endif
    }
#endif
    ExitProcess(1);
}

static
VOID
TestCrashExitCode(
    _In_ PCWSTR Mode,
    _In_ DWORD ExpectedCode)
{
    BOOL Success;
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 64];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    DWORD Wait, ExitCode;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine,
                    sizeof(CommandLine),
                    L"\"%ls\" SetUnhandledExceptionFilter %ls",
                    FileName,
                    Mode);

    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    Success = CreateProcessW(FileName,
                             CommandLine,
                             NULL,
                             NULL,
                             FALSE,
                             0,
                             NULL,
                             NULL,
                             &StartupInfo,
                             &ProcessInfo);
    ok(Success, "%ls: CreateProcess failed with %lu\n", Mode, GetLastError());
    if (!Success)
        return;
    CloseHandle(ProcessInfo.hThread);

    Wait = WaitForSingleObject(ProcessInfo.hProcess, 60000);
    ok(Wait == WAIT_OBJECT_0, "%ls: wait returned %lu\n", Mode, Wait);
    if (Wait != WAIT_OBJECT_0)
        TerminateProcess(ProcessInfo.hProcess, 0xDEAD);

    ExitCode = 0x55555555;
    Success = GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    ok(Success, "%ls: GetExitCodeProcess failed with %lu\n", Mode, GetLastError());
    ok(ExitCode == ExpectedCode, "%ls: exit code 0x%08lx, expected 0x%08lx\n",
       Mode, ExitCode, ExpectedCode);
    CloseHandle(ProcessInfo.hProcess);
}

/*
 * A process killed by an unhandled exception exits with the exception code,
 * also when the exception is raised in another thread, when a filter set with
 * SetUnhandledExceptionFilter returns EXCEPTION_EXECUTE_HANDLER, and when the
 * exception cannot be dispatched to user mode at all. A filter that ends the
 * process itself decides the exit code: it runs before anything else.
 */
static
VOID
TestCrashExitCodes(VOID)
{
    TestCrashExitCode(L"av", STATUS_ACCESS_VIOLATION);
    TestCrashExitCode(L"raise", CUSTOM_CODE);
    TestCrashExitCode(L"noncontinuable", NONCONTINUABLE_CODE);
    TestCrashExitCode(L"thread", STATUS_ACCESS_VIOLATION);
    TestCrashExitCode(L"filter", CUSTOM_CODE);
    TestCrashExitCode(L"filterexit", FILTER_EXIT_CODE);
#if defined(__GNUC__) || defined(__clang__)
    TestCrashExitCode(L"badstack", STATUS_ACCESS_VIOLATION);
#endif
}

START_TEST(SetUnhandledExceptionFilter)
{
    int argc;
    char **argv;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3)
    {
        RunCrashChild(argv[2]);
        return;
    }

    TestSetUnhandledExceptionFilter();
    TestCrashExitCodes();
    TestSSEExceptions();
}
