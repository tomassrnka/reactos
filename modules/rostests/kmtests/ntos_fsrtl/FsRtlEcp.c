/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the kernel exports WinFsp needs: ECP lists, reparse buffer
 *              validation, one-time initialization, active processor count and others
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

/* Declared by the headers only for newer NTDDI_VERSIONs than this driver is built for. No NTKERNELAPI:
 * some of these are also implemented in ntoskrnl_vista, which this driver links */
#define TEST_ECP_NONPAGED_POOL 0x00000002
#undef CcCopyWriteWontFlush

typedef struct _TEST_ECP_LIST *PTEST_ECP_LIST;
typedef VOID (*PTEST_ECP_CLEANUP)(PVOID EcpContext, const GUID *EcpType);

typedef NTSTATUS (NTAPI *P_FsRtlAllocateExtraCreateParameterList)(ULONG Flags, PTEST_ECP_LIST *EcpList);
static P_FsRtlAllocateExtraCreateParameterList pFsRtlAllocateExtraCreateParameterList;
#define FsRtlAllocateExtraCreateParameterList pFsRtlAllocateExtraCreateParameterList
typedef VOID (NTAPI *P_FsRtlFreeExtraCreateParameterList)(PTEST_ECP_LIST EcpList);
static P_FsRtlFreeExtraCreateParameterList pFsRtlFreeExtraCreateParameterList;
#define FsRtlFreeExtraCreateParameterList pFsRtlFreeExtraCreateParameterList
typedef NTSTATUS (NTAPI *P_FsRtlAllocateExtraCreateParameter)(const GUID *EcpType, ULONG SizeOfContext, ULONG Flags,
                                                             PTEST_ECP_CLEANUP Cleanup, ULONG PoolTag, PVOID *EcpContext);
static P_FsRtlAllocateExtraCreateParameter pFsRtlAllocateExtraCreateParameter;
#define FsRtlAllocateExtraCreateParameter pFsRtlAllocateExtraCreateParameter
typedef VOID (NTAPI *P_FsRtlFreeExtraCreateParameter)(PVOID EcpContext);
static P_FsRtlFreeExtraCreateParameter pFsRtlFreeExtraCreateParameter;
#define FsRtlFreeExtraCreateParameter pFsRtlFreeExtraCreateParameter
typedef NTSTATUS (NTAPI *P_FsRtlInsertExtraCreateParameter)(PTEST_ECP_LIST EcpList, PVOID EcpContext);
static P_FsRtlInsertExtraCreateParameter pFsRtlInsertExtraCreateParameter;
#define FsRtlInsertExtraCreateParameter pFsRtlInsertExtraCreateParameter
typedef NTSTATUS (NTAPI *P_FsRtlFindExtraCreateParameter)(PTEST_ECP_LIST EcpList, const GUID *EcpType,
                                                         PVOID *EcpContext, PULONG EcpContextSize);
static P_FsRtlFindExtraCreateParameter pFsRtlFindExtraCreateParameter;
#define FsRtlFindExtraCreateParameter pFsRtlFindExtraCreateParameter
typedef NTSTATUS (NTAPI *P_FsRtlRemoveExtraCreateParameter)(PTEST_ECP_LIST EcpList, const GUID *EcpType,
                                                           PVOID *EcpContext, PULONG EcpContextSize);
static P_FsRtlRemoveExtraCreateParameter pFsRtlRemoveExtraCreateParameter;
#define FsRtlRemoveExtraCreateParameter pFsRtlRemoveExtraCreateParameter
typedef NTSTATUS (NTAPI *P_FsRtlGetNextExtraCreateParameter)(PTEST_ECP_LIST EcpList, PVOID Current, LPGUID NextType,
                                                            PVOID *NextContext, PULONG NextSize);
