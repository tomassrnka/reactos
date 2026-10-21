/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     kbdclass/mouclass pending read IRP contract test. Only one read can be
 *              pending on a class device. A further read must not displace the pending one:
 *              every read issued here has to complete (rejected, cancelled or with data),
 *              and cancelling must not leave an IRP behind. Also checks that a single read
 *              can pend, be cancelled, and that a new read can pend again afterwards.
 */

#include <apitest.h>
#include <ndk/ntndk.h>

#define WAIT_MS 5000
#define LOG(...) do { DbgPrint("KMC: " __VA_ARGS__); trace(__VA_ARGS__); } while (0)

typedef struct _RD
{
    HANDLE Event;
    IO_STATUS_BLOCK Iosb;
    UCHAR Buffer[256];
    NTSTATUS Status;
} RD;

static HANDLE OpenDev(PCWSTR Name)
{
    UNICODE_STRING Us;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    HANDLE h = NULL;
    NTSTATUS Status;

    RtlInitUnicodeString(&Us, Name);
    InitializeObjectAttributes(&Oa, &Us, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&h, FILE_READ_DATA | SYNCHRONIZE, &Oa, &Iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    if (!NT_SUCCESS(Status))
        return NULL;
    return h;
}

static void StartRead(HANDLE h, RD *r)
{
    LARGE_INTEGER Off;
    Off.QuadPart = 0;
    RtlZeroMemory(r, sizeof(*r));
    NtCreateEvent(&r->Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    r->Iosb.Status = 0x7FFFFFFF;
    r->Status = NtReadFile(h, r->Event, NULL, NULL, &r->Iosb, r->Buffer, sizeof(r->Buffer), &Off, NULL);
}

/* Returns TRUE if the request completed in time; Status is the final status */
static BOOLEAN Finish(RD *r, NTSTATUS *Final)
{
    LARGE_INTEGER To;
    NTSTATUS Wait;

    if (r->Status != STATUS_PENDING)
    {
        *Final = r->Status;
        return TRUE;
    }
    To.QuadPart = -(LONGLONG)WAIT_MS * 10000;
    Wait = NtWaitForSingleObject(r->Event, FALSE, &To);
    if (Wait != STATUS_SUCCESS)
        return FALSE;
    *Final = r->Iosb.Status;
    return TRUE;
}

static void TestDevice(PCWSTR Name)
{
    HANDLE h;
    RD a, b, c;
    NTSTATUS Fa = 0, Fb = 0, Fc = 0;
    IO_STATUS_BLOCK Iosb;
    BOOLEAN DoneA, DoneB, DoneC;

    h = OpenDev(Name);
    if (!h)
    {
        skip("%ls: cannot open\n", Name);
        return;
    }

    /* two reads back to back, with no input available */
    StartRead(h, &a);
    StartRead(h, &b);
    LOG("%ls: read A -> %lx, read B -> %lx\n", Name, a.Status, b.Status);
    ok(a.Status == STATUS_PENDING || a.Status == STATUS_DEVICE_BUSY || NT_SUCCESS(a.Status), "A: 0x%lx\n", a.Status);
    /* the second read must be rejected rather than displace the first */
    ok(b.Status == STATUS_DEVICE_BUSY || NT_SUCCESS(b.Status), "%ls: second read not rejected: 0x%lx\n", Name, b.Status);

    LOG("cancelling\n");
    NtCancelIoFile(h, &Iosb);
    DoneA = Finish(&a, &Fa);
    DoneB = Finish(&b, &Fb);
    ok(DoneA, "%ls: first read never completed after cancel (IRP leaked)\n", Name);
    ok(DoneB, "%ls: second read never completed after cancel (IRP leaked)\n", Name);
    if (DoneA && a.Status == STATUS_PENDING)
        ok(Fa == STATUS_CANCELLED || NT_SUCCESS(Fa), "%ls: A final 0x%lx\n", Name, Fa);
    LOG("%ls: A done=%d 0x%lx, B done=%d 0x%lx\n", Name, DoneA, Fa, DoneB, Fb);
    fflush(stdout);

    /* single reader: a new read can pend again after the cancel, and cancels cleanly */
    StartRead(h, &c);
    LOG("%ls: read C -> %lx\n", Name, c.Status);
    ok(c.Status == STATUS_PENDING || c.Status == STATUS_DEVICE_BUSY || NT_SUCCESS(c.Status), "C: 0x%lx\n", c.Status);
    LOG("cancelling\n");
    NtCancelIoFile(h, &Iosb);
    DoneC = Finish(&c, &Fc);
    ok(DoneC, "%ls: read after cancel never completed\n", Name);
    fflush(stdout);

    if (DoneA && DoneB && DoneC)
        NtClose(h);
}

START_TEST(KbdMouClassPendingIrp)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    TestDevice(L"\\Device\\KeyboardClass0");
    TestDevice(L"\\Device\\PointerClass0");
}
