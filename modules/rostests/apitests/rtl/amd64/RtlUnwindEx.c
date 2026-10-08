/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for x64 RtlUnwindEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <rtltests.h>

/*
 * The test function: a frame of 0x28 bytes with a handler for exceptions
 * and unwinds, which raises a breakpoint and returns.
 */
static const UCHAR TestCode[] =
{
    0x48, 0x83, 0xEC, 0x28, /* 00: sub rsp, 28h */
    0xCC,                   /* 04: int 3 */
    0x90,                   /* 05: nop */
    0x48, 0x83, 0xC4, 0x28, /* 06: add rsp, 28h */
    0xC3,                   /* 0A: ret */
};

#define TEST_CODE_RESUME   0x06
#define TEST_CODE_THUNK    0x10
#define TEST_CODE_UNWIND   0x20
#define TEST_CODE_FUNCTION 0x40

static PUCHAR CodeMem;
static ULONG HandlerCalls;

static
EXCEPTION_DISPOSITION
CollidedHandler(
    _Inout_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID EstablisherFrame,
    _Inout_ PCONTEXT ContextRecord,
    _In_ PDISPATCHER_CONTEXT DispatcherContext)
{
    CONTEXT Context;

    switch (HandlerCalls++)
    {
        case 0:
            /* The breakpoint: unwind to this frame */
            ok_hex(ExceptionRecord->ExceptionCode, STATUS_BREAKPOINT);
            RtlUnwindEx(EstablisherFrame, CodeMem + TEST_CODE_RESUME, NULL, NULL, &Context, NULL);
            ok(0, "RtlUnwindEx returned\n");
            break;

        case 1:
            /* The unwind reached its target frame; raise from its handler */
            ok_hex(ExceptionRecord->ExceptionCode, STATUS_UNWIND);
            ok_hex(ExceptionRecord->ExceptionFlags, EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND);
            RaiseException(0xDEADBEEF, 0, 0, NULL);
            ok(0, "RaiseException returned\n");
            break;

        case 2:
            /* Unwind to this frame again, across the unwind in progress */
            ok_hex(ExceptionRecord->ExceptionCode, 0xDEADBEEF);
            ok_hex(ExceptionRecord->ExceptionFlags & ~EXCEPTION_SOFTWARE_ORIGINATE, 0);
            RtlUnwindEx(EstablisherFrame, CodeMem + TEST_CODE_RESUME, NULL, NULL, &Context, NULL);
            ok(0, "RtlUnwindEx returned\n");
            break;

        case 3:
            /* The collided unwind calls this frame's handler again, as its target */
            ok_hex(ExceptionRecord->ExceptionCode, STATUS_UNWIND);
            ok_hex(ExceptionRecord->ExceptionFlags,
                   EXCEPTION_UNWINDING | EXCEPTION_COLLIDED_UNWIND | EXCEPTION_TARGET_UNWIND);
            ok_ptr((PVOID)DispatcherContext->EstablisherFrame, EstablisherFrame);
            ok_ptr(DispatcherContext->ContextRecord, ContextRecord);
            ok_ptr((PVOID)ContextRecord->Rip, CodeMem + 4);
            break;

        default:
            ok(0, "Unexpected handler call %lu, code 0x%lx, flags 0x%lx\n",
               HandlerCalls, ExceptionRecord->ExceptionCode, ExceptionRecord->ExceptionFlags);
            break;
    }

    return ExceptionContinueSearch;
}

static
VOID
Test_CollidedUnwindTargetFrame(VOID)
{
    PRUNTIME_FUNCTION Function;
    PUCHAR UnwindInfo;

    CodeMem = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!CodeMem)
    {
        skip("VirtualAlloc failed: %lu\n", GetLastError());
        return;
    }

    RtlCopyMemory(CodeMem, TestCode, sizeof(TestCode));

    /* The handler must lie within 4 GB of the base: mov rax, imm64; jmp rax */
    CodeMem[TEST_CODE_THUNK + 0] = 0x48;
    CodeMem[TEST_CODE_THUNK + 1] = 0xB8;
    *(PULONG64)&CodeMem[TEST_CODE_THUNK + 2] = (ULONG64)CollidedHandler;
    CodeMem[TEST_CODE_THUNK + 10] = 0xFF;
    CodeMem[TEST_CODE_THUNK + 11] = 0xE0;

    /* Version 1, one unwind code (UWOP_ALLOC_SMALL 28h at offset 4), handler */
    UnwindInfo = CodeMem + TEST_CODE_UNWIND;
    UnwindInfo[0] = 1 | ((UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER) << 3);
    UnwindInfo[1] = 4;
    UnwindInfo[2] = 1;
    UnwindInfo[3] = 0;
    UnwindInfo[4] = 4;
    UnwindInfo[5] = 0x42;
    *(PULONG)&UnwindInfo[8] = TEST_CODE_THUNK;

    Function = (PRUNTIME_FUNCTION)(CodeMem + TEST_CODE_FUNCTION);
    Function->BeginAddress = 0;
    Function->EndAddress = sizeof(TestCode);
    Function->UnwindData = TEST_CODE_UNWIND;

    if (!RtlAddFunctionTable(Function, 1, (ULONG64)CodeMem))
    {
        skip("RtlAddFunctionTable failed\n");
        VirtualFree(CodeMem, 0, MEM_RELEASE);
        return;
    }

    HandlerCalls = 0;
    ((VOID (*)(VOID))CodeMem)();
    ok_int(HandlerCalls, 4);

    RtlDeleteFunctionTable(Function);
    VirtualFree(CodeMem, 0, MEM_RELEASE);
}

START_TEST(RtlUnwindEx)
{
    Test_CollidedUnwindTargetFrame();
}
