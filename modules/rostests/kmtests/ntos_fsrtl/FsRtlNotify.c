/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Contract tests for the FsRtl change-notify package: report contents and
 *              sizes, cancel-versus-cleanup ownership, and completed-IRP handling
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define TAG_NTFY 'yNmK'
#define IRP_BUFFER_SIZE 1024
#define IRP_NOTIFY_LENGTH 512
#define CANARY 0xCD

/* The package keeps its mutex in the NOTIFY_SYNC it hands out; the race test needs
 * to hold that mutex like a file system driver in the middle of a report would. */
typedef struct _MIRROR_NOTIFY_SYNC
{
    FAST_MUTEX FastMutex;
    ULONG_PTR OwningThread;
    ULONG OwnerCount;
} MIRROR_NOTIFY_SYNC, *PMIRROR_NOTIFY_SYNC;

typedef struct _TEST_IRP
{
    PIRP Irp;
    PUCHAR Buffer;
    PFILE_OBJECT FileObject;
    LONG Calls;
    NTSTATUS Status;
    ULONG_PTR Information;
} TEST_IRP, *PTEST_IRP;

/* The pre-Vista report interface is exported but not declared by the DDK headers */
NTKERNELAPI
VOID
NTAPI
FsRtlNotifyReportChange(IN PNOTIFY_SYNC NotifySync,
                        IN PLIST_ENTRY NotifyList,
                        IN PSTRING FullTargetName,
                        IN PUSHORT FileNamePartLength,
                        IN ULONG FilterMatch);

static PNOTIFY_SYNC Sync;
static LIST_ENTRY NotifyList;
static LIST_ENTRY OemNotifyList;
static ULONG KeyA, KeyB;

static STRING DirName;
static STRING OemDirName;

static
NTSTATUS
NTAPI
NotifyCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    PTEST_IRP Test = Context;

    InterlockedIncrement(&Test->Calls);
    Test->Status = Irp->IoStatus.Status;
    Test->Information = Irp->IoStatus.Information;
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
PFILE_OBJECT
AllocateFileObject(PVOID Context)
{
    PFILE_OBJECT FileObject = ExAllocatePoolWithTag(NonPagedPool, sizeof(*FileObject), TAG_NTFY);

    if (FileObject)
    {
        RtlZeroMemory(FileObject, sizeof(*FileObject));
        FileObject->Type = IO_TYPE_FILE;
        FileObject->Size = sizeof(*FileObject);
        FileObject->FsContext = Context;
    }
    return FileObject;
}

static
BOOLEAN
BuildNotifyIrp(PTEST_IRP Test, PFILE_OBJECT FileObject)
{
    PIO_STACK_LOCATION Stack;

    RtlZeroMemory(Test, sizeof(*Test));
    Test->FileObject = FileObject;
    Test->Buffer = ExAllocatePoolWithTag(NonPagedPool, IRP_BUFFER_SIZE, TAG_NTFY);
    Test->Irp = IoAllocateIrp(2, FALSE);
    if (!Test->Buffer || !Test->Irp)
    {
        if (Test->Buffer) ExFreePoolWithTag(Test->Buffer, TAG_NTFY);
        if (Test->Irp) IoFreeIrp(Test->Irp);
        Test->Buffer = NULL;
        Test->Irp = NULL;
        return FALSE;
    }
    RtlFillMemory(Test->Buffer, IRP_BUFFER_SIZE, CANARY);

    Test->Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Test->Irp->RequestorMode = KernelMode;
    Test->Irp->AssociatedIrp.SystemBuffer = Test->Buffer;
    IoSetNextIrpStackLocation(Test->Irp);
    Stack = IoGetCurrentIrpStackLocation(Test->Irp);
    Stack->MajorFunction = IRP_MJ_DIRECTORY_CONTROL;
    Stack->MinorFunction = IRP_MN_NOTIFY_CHANGE_DIRECTORY;
    Stack->FileObject = FileObject;
    Stack->Parameters.NotifyDirectory.Length = IRP_NOTIFY_LENGTH;
    Stack->CompletionRoutine = NotifyCompletion;
    Stack->Context = Test;
    Stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;
    return TRUE;
}

static
VOID
FreeNotifyIrp(PTEST_IRP Test)
{
    if (Test->Irp) IoFreeIrp(Test->Irp);
    if (Test->Buffer) ExFreePoolWithTag(Test->Buffer, TAG_NTFY);
    Test->Irp = NULL;
    Test->Buffer = NULL;
}

