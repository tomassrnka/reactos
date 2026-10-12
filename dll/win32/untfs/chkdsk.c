/*
 * PROJECT:     ReactOS NTFS File System Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Chkdsk entry of the NTFS provider: runs the checker in ntfsck.dll
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#define NTOS_MODE_USER
#include <ndk/rtlfuncs.h>
#include <ndk/ldrfuncs.h>
#include <ndk/umfuncs.h>
#include <ntstatus.h>
#include <fmifs/fmifs.h>
#include <fslib/ntfsck.h>
#include <pseh/pseh2.h>

#define NDEBUG
#include <debug.h>

RTL_CRITICAL_SECTION UntfsChkdskLock;

/*
 * The argument was reserved and unused before: a caller may pass anything. Take a copy under
 * an exception handler and use it only if it is a CHKDSK_EXTENSION.
 */
static BOOLEAN
GetExtension(PVOID p, PCHKDSK_EXTENSION Copy)
{
    BOOLEAN Valid = FALSE;

    if (!p)
        return FALSE;
    _SEH2_TRY
    {
        RtlCopyMemory(Copy, p, sizeof(*Copy));
        Valid = Copy->Magic == CHKDSK_EXTENSION_MAGIC && Copy->Size >= sizeof(CHKDSK_EXTENSION);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Valid = FALSE;
    }
    _SEH2_END;
    return Valid;
}

static VOID
SetExitCode(PVOID p, ULONG ExitCode)
{
    _SEH2_TRY
    {
        ((PCHKDSK_EXTENSION)p)->ExitCode = ExitCode;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
    }
    _SEH2_END;
}

BOOLEAN
NTAPI
UntfsChkdsk(
    IN PUNICODE_STRING DriveRoot,
    IN PFMIFSCALLBACK Callback,
    IN BOOLEAN FixErrors,
    IN BOOLEAN Verbose,
    IN BOOLEAN CheckOnlyIfDirty,
    IN BOOLEAN ScanDrive,
    IN PVOID pUnknown1,
    IN PVOID pUnknown2,
    IN PVOID pUnknown3,
    IN PVOID pUnknown4,
    IN PULONG ExitStatus)
{
    static const UNICODE_STRING DllName = RTL_CONSTANT_STRING(L"ntfsck.dll");
    static const ANSI_STRING RunName = RTL_CONSTANT_STRING("NtfsckRun");
    CHKDSK_EXTENSION Ext;
    BOOLEAN HaveExt = GetExtension(pUnknown1, &Ext);
    NTFSCK_PARAMS Params;
    PNTFSCK_RUN NtfsckRunFn;
    PVOID Module;
    NTSTATUS Status;
    BOOLEAN Ran = FALSE;

    UNREFERENCED_PARAMETER(pUnknown2);
    UNREFERENCED_PARAMETER(pUnknown3);
    UNREFERENCED_PARAMETER(pUnknown4);

    RtlZeroMemory(&Params, sizeof(Params));
    Params.Size = sizeof(Params);
    Params.Version = NTFSCK_PARAMS_VERSION;
    Params.Volume = DriveRoot;
    Params.Callback = Callback;
    if (FixErrors)
        Params.Flags |= NTFSCK_FLAG_FIX;
    if (Verbose)
        Params.Flags |= NTFSCK_FLAG_VERBOSE;
    if (CheckOnlyIfDirty)
        Params.Flags |= NTFSCK_FLAG_DIRTY_ONLY;
    if (ScanDrive)
        Params.Flags |= NTFSCK_FLAG_SCAN;
    if (HaveExt)
    {
        Params.Flags |= Ext.Flags;
        Params.LogSizeKB = Ext.LogSizeKB;
    }
    Params.ExitCode = 3;

    /*
     * The checker keeps its state in globals, so each check gets a freshly loaded copy of
     * ntfsck.dll, and checks run one at a time.
     */
    RtlEnterCriticalSection(&UntfsChkdskLock);
    Status = LdrLoadDll(NULL, NULL, (PUNICODE_STRING)&DllName, &Module);
    if (NT_SUCCESS(Status))
    {
        Status = LdrGetProcedureAddress(Module, (PANSI_STRING)&RunName, 0, (PVOID *)&NtfsckRunFn);
        if (NT_SUCCESS(Status))
        {
            Ran = NtfsckRunFn(&Params);
            Status = Params.Status;
        }
        LdrUnloadDll(Module);
    }
    else
    {
        DPRINT1("untfs: ntfsck.dll not loaded (0x%08lx)\n", Status);
    }
    if (!NT_SUCCESS(Status) && Params.Status == STATUS_SUCCESS)
        Params.Status = Status;
    if (Status == Params.Status && !Ran && Params.ExitCode == 3 && Callback &&
        (Status == STATUS_DLL_NOT_FOUND || Status == STATUS_PROCEDURE_NOT_FOUND || Status == STATUS_INVALID_IMAGE_FORMAT))
    {
        TEXTOUTPUT Text;
        Text.Lines = 1;
        Text.Output = "The NTFS checker (ntfsck.dll) could not be loaded.\n";
        Callback(OUTPUT, 0, &Text);
    }
    RtlLeaveCriticalSection(&UntfsChkdskLock);

    if (HaveExt)
        SetExitCode(pUnknown1, Params.ExitCode);
    *ExitStatus = (ULONG)Status;
    /*
     * Success means "checked, nothing wrong": a check that found problems reports
     * STATUS_DISK_CORRUPT_ERROR and FALSE, for callers that only look at the BOOLEAN.
     */
    return Ran && NT_SUCCESS(Status);
}