static P_FsRtlGetNextExtraCreateParameter pFsRtlGetNextExtraCreateParameter;
#define FsRtlGetNextExtraCreateParameter pFsRtlGetNextExtraCreateParameter
typedef BOOLEAN (NTAPI *P_FsRtlIsEcpFromUserMode)(PVOID EcpContext);
static P_FsRtlIsEcpFromUserMode pFsRtlIsEcpFromUserMode;
#define FsRtlIsEcpFromUserMode pFsRtlIsEcpFromUserMode
typedef VOID (NTAPI *P_FsRtlAcknowledgeEcp)(PVOID EcpContext);
static P_FsRtlAcknowledgeEcp pFsRtlAcknowledgeEcp;
#define FsRtlAcknowledgeEcp pFsRtlAcknowledgeEcp
typedef BOOLEAN (NTAPI *P_FsRtlIsEcpAcknowledged)(PVOID EcpContext);
static P_FsRtlIsEcpAcknowledged pFsRtlIsEcpAcknowledged;
#define FsRtlIsEcpAcknowledged pFsRtlIsEcpAcknowledged
typedef NTSTATUS (NTAPI *P_FsRtlGetEcpListFromIrp)(PIRP Irp, PTEST_ECP_LIST *EcpList);
static P_FsRtlGetEcpListFromIrp pFsRtlGetEcpListFromIrp;
#define FsRtlGetEcpListFromIrp pFsRtlGetEcpListFromIrp
typedef NTSTATUS (NTAPI *P_FsRtlValidateReparsePointBuffer)(ULONG BufferLength, PVOID ReparseBuffer);
static P_FsRtlValidateReparsePointBuffer pFsRtlValidateReparsePointBuffer;
#define FsRtlValidateReparsePointBuffer pFsRtlValidateReparsePointBuffer
typedef BOOLEAN (NTAPI *P_CcCopyWriteWontFlush)(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length);
static P_CcCopyWriteWontFlush pCcCopyWriteWontFlush;
#define CcCopyWriteWontFlush pCcCopyWriteWontFlush
BOOLEAN NTAPI FsRtlAreThereCurrentOrInProgressFileLocks(PFILE_LOCK FileLock);
typedef BOOLEAN (NTAPI *P_FsRtlCurrentOplockH)(PVOID Oplock);
static P_FsRtlCurrentOplockH pFsRtlCurrentOplockH;
#define FsRtlCurrentOplockH pFsRtlCurrentOplockH
typedef NTSTATUS (NTAPI *P_FsRtlRegisterUncProviderEx)(PHANDLE MupHandle, PUNICODE_STRING Name, PDEVICE_OBJECT Device, ULONG Flags);
static P_FsRtlRegisterUncProviderEx pFsRtlRegisterUncProviderEx;
#define FsRtlRegisterUncProviderEx pFsRtlRegisterUncProviderEx
typedef ULONG (NTAPI *P_KeQueryActiveProcessorCount)(PKAFFINITY ActiveProcessors);
static P_KeQueryActiveProcessorCount pKeQueryActiveProcessorCount;
#define KeQueryActiveProcessorCount pKeQueryActiveProcessorCount
typedef NTSTATUS (NTAPI *P_IoCreateFileEx)(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                          PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                          ULONG ShareAccess, ULONG Disposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength,
                                          CREATE_FILE_TYPE CreateFileType, PVOID InternalParameters, ULONG Options, PVOID DriverContext);
static P_IoCreateFileEx pIoCreateFileEx;
#define IoCreateFileEx pIoCreateFileEx

typedef struct _TEST_RUN_ONCE { PVOID Ptr; } TEST_RUN_ONCE, *PTEST_RUN_ONCE;
typedef ULONG (NTAPI *PTEST_RUN_ONCE_INIT)(PTEST_RUN_ONCE RunOnce, PVOID Parameter, PVOID *Context);
typedef NTSTATUS (NTAPI *P_RtlRunOnceExecuteOnce)(PTEST_RUN_ONCE RunOnce, PTEST_RUN_ONCE_INIT InitFn, PVOID Parameter, PVOID *Context);
static P_RtlRunOnceExecuteOnce pRtlRunOnceExecuteOnce;
#define RtlRunOnceExecuteOnce pRtlRunOnceExecuteOnce

static const GUID EcpTypeA = { 0x11111111, 0x1111, 0x1111, { 1, 1, 1, 1, 1, 1, 1, 1 } };
static const GUID EcpTypeB = { 0x22222222, 0x2222, 0x2222, { 2, 2, 2, 2, 2, 2, 2, 2 } };

static ULONG CleanupCount;

static
VOID
EcpCleanup(
    _Inout_ PVOID EcpContext,
    _In_ const GUID *EcpType)
{
    UNREFERENCED_PARAMETER(EcpContext);
    UNREFERENCED_PARAMETER(EcpType);
    CleanupCount++;
}

