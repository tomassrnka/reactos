/*
 * PROJECT:     ReactOS kernel tests
 * LICENSE:     MIT
 * PURPOSE:     Observe the i386 syscall frame link after context services
 */
#include <kmt_test.h>

static PKTHREAD TestThread;
static PKTRAP_FRAME SavedLink;

static NTSTATUS
TestMessage(PDEVICE_OBJECT DeviceObject, ULONG Code, PVOID Buffer,
            SIZE_T InLength, PSIZE_T OutLength)
{
#ifdef _M_IX86
    PKTHREAD Thread = KeGetCurrentThread();
    /* The current IOCTL's syscall frame links to the value that was stored
     * in KTHREAD.TrapFrame while the caller was in user mode. Never dereference
     * the link: the buggy kernel may have stored arbitrary user EDX there. */
    PKTRAP_FRAME Link = (PKTRAP_FRAME)Thread->TrapFrame->Edx;
    if (Code == 1)
    {
        if (TestThread) ObDereferenceObject(TestThread);
        TestThread = Thread;
        ObReferenceObject(TestThread);
        SavedLink = Link;
    }
    else if (Code == 2)
    {
        ok_eq_pointer(Thread, TestThread);
        if (Thread == TestThread) ok_eq_pointer(Link, SavedLink);
    }
    else return STATUS_INVALID_DEVICE_REQUEST;
    return STATUS_SUCCESS;
#else
    return STATUS_NOT_SUPPORTED;
#endif
}

NTSTATUS
TestEntry(PDRIVER_OBJECT DriverObject, PCUNICODE_STRING RegistryPath,
          PCWSTR *DeviceName, INT *Flags)
{
    *DeviceName = L"KeTrapFrame";
    KmtRegisterMessageHandler(0, NULL, TestMessage);
    return STATUS_SUCCESS;
}

VOID
TestUnload(PDRIVER_OBJECT DriverObject)
{
    if (TestThread) ObDereferenceObject(TestThread);
}