static
VOID
Watch(PTEST_IRP Test, PLIST_ENTRY List, PSTRING Name, BOOLEAN WatchTree)
{
    FsRtlNotifyFullChangeDirectory(Sync,
                                   List,
                                   Test->FileObject->FsContext,
                                   Name,
                                   WatchTree,
                                   FALSE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                   Test->Irp,
                                   NULL,
                                   NULL);
}

static
ULONG
WLen(PCWSTR String)
{
    ULONG Length = 0;

    while (String[Length]) Length++;
    return Length;
}

static
VOID
ReportWide(PLIST_ENTRY List, PCWSTR FullName, ULONG OffsetChars, PCWSTR Stream, ULONG Action)
{
    STRING Full, StreamString;

    Full.Buffer = (PCHAR)FullName;
    Full.Length = Full.MaximumLength = (USHORT)(WLen(FullName) * sizeof(WCHAR));
    StreamString.Buffer = (PCHAR)Stream;
    if (Stream)
        StreamString.Length = StreamString.MaximumLength = (USHORT)(WLen(Stream) * sizeof(WCHAR));
    FsRtlNotifyFullReportChange(Sync, List, &Full, (USHORT)(OffsetChars * sizeof(WCHAR)),
                                Stream ? &StreamString : NULL, NULL,
                                FILE_NOTIFY_CHANGE_FILE_NAME, Action, NULL);
}

static
VOID
ReportOem(PLIST_ENTRY List, PCSTR FullName, ULONG Offset, PCSTR Stream, ULONG Action)
{
    STRING Full, StreamString;

    Full.Buffer = (PCHAR)FullName;
    Full.Length = Full.MaximumLength = (USHORT)strlen(FullName);
    StreamString.Buffer = (PCHAR)Stream;
    if (Stream)
        StreamString.Length = StreamString.MaximumLength = (USHORT)strlen(Stream);
    FsRtlNotifyFullReportChange(Sync, List, &Full, (USHORT)Offset,
                                Stream ? &StreamString : NULL, NULL,
                                FILE_NOTIFY_CHANGE_FILE_NAME, Action, NULL);
}

/* One FILE_NOTIFY_INFORMATION at Offset of the IRP's buffer must read: Action, Name, Next */
static
BOOLEAN
EntryIs(PTEST_IRP Test, ULONG Offset, ULONG Action, PCWSTR Name, ULONG Next)
{
    PFILE_NOTIFY_INFORMATION Entry = (PVOID)(Test->Buffer + Offset);
    ULONG Length = WLen(Name) * sizeof(WCHAR);

    return Entry->Action == Action &&
           Entry->NextEntryOffset == Next &&
           Entry->FileNameLength == Length &&
           RtlCompareMemory(Entry->FileName, Name, Length) == Length;
}

/* Nothing may be written behind what the completion says was returned */
static
BOOLEAN
CanaryIntact(PTEST_IRP Test)
{
    ULONG i;

    for (i = (ULONG)Test->Information; i < IRP_BUFFER_SIZE; i++)
    {
        if (Test->Buffer[i] != CANARY)
            return FALSE;
    }
    return TRUE;
}

/* A report that must reach the single queued IRP as one entry */
static
VOID
ExpectOneEntry(PTEST_IRP Test, PCWSTR ExpectedName, ULONG Action, PCSTR What)
{
    ULONG Expected = FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName) + WLen(ExpectedName) * sizeof(WCHAR);

    ok(Test->Calls == 1, "%s: completed %ld times\n", What, Test->Calls);
    ok(Test->Status == STATUS_SUCCESS, "%s: status 0x%lx\n", What, Test->Status);
    ok(Test->Information == Expected, "%s: returned %Iu, expected %lu\n", What, Test->Information, Expected);
    ok(EntryIs(Test, 0, Action, ExpectedName, 0), "%s: wrong entry\n", What);
    ok(CanaryIntact(Test), "%s: wrote past the returned length\n", What);
}