static
VOID
TestEcp(VOID)
{
    PTEST_ECP_LIST List;
    PVOID A, B, Context, Next;
    ULONG Size;
    GUID Type;
    NTSTATUS Status;

    Status = FsRtlAllocateExtraCreateParameterList(0, &List);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = FsRtlAllocateExtraCreateParameter(&EcpTypeA, 24, 0, EcpCleanup, 'tseT', &A);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = FsRtlAllocateExtraCreateParameter(&EcpTypeB, 8, TEST_ECP_NONPAGED_POOL, EcpCleanup, 0, &B);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (A == NULL || B == NULL)
    {
        skip(FALSE, "No ECPs\n");
        return;
    }
    RtlFillMemory(A, 24, 0xAA);

    ok_bool_false(FsRtlIsEcpFromUserMode(A), "FsRtlIsEcpFromUserMode");
    ok_bool_false(FsRtlIsEcpAcknowledged(A), "FsRtlIsEcpAcknowledged");
    FsRtlAcknowledgeEcp(A);
    ok_bool_true(FsRtlIsEcpAcknowledged(A), "FsRtlIsEcpAcknowledged");

    Status = FsRtlFindExtraCreateParameter(List, &EcpTypeA, &Context, &Size);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    ok_eq_pointer(Context, NULL);

    Status = FsRtlInsertExtraCreateParameter(List, A);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = FsRtlInsertExtraCreateParameter(List, A);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = FsRtlInsertExtraCreateParameter(List, B);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = FsRtlFindExtraCreateParameter(List, &EcpTypeB, &Context, &Size);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_pointer(Context, B);
    ok_eq_ulong(Size, 8);
    Status = FsRtlFindExtraCreateParameter(List, &EcpTypeA, NULL, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* A second ECP of a type already in the list collides */
    {
        PVOID Dup;

        Status = FsRtlAllocateExtraCreateParameter(&EcpTypeA, 4, 0, EcpCleanup, 0, &Dup);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Status = FsRtlInsertExtraCreateParameter(List, Dup);
            ok_eq_hex(Status, STATUS_OBJECT_NAME_COLLISION);
            FsRtlFreeExtraCreateParameter(Dup);
            ok_eq_ulong(CleanupCount, 1);
        }
    }

    /* Enumeration is in insertion order */
    Status = FsRtlGetNextExtraCreateParameter(List, NULL, &Type, &Next, &Size);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_pointer(Next, A);
    ok_eq_ulong(Size, 24);
    ok(RtlEqualMemory(&Type, &EcpTypeA, sizeof(Type)), "First type is not A\n");
    Status = FsRtlGetNextExtraCreateParameter(List, A, &Type, &Next, &Size);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_pointer(Next, B);
    Status = FsRtlGetNextExtraCreateParameter(List, B, &Type, &Next, &Size);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    ok_eq_pointer(Next, NULL);

    /* A removed ECP is the caller's again */
    Status = FsRtlRemoveExtraCreateParameter(List, &EcpTypeA, &Context, &Size);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_pointer(Context, A);
    Status = FsRtlFindExtraCreateParameter(List, &EcpTypeA, &Context, &Size);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    FsRtlFreeExtraCreateParameter(A);
    ok_eq_ulong(CleanupCount, 2);

    /* Freeing the list frees what is in it */
    FsRtlFreeExtraCreateParameterList(List);
    ok_eq_ulong(CleanupCount, 3);

}

static
VOID
TestEcpIrp(VOID)
{
    PTEST_ECP_LIST List, Found = NULL;
    PIRP Irp;
    PVOID A, Next;
    ULONG Size;
    NTSTATUS Status;

    Irp = IoAllocateIrp(1, FALSE);
    if (Irp == NULL)
    {
        skip(FALSE, "No IRP\n");
        return;
    }

    /* Only a create IRP can carry a list */
    Status = FsRtlGetEcpListFromIrp(Irp, &Found);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Status = FsRtlAllocateExtraCreateParameterList(0, &List);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = FsRtlAllocateExtraCreateParameter(&EcpTypeA, 24, 0, NULL, 0, &A);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = FsRtlInsertExtraCreateParameter(List, A);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Irp->Flags |= IRP_CREATE_OPERATION;
    Irp->UserBuffer = List;
    Status = FsRtlGetEcpListFromIrp(Irp, &Found);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_pointer(Found, List);

    /* The enumeration of ntoskrnl_vista reads the lists made here */
    Next = NULL;
    Size = 0;
    Status = FsRtlGetNextExtraCreateParameter(Found, NULL, NULL, &Next, &Size);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_pointer(Next, A);
    ok_eq_ulong(Size, 24);

    IoFreeIrp(Irp);
    FsRtlFreeExtraCreateParameterList(List);
}