static
VOID
TestReports(VOID)
{
    PFILE_OBJECT Fo;
    TEST_IRP Test;
    LONG Cancelled;
    USHORT PartLength;
    STRING Full;

    Fo = AllocateFileObject(&KeyA);
    if (skip(Fo != NULL, "No file object\n"))
        return;

    /* Unicode, direct child */
    if (!skip(BuildNotifyIrp(&Test, Fo), "No IRP\n"))
    {
        Watch(&Test, &NotifyList, &DirName, TRUE);
        ok_eq_long(Test.Calls, 0L);
        ReportWide(&NotifyList, L"\\Dir\\file.txt", 5, NULL, FILE_ACTION_ADDED);
        ExpectOneEntry(&Test, L"file.txt", FILE_ACTION_ADDED, "wide child");
        FreeNotifyIrp(&Test);
    }

    /* Unicode, deeper in the watched tree: the parent part is included */
    if (!skip(BuildNotifyIrp(&Test, Fo), "No IRP\n"))
    {
        Watch(&Test, &NotifyList, &DirName, TRUE);
        ReportWide(&NotifyList, L"\\Dir\\Sub\\a.txt", 9, NULL, FILE_ACTION_REMOVED);
        ExpectOneEntry(&Test, L"Sub\\a.txt", FILE_ACTION_REMOVED, "wide tree");
        FreeNotifyIrp(&Test);
    }

    /* Unicode, with a stream name: "name:stream" */
    if (!skip(BuildNotifyIrp(&Test, Fo), "No IRP\n"))
    {
        Watch(&Test, &NotifyList, &DirName, TRUE);
        ReportWide(&NotifyList, L"\\Dir\\file.txt", 5, L"str", FILE_ACTION_MODIFIED);
        ExpectOneEntry(&Test, L"file.txt:str", FILE_ACTION_MODIFIED, "wide stream");
        FreeNotifyIrp(&Test);
    }

    /* A final component longer than the name it belongs to is the FSD's error: nothing
     * is reported, nothing is read from outside the name */
    if (!skip(BuildNotifyIrp(&Test, Fo), "No IRP\n"))
    {
        Watch(&Test, &NotifyList, &DirName, TRUE);
        Full.Buffer = (PCHAR)L"\\Dir\\file.txt";
        Full.Length = Full.MaximumLength = (USHORT)(WLen(L"\\Dir\\file.txt") * sizeof(WCHAR));
        PartLength = Full.Length + 40;
        FsRtlNotifyReportChange(Sync, &NotifyList, &Full, &PartLength, FILE_NOTIFY_CHANGE_FILE_NAME);
        ok(Test.Calls == 0, "oversized part length completed the IRP (%ld)\n", Test.Calls);
        /* An offset behind the end of the name, through the full interface */
        FsRtlNotifyFullReportChange(Sync, &NotifyList, &Full, (USHORT)(Full.Length + 8), NULL, NULL,
                                    FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_ADDED, NULL);
        ok(Test.Calls == 0, "offset behind the name completed the IRP (%ld)\n", Test.Calls);
        /* The watcher still works afterwards */
        ReportWide(&NotifyList, L"\\Dir\\ok.txt", 5, NULL, FILE_ACTION_ADDED);
        ExpectOneEntry(&Test, L"ok.txt", FILE_ACTION_ADDED, "after rejected reports");
        FreeNotifyIrp(&Test);
    }

    /* Plain cancel of a queued IRP; the watcher then goes away with the handle */
    if (!skip(BuildNotifyIrp(&Test, Fo), "No IRP\n"))
    {
        Watch(&Test, &NotifyList, &DirName, TRUE);
        Cancelled = IoCancelIrp(Test.Irp);
        ok(Cancelled, "IoCancelIrp failed\n");
        ok_eq_long(Test.Calls, 1L);
        ok_eq_hex(Test.Status, STATUS_CANCELLED);
        FreeNotifyIrp(&Test);
    }
    FsRtlNotifyCleanup(Sync, &NotifyList, &KeyA);
    ok(IsListEmpty(&NotifyList), "watcher left on the list after cleanup\n");

    ExFreePoolWithTag(Fo, TAG_NTFY);
}

static
VOID
TestOemReports(VOID)
{
    PFILE_OBJECT Fo;
    TEST_IRP Test;

    Fo = AllocateFileObject(&KeyB);
    if (skip(Fo != NULL, "No file object\n"))
        return;

    /* A watcher whose names are OEM strings: reports come back as Unicode */
    if (!skip(BuildNotifyIrp(&Test, Fo), "No IRP\n"))
    {
        Watch(&Test, &OemNotifyList, &OemDirName, TRUE);
        ReportOem(&OemNotifyList, "\\Dir\\c.txt", 5, NULL, FILE_ACTION_ADDED);
        ExpectOneEntry(&Test, L"c.txt", FILE_ACTION_ADDED, "oem child");
        FreeNotifyIrp(&Test);
    }
    if (!skip(BuildNotifyIrp(&Test, Fo), "No IRP\n"))
    {
        Watch(&Test, &OemNotifyList, &OemDirName, TRUE);
        ReportOem(&OemNotifyList, "\\Dir\\Sub\\b.txt", 9, NULL, FILE_ACTION_ADDED);
        ExpectOneEntry(&Test, L"Sub\\b.txt", FILE_ACTION_ADDED, "oem tree");
        FreeNotifyIrp(&Test);
    }
    /* The stream name is the one that is appended: shorter and longer than the file name */
    if (!skip(BuildNotifyIrp(&Test, Fo), "No IRP\n"))
    {
        Watch(&Test, &OemNotifyList, &OemDirName, TRUE);
        ReportOem(&OemNotifyList, "\\Dir\\longname.txt", 5, "s", FILE_ACTION_MODIFIED);
        ExpectOneEntry(&Test, L"longname.txt:s", FILE_ACTION_MODIFIED, "oem stream (short)");
        FreeNotifyIrp(&Test);
    }
    if (!skip(BuildNotifyIrp(&Test, Fo), "No IRP\n"))
    {
        Watch(&Test, &OemNotifyList, &OemDirName, TRUE);
        ReportOem(&OemNotifyList, "\\Dir\\a", 5, "longstream", FILE_ACTION_MODIFIED);
        ExpectOneEntry(&Test, L"a:longstream", FILE_ACTION_MODIFIED, "oem stream (long)");
        FreeNotifyIrp(&Test);
    }

    FsRtlNotifyCleanup(Sync, &OemNotifyList, &KeyB);
    ok(IsListEmpty(&OemNotifyList), "oem watcher left on the list after cleanup\n");
    ExFreePoolWithTag(Fo, TAG_NTFY);
}

/* A cancelled IRP whose buffer holds the pending data hands the data to what is left */
static
VOID
TestCancelKeepsPendingData(VOID)
{
    PFILE_OBJECT Fo;
    TEST_IRP A, B;
    ULONG Next = ((FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName) + 7 * sizeof(WCHAR)) + 3) & ~3UL;
    ULONG Total = Next + FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName) + 7 * sizeof(WCHAR);

    RtlZeroMemory(&A, sizeof(A));
    RtlZeroMemory(&B, sizeof(B));
    Fo = AllocateFileObject(&KeyA);
    if (skip(Fo != NULL, "No file object\n"))
        return;

    /* Another IRP is queued behind the cancelled one */
    if (!skip(BuildNotifyIrp(&A, Fo) && BuildNotifyIrp(&B, Fo), "No IRPs\n"))
    {
        Watch(&A, &NotifyList, &DirName, TRUE);
        Watch(&B, &NotifyList, &DirName, TRUE);
        /* The first half of a rename is held back until the new name is known */
        ReportWide(&NotifyList, L"\\Dir\\old.txt", 5, NULL, FILE_ACTION_RENAMED_OLD_NAME);
        ok_eq_long(A.Calls, 0L);
        ok_eq_long(B.Calls, 0L);
        ok(IoCancelIrp(A.Irp), "IoCancelIrp failed\n");
        ok_eq_long(A.Calls, 1L);
        ok_eq_hex(A.Status, STATUS_CANCELLED);
        ReportWide(&NotifyList, L"\\Dir\\new.txt", 5, NULL, FILE_ACTION_RENAMED_NEW_NAME);
        ok_eq_long(B.Calls, 1L);
        ok_eq_hex(B.Status, STATUS_SUCCESS);
        ok(B.Information == Total, "returned %Iu, expected %lu\n", B.Information, Total);
        ok(EntryIs(&B, 0, FILE_ACTION_RENAMED_OLD_NAME, L"old.txt", Next), "old name lost with the cancelled IRP\n");
        ok(EntryIs(&B, Next, FILE_ACTION_RENAMED_NEW_NAME, L"new.txt", 0), "new name entry wrong\n");
        ok(CanaryIntact(&B), "wrote past the returned length\n");
    }
    if (A.Irp) FreeNotifyIrp(&A);
    if (B.Irp) FreeNotifyIrp(&B);
    FsRtlNotifyCleanup(Sync, &NotifyList, &KeyA);
    ok(IsListEmpty(&NotifyList), "watcher left on the list\n");

    /* Nothing queued behind the cancelled one: the data is kept for the next IRP */
    if (!skip(BuildNotifyIrp(&A, Fo) && BuildNotifyIrp(&B, Fo), "No IRPs\n"))
    {
        Watch(&A, &NotifyList, &DirName, TRUE);
        ReportWide(&NotifyList, L"\\Dir\\old.txt", 5, NULL, FILE_ACTION_RENAMED_OLD_NAME);
        ok(IoCancelIrp(A.Irp), "IoCancelIrp failed\n");
        ok_eq_long(A.Calls, 1L);
        Watch(&B, &NotifyList, &DirName, TRUE);
        ok_eq_long(B.Calls, 0L);
        ReportWide(&NotifyList, L"\\Dir\\new.txt", 5, NULL, FILE_ACTION_RENAMED_NEW_NAME);
        ok_eq_long(B.Calls, 1L);
        ok_eq_hex(B.Status, STATUS_SUCCESS);
        ok(B.Information == Total, "returned %Iu, expected %lu\n", B.Information, Total);
        ok(EntryIs(&B, 0, FILE_ACTION_RENAMED_OLD_NAME, L"old.txt", Next), "old name lost\n");
        ok(EntryIs(&B, Next, FILE_ACTION_RENAMED_NEW_NAME, L"new.txt", 0), "new name entry wrong\n");
    }
    if (A.Irp) FreeNotifyIrp(&A);
    if (B.Irp) FreeNotifyIrp(&B);
    FsRtlNotifyCleanup(Sync, &NotifyList, &KeyA);
    ok(IsListEmpty(&NotifyList), "watcher left on the list\n");

    ExFreePoolWithTag(Fo, TAG_NTFY);
}