typedef struct _TEST_SYMLINK
{
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    ULONG Flags;
    WCHAR PathBuffer[8];
} TEST_SYMLINK;

typedef struct _TEST_GUID_BUFFER
{
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    GUID ReparseGuid;
    UCHAR Data[4];
} TEST_GUID_BUFFER;

static
VOID
TestReparse(VOID)
{
    TEST_SYMLINK Link;
    TEST_GUID_BUFFER Guid;
    NTSTATUS Status;
    const ULONG Header = 8;
    const ULONG Total = sizeof(Link);

    RtlZeroMemory(&Link, sizeof(Link));
    Link.ReparseTag = IO_REPARSE_TAG_SYMLINK;
    Link.ReparseDataLength = (USHORT)(Total - Header);
    Link.SubstituteNameOffset = 0;
    Link.SubstituteNameLength = 8;
    Link.PrintNameOffset = 8;
    Link.PrintNameLength = 8;
    Status = FsRtlValidateReparsePointBuffer(Total, &Link);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = FsRtlValidateReparsePointBuffer(Header - 1, &Link);
    ok_eq_hex(Status, STATUS_IO_REPARSE_DATA_INVALID);
    Status = FsRtlValidateReparsePointBuffer(Total - 2, &Link);
    ok_eq_hex(Status, STATUS_IO_REPARSE_DATA_INVALID);

    Link.PrintNameLength = 2 * sizeof(Link.PathBuffer);
    Status = FsRtlValidateReparsePointBuffer(Total, &Link);
    ok_eq_hex(Status, STATUS_IO_REPARSE_DATA_INVALID);
    Link.PrintNameLength = 8;

    Link.SubstituteNameOffset = 1;
    Status = FsRtlValidateReparsePointBuffer(Total, &Link);
    ok_eq_hex(Status, STATUS_IO_REPARSE_DATA_INVALID);
    Link.SubstituteNameOffset = 0;

    /* Reserved bits in a Microsoft tag */
    Link.ReparseTag = 0x80010000;
    Status = FsRtlValidateReparsePointBuffer(Total, &Link);
    ok_eq_hex(Status, STATUS_IO_REPARSE_TAG_INVALID);

    /* A tag of another vendor has the GUID header, and a GUID that is not null */
    RtlZeroMemory(&Guid, sizeof(Guid));
    Guid.ReparseTag = 0x00000123;
    Guid.ReparseDataLength = sizeof(Guid.Data);
    Guid.ReparseGuid = EcpTypeA;
    Status = FsRtlValidateReparsePointBuffer(sizeof(Guid), &Guid);
    ok_eq_hex(Status, STATUS_SUCCESS);
    RtlZeroMemory(&Guid.ReparseGuid, sizeof(GUID));
    Status = FsRtlValidateReparsePointBuffer(sizeof(Guid), &Guid);
    ok_eq_hex(Status, STATUS_IO_REPARSE_DATA_INVALID);
    Guid.ReparseGuid = EcpTypeA;
    Status = FsRtlValidateReparsePointBuffer(Header + sizeof(Guid.Data), &Guid);
    ok_eq_hex(Status, STATUS_IO_REPARSE_DATA_INVALID);
}

static ULONG InitCalls;
static ULONG InitFailOnce = 1;

static
ULONG
NTAPI
RunOnceInit(
    _Inout_ PTEST_RUN_ONCE RunOnce,
    _Inout_opt_ PVOID Parameter,
    _Out_opt_ PVOID *Context)
{
    UNREFERENCED_PARAMETER(RunOnce);

    InitCalls++;
    if (Parameter != NULL && InitFailOnce)
    {
        InitFailOnce = 0;
        return FALSE;
    }
    *Context = (PVOID)(ULONG_PTR)0x1230;
    return TRUE;
}