/* Cancel against cleanup. The cancel routine of IRP A is in flight (it dropped its lock
 * and waits for the notify mutex) when the handle is cleaned up. */

typedef struct _CANCEL_WORKER
{
    PIRP Irp;
    KEVENT Done;
} CANCEL_WORKER, *PCANCEL_WORKER;

static
VOID
NTAPI
CancelWorker(PVOID Context)
{
    PCANCEL_WORKER Worker = Context;

    IoCancelIrp(Worker->Irp);
    KeSetEvent(&Worker->Done, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
HoldNotifyMutex(PNOTIFY_SYNC NotifySync)
{
    PMIRROR_NOTIFY_SYNC Mirror = (PMIRROR_NOTIFY_SYNC)NotifySync;

    KeEnterCriticalRegion();
    ExAcquireFastMutexUnsafe(&Mirror->FastMutex);
    Mirror->OwningThread = (ULONG_PTR)KeGetCurrentThread();
    Mirror->OwnerCount = 1;
}

static
VOID
ReleaseNotifyMutex(PNOTIFY_SYNC NotifySync)
{
    PMIRROR_NOTIFY_SYNC Mirror = (PMIRROR_NOTIFY_SYNC)NotifySync;

    Mirror->OwnerCount = 0;
    Mirror->OwningThread = 0;
    ExReleaseFastMutexUnsafe(&Mirror->FastMutex);
    KeLeaveCriticalRegion();
}

static
VOID
TestCancelAgainstCleanup(VOID)
{
    PFILE_OBJECT Fo, FoLate;
    TEST_IRP A, B, C;
    CANCEL_WORKER Worker;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Handle;
    PVOID Thread = NULL;
    LARGE_INTEGER Delay, Timeout;
    NTSTATUS Status;
    ULONG i;
    BOOLEAN Held = FALSE;

    Fo = AllocateFileObject(&KeyA);
    FoLate = AllocateFileObject(&KeyA);
    if (skip(Fo && FoLate, "No file objects\n"))
        return;
    if (skip(BuildNotifyIrp(&A, Fo) && BuildNotifyIrp(&B, Fo) && BuildNotifyIrp(&C, FoLate), "No IRPs\n"))
        return;

    Watch(&A, &NotifyList, &DirName, TRUE);
    Watch(&B, &NotifyList, &DirName, TRUE);

    HoldNotifyMutex(Sync);
    Held = TRUE;

    /* The cancel of A starts: it takes A's cancel routine away and waits on the mutex */
    Worker.Irp = A.Irp;
    KeInitializeEvent(&Worker.Done, NotificationEvent, FALSE);
    InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, &Attributes, NULL, NULL, CancelWorker, &Worker);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = ObReferenceObjectByHandle(Handle, SYNCHRONIZE, *PsThreadType, KernelMode, &Thread, NULL);
        ZwClose(Handle);
    }
    if (skip(NT_SUCCESS(Status) && Thread != NULL, "No worker thread\n"))
    {
        ReleaseNotifyMutex(Sync);
        return;
    }

    Delay.QuadPart = -10 * 1000 * 10; /* 10 ms */
    for (i = 0; i < 1000 && !(*(volatile BOOLEAN *)&A.Irp->Cancel && *(PVOID volatile *)&A.Irp->CancelRoutine == NULL); i++)
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    ok(A.Irp->Cancel && A.Irp->CancelRoutine == NULL, "the cancel of A did not start\n");

    /* The handle is closed meanwhile: B completes, A is not the cleanup's to complete */
    FsRtlNotifyCleanup(Sync, &NotifyList, &KeyA);
    ok_eq_long(A.Calls, 0L);
    ok_eq_long(B.Calls, 1L);
    ok_eq_hex(B.Status, STATUS_NOTIFY_CLEANUP);

    /* A new request on the handle that is being cleaned up is completed once, and is
     * not queued behind it */
    Watch(&C, &NotifyList, &DirName, TRUE);
    ok_eq_long(C.Calls, 1L);
    ok_eq_hex(C.Status, STATUS_NOTIFY_CLEANUP);
    ok(C.Irp->CancelRoutine == NULL, "the completed IRP was queued again\n");

    /* The change of a closed handle's directory must not reach the dying watcher */
    ReportWide(&NotifyList, L"\\Dir\\file.txt", 5, NULL, FILE_ACTION_ADDED);

    ReleaseNotifyMutex(Sync);
    Held = FALSE;

    Timeout.QuadPart = -10 * 1000 * 1000 * 20; /* 20 s */
    Status = KeWaitForSingleObject(&Worker.Done, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ObDereferenceObject(Thread);
    if (Status != STATUS_SUCCESS)
    {
        trace("cancel never finished, leaking the test state\n");
        return;
    }

    ok_eq_long(A.Calls, 1L);
    ok_eq_hex(A.Status, STATUS_CANCELLED);
    ok_eq_long(B.Calls, 1L);
    ok_eq_long(C.Calls, 1L);

    /* The last reference went with the cancel: the notification has to be off the list */
    if (!IsListEmpty(&NotifyList))
    {
        ok(FALSE, "a freed watcher is still on the FSD's list\n");
        /* Do not let the stale entry outlive the test */
        InitializeListHead(&NotifyList);
    }

    if (Held) ReleaseNotifyMutex(Sync);
    FreeNotifyIrp(&A);
    FreeNotifyIrp(&B);
    FreeNotifyIrp(&C);
    ExFreePoolWithTag(Fo, TAG_NTFY);
    ExFreePoolWithTag(FoLate, TAG_NTFY);
}

START_TEST(FsRtlNotify)
{
    DirName.Buffer = (PCHAR)L"\\Dir";
    DirName.Length = DirName.MaximumLength = 4 * sizeof(WCHAR);
    OemDirName.Buffer = "\\Dir";
    OemDirName.Length = OemDirName.MaximumLength = 4;
    InitializeListHead(&NotifyList);
    InitializeListHead(&OemNotifyList);

    _SEH2_TRY
    {
        FsRtlNotifyInitializeSync(&Sync);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Sync = NULL;
    }
    _SEH2_END;
    if (skip(Sync != NULL, "No notify sync\n"))
        return;

    /* File systems call the package inside a critical region (it takes its mutex unsafely) */
    KeEnterCriticalRegion();
    TestReports();
    TestOemReports();
    TestCancelKeepsPendingData();
    TestCancelAgainstCleanup();
    KeLeaveCriticalRegion();

    FsRtlNotifyUninitializeSync(&Sync);
}