static
VOID
TestRunOnce(VOID)
{
    TEST_RUN_ONCE Once = { NULL };
    PVOID Context = NULL;
    NTSTATUS Status;

    Status = RtlRunOnceExecuteOnce(&Once, RunOnceInit, (PVOID)1, &Context);
    ok_eq_hex(Status, STATUS_UNSUCCESSFUL);
    ok_eq_ulong(InitCalls, 1);
    ok_eq_pointer(Once.Ptr, NULL);

    Status = RtlRunOnceExecuteOnce(&Once, RunOnceInit, (PVOID)1, &Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(InitCalls, 2);
    ok_eq_pointer(Context, (PVOID)(ULONG_PTR)0x1230);

    Context = NULL;
    Status = RtlRunOnceExecuteOnce(&Once, RunOnceInit, (PVOID)1, &Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(InitCalls, 2);
    ok_eq_pointer(Context, (PVOID)(ULONG_PTR)0x1230);
}

static
VOID
TestMisc(VOID)
{
    KAFFINITY Active = 0, Expected;
    ULONG Count, Bits = 0;
    LARGE_INTEGER Offset = { { 0 } };
    NTSTATUS Status;
    HANDLE Handle, MupHandle = (HANDLE)1;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\Null");
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;

    Count = KeQueryActiveProcessorCount(&Active);
    Expected = KeQueryActiveProcessors();
    ok_eq_ulongptr(Active, Expected);
    while (Expected)
    {
        Bits += (ULONG)(Expected & 1);
        Expected >>= 1;
    }
    ok(Count >= 1, "Count = %lu\n", Count);
    ok_eq_ulong(Count, Bits);
    ok_eq_ulong(KeQueryActiveProcessorCount(NULL), Count);

    ok_bool_true(CcCopyWriteWontFlush(NULL, &Offset, 0x10000), "CcCopyWriteWontFlush(64k)");
    ok_bool_false(CcCopyWriteWontFlush(NULL, &Offset, 0x10001), "CcCopyWriteWontFlush(64k + 1)");

    ok_bool_false(FsRtlCurrentOplockH(NULL), "FsRtlCurrentOplockH");

    Status = FsRtlRegisterUncProviderEx(&MupHandle, &Name, NULL, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_pointer(MupHandle, NULL);

    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = IoCreateFileEx(&Handle, FILE_READ_DATA, &ObjectAttributes, &IoStatus, NULL, 0,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0, CreateFileTypeNone,
                            NULL, IO_NO_PARAMETER_CHECKING, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ObCloseHandle(Handle, KernelMode);
}

static
PVOID
GetExport(PCWSTR Name)
{
    UNICODE_STRING String;

    RtlInitUnicodeString(&String, Name);
    return MmGetSystemRoutineAddress(&String);
}

START_TEST(FsRtlEcp)
{
    /* Everything is looked up: the exports only exist for NT 6+ builds. Four of them are also in
     * ntoskrnl_vista, which this driver links; looking them up tests the exports of ntoskrnl itself. */
    BOOLEAN Missing = FALSE;

    pFsRtlGetEcpListFromIrp = GetExport(L"FsRtlGetEcpListFromIrp");
    if (pFsRtlGetEcpListFromIrp == NULL) { Missing = TRUE; trace("FsRtlGetEcpListFromIrp is not exported\n"); }
    pFsRtlGetNextExtraCreateParameter = GetExport(L"FsRtlGetNextExtraCreateParameter");
    if (pFsRtlGetNextExtraCreateParameter == NULL) { Missing = TRUE; trace("FsRtlGetNextExtraCreateParameter is not exported\n"); }
    pFsRtlValidateReparsePointBuffer = GetExport(L"FsRtlValidateReparsePointBuffer");
    if (pFsRtlValidateReparsePointBuffer == NULL) { Missing = TRUE; trace("FsRtlValidateReparsePointBuffer is not exported\n"); }
    pKeQueryActiveProcessorCount = GetExport(L"KeQueryActiveProcessorCount");
    if (pKeQueryActiveProcessorCount == NULL) { Missing = TRUE; trace("KeQueryActiveProcessorCount is not exported\n"); }
    pFsRtlAllocateExtraCreateParameterList = GetExport(L"FsRtlAllocateExtraCreateParameterList");
    if (pFsRtlAllocateExtraCreateParameterList == NULL) { Missing = TRUE; trace("FsRtlAllocateExtraCreateParameterList is not exported\n"); }
    pFsRtlFreeExtraCreateParameterList = GetExport(L"FsRtlFreeExtraCreateParameterList");
    if (pFsRtlFreeExtraCreateParameterList == NULL) { Missing = TRUE; trace("FsRtlFreeExtraCreateParameterList is not exported\n"); }
    pFsRtlAllocateExtraCreateParameter = GetExport(L"FsRtlAllocateExtraCreateParameter");
    if (pFsRtlAllocateExtraCreateParameter == NULL) { Missing = TRUE; trace("FsRtlAllocateExtraCreateParameter is not exported\n"); }
    pFsRtlFreeExtraCreateParameter = GetExport(L"FsRtlFreeExtraCreateParameter");
    if (pFsRtlFreeExtraCreateParameter == NULL) { Missing = TRUE; trace("FsRtlFreeExtraCreateParameter is not exported\n"); }
    pFsRtlInsertExtraCreateParameter = GetExport(L"FsRtlInsertExtraCreateParameter");
    if (pFsRtlInsertExtraCreateParameter == NULL) { Missing = TRUE; trace("FsRtlInsertExtraCreateParameter is not exported\n"); }
    pFsRtlFindExtraCreateParameter = GetExport(L"FsRtlFindExtraCreateParameter");
    if (pFsRtlFindExtraCreateParameter == NULL) { Missing = TRUE; trace("FsRtlFindExtraCreateParameter is not exported\n"); }
    pFsRtlRemoveExtraCreateParameter = GetExport(L"FsRtlRemoveExtraCreateParameter");
    if (pFsRtlRemoveExtraCreateParameter == NULL) { Missing = TRUE; trace("FsRtlRemoveExtraCreateParameter is not exported\n"); }
    pFsRtlIsEcpFromUserMode = GetExport(L"FsRtlIsEcpFromUserMode");
    if (pFsRtlIsEcpFromUserMode == NULL) { Missing = TRUE; trace("FsRtlIsEcpFromUserMode is not exported\n"); }
    pFsRtlAcknowledgeEcp = GetExport(L"FsRtlAcknowledgeEcp");
    if (pFsRtlAcknowledgeEcp == NULL) { Missing = TRUE; trace("FsRtlAcknowledgeEcp is not exported\n"); }
    pFsRtlIsEcpAcknowledged = GetExport(L"FsRtlIsEcpAcknowledged");
    if (pFsRtlIsEcpAcknowledged == NULL) { Missing = TRUE; trace("FsRtlIsEcpAcknowledged is not exported\n"); }
    pCcCopyWriteWontFlush = GetExport(L"CcCopyWriteWontFlush");
    if (pCcCopyWriteWontFlush == NULL) { Missing = TRUE; trace("CcCopyWriteWontFlush is not exported\n"); }
    pFsRtlCurrentOplockH = GetExport(L"FsRtlCurrentOplockH");
    if (pFsRtlCurrentOplockH == NULL) { Missing = TRUE; trace("FsRtlCurrentOplockH is not exported\n"); }
    pFsRtlRegisterUncProviderEx = GetExport(L"FsRtlRegisterUncProviderEx");
    if (pFsRtlRegisterUncProviderEx == NULL) { Missing = TRUE; trace("FsRtlRegisterUncProviderEx is not exported\n"); }
    pIoCreateFileEx = GetExport(L"IoCreateFileEx");
    if (pIoCreateFileEx == NULL) { Missing = TRUE; trace("IoCreateFileEx is not exported\n"); }
    pRtlRunOnceExecuteOnce = GetExport(L"RtlRunOnceExecuteOnce");
    if (pRtlRunOnceExecuteOnce == NULL) { Missing = TRUE; trace("RtlRunOnceExecuteOnce is not exported\n"); }
    if (Missing)
    {
        skip(FALSE, "Exports missing\n");
        return;
    }

    TestEcp();
    TestEcpIrp();
    TestReparse();
    TestRunOnce();
    TestMisc();
}
