/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         HidParser description test
 * PROGRAMMER:      Thomas Faber <thomas.faber@reactos.org>
 */

#include <kmt_test.h>
#include <hidpddi.h>

#define NDEBUG
#include <debug.h>

#include "HidP.h"

static UCHAR ExampleKeyboardDescriptor[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop), */
    0x09, 0x06,       /* Usage (Keyboard), */
    0xA1, 0x01,       /* Collection (Application), */
    0x05, 0x07,       /*   Usage Page (Key Codes); */
    0x19, 0xE0,       /*   Usage Minimum (224), */
    0x29, 0xE7,       /*   Usage Maximum (231), */
    0x15, 0x00,       /*   Logical Minimum (0), */
    0x25, 0x01,       /*   Logical Maximum (1), */
    0x75, 0x01,       /*   Report Size (1), */
    0x95, 0x08,       /*   Report Count (8), */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute), ;Modifier byte */
    0x95, 0x01,       /*   Report Count (1), */
    0x75, 0x08,       /*   Report Size (8), */
    0x81, 0x01,       /*   Input (Constant), ;Reserved byte */
    0x95, 0x05,       /*   Report Count (5), */
    0x75, 0x01,       /*   Report Size (1), */
    0x05, 0x08,       /*   Usage Page (Page# for LEDs), */
    0x19, 0x01,       /*   Usage Minimum (1), */
    0x29, 0x05,       /*   Usage Maximum (5), */
    0x91, 0x02,       /*   Output (Data, Variable, Absolute), ;LED report */
    0x95, 0x01,       /*   Report Count (1), */
    0x75, 0x03,       /*   Report Size (3), */
    0x91, 0x01,       /*   Output (Constant), ;LED report padding */
    0x95, 0x06,       /*   Report Count (6), */
    0x75, 0x08,       /*   Report Size (8), */
    0x15, 0x00,       /*   Logical Minimum (0), */
    0x25, 0x65,       /*   Logical Maximum (101), */
    0x05, 0x07,       /*   Usage Page (Key Codes), */
    0x19, 0x00,       /*   Usage Minimum (0), */
    0x29, 0x65,       /*   Usage Maximum (101) */
    0x81, 0x00,       /*   Input (Data, Array), ;Key arrays (6 bytes) */
    0xC0              /* End Collection */
};

static UCHAR PowerProEliteDescriptor[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop), */
    0x09, 0x04,       /* Usage (Joystick), */
    0xa1, 0x01,       /* Collection (Application), */
    0xa1, 0x02,       /*   Collection (Logical), */
    0x85, 0x01,       /*     Report ID (1) */
    0x75, 0x08,       /*     Report Size (8), */
    0x95, 0x01,       /*     Report Count (1), */
    0x15, 0x00,       /*     Logical Minimum (0), */
    0x26, 0xff, 0x00, /*     Logical Maximum (255), */
    0x81, 0x03,       /*     Input (Constant, Variable, Absolute), */
    0x75, 0x01,       /*     Report Size (1), */
    0x95, 0x13,       /*     Report Count (19), */
    0x15, 0x00,       /*     Logical Minimum (0), */
    0x25, 0x01,       /*     Logical Maximum (1), */
    0x35, 0x00,       /*     Physical Minimum (0), */
    0x45, 0x01,       /*     Physical Maximum (1), */
    0x05, 0x09,       /*     Usage Page (Button), */
    0x19, 0x01,       /*     Usage Minimum (1), */
    0x29, 0x13,       /*     Usage Maximum (19), */
    0x81, 0x02,       /*     Input (Data, Variable, Absolute), */
    0x75, 0x01,       /*     Report Size (1), */
    0x95, 0x0d,       /*     Report Count (13), */
    0x06, 0x00, 0xff, /*     Usage Page (Vendor-defined FF00), */
    0x81, 0x03,       /*     Input (Constant, Variable, Absolute), */
    0x15, 0x00,       /*     Logical Minimum (0), */
    0x26, 0xff, 0x00, /*     Logical Maximum (255), */
    0x05, 0x01,       /*     Usage Page (Generic Desktop), */
    0x09, 0x01,       /*     Usage (Pointer), */
    0xa1, 0x00,       /*     Collection (Physical), */
    0x75, 0x08,       /*       Report Size (8), */
    0x95, 0x04,       /*       Report Count (4), */
    0x35, 0x00,       /*       Physical Minimum (0), */
    0x46, 0xff, 0x00, /*       Physical Maximum (255), */
    0x09, 0x30,       /*       Usage (X), */
    0x09, 0x31,       /*       Usage (Y), */
    0x09, 0x32,       /*       Usage (Z), */
    0x09, 0x35,       /*       Usage (Rz), */
    0x81, 0x02,       /*       Input (Data, Variable, Absolute), */
    0xc0,             /*     End Collection */
    0x05, 0x01,       /*     Usage Page (Generic Desktop), */
    0x75, 0x08,       /*     Report Size (8), */
    0x95, 0x27,       /*     Report Count (39), */
    0x09, 0x01,       /*     Usage (Pointer), */
    0x81, 0x02,       /*     Input (Data, Variable, Absolute), */
    0x75, 0x08,       /*     Report Size (8), */
    0x95, 0x30,       /*     Report Count (48), */
    0x09, 0x01,       /*     Usage (Pointer), */
    0x91, 0x02,       /*     Output (Data, Variable, Absolute), */
    0x75, 0x08,       /*     Report Size (8), */
    0x95, 0x30,       /*     Report Count (48), */
    0x09, 0x01,       /*     Usage (Pointer), */
    0xb1, 0x02,       /*     Feature (Data, Variable, Absolute), */
    0xc0,             /*   End Collection */

    0xa1, 0x02,       /*   Collection (Logical), */
    0x85, 0x02,       /*     Report ID (2) */
    0x75, 0x08,       /*     Report Size (8), */
    0x95, 0x30,       /*     Report Count (48), */
    0x09, 0x01,       /*     Usage (Pointer), */
    0xb1, 0x02,       /*     Feature (Data, Variable, Absolute), */
    0xc0,             /*   End Collection */
    0xa1, 0x02,       /*   Collection (Logical), */
    0x85, 0xee,       /*     Report ID (238) */
    0x75, 0x08,       /*     Report Size (8), */
    0x95, 0x30,       /*     Report Count (48), */
    0x09, 0x01,       /*     Usage (Pointer), */
    0xb1, 0x02,       /*     Feature (Data, Variable, Absolute), */
    0xc0,             /*   End Collection */
    0xa1, 0x02,       /*   Collection (Logical), */
    0x85, 0xef,       /*     Report ID (239) */
    0x75, 0x08,       /*     Report Size (8), */
    0x95, 0x30,       /*     Report Count (48), */
    0x09, 0x01,       /*     Usage (Pointer), */
    0xb1, 0x02,       /*     Feature (Data, Variable, Absolute), */
    0xc0,             /*   End Collection */
    0xc0,             /* End Collection */
};
C_ASSERT(sizeof(PowerProEliteDescriptor) == 148);

//
// CVE-class regression inputs: untrusted report descriptors that drove
// unchecked size arithmetic, out-of-bounds reads or unbounded recursion in the
// parser. Each must fail cleanly (an error NTSTATUS, no pool corruption, no
// bugcheck) rather than succeed or crash. Run under Driver Verifier / special
// pool so the pre-fix out-of-bounds writes corrupt visibly.
//

/* Report Count 0x40000000: count * sizeof(HID_REPORT_ITEM) wrapped the
 * allocation to a few dozen bytes, then the item loop wrote past it. */
static UCHAR WrapReportCountDescriptor[] = {
    0x05, 0x01,                   /* Usage Page (Generic Desktop) */
    0x09, 0x06,                   /* Usage (Keyboard) */
    0xA1, 0x01,                   /* Collection (Application) */
    0x75, 0x08,                   /*   Report Size (8) */
    0x97, 0x00, 0x00, 0x00, 0x40, /*   Report Count (0x40000000) */
    0x09, 0x00,                   /*   Usage (0) */
    0x81, 0x02,                   /*   Input (Data,Var,Abs) */
    0xC0                          /* End Collection */
};

/* A truncated final item claims four value bytes but only one is present;
 * the parser read past the end of the descriptor buffer. */
static UCHAR TruncatedItemDescriptor[] = {
    0x05, 0x01,                   /* Usage Page (Generic Desktop) */
    0x09, 0x06,                   /* Usage (Keyboard) */
    0xA1, 0x01,                   /* Collection (Application) */
    0x27, 0xFF                    /* Logical Maximum (size 4) but only 1 byte */
};

/* Reserved long-item prefix 0xFD, descriptor ends before the long-item tag
 * byte: the parser read the nonexistent third header byte. */
static UCHAR ReservedLongItemDescriptor[] = {
    0x05, 0x01,                   /* Usage Page (Generic Desktop) */
    0x09, 0x06,                   /* Usage (Keyboard) */
    0xA1, 0x01,                   /* Collection (Application) */
    0xFD, 0x00                    /* reserved type-3 prefix + bDataSize, no tag */
};

/* 2000 report items in one collection: the full context size exceeds the
 * USHORT PreparsedDataLength, which was silently truncated on export. */
static UCHAR OversizeContextDescriptor[] = {
    0x05, 0x01,                   /* Usage Page (Generic Desktop) */
    0x09, 0x06,                   /* Usage (Keyboard) */
    0xA1, 0x01,                   /* Collection (Application) */
    0x75, 0x01,                   /*   Report Size (1) */
    0x96, 0xD0, 0x07,             /*   Report Count (2000) */
    0x09, 0x00,                   /*   Usage (0) */
    0x81, 0x02,                   /*   Input (Data,Var,Abs) */
    0xC0                          /* End Collection */
};

/* A 64-bit vendor field (some real devices use these). It must parse: the
 * parser keeps wide fields; only value extraction above 32 bits is refused. */
static UCHAR WideFieldDescriptor[] = {
    0x06, 0x00, 0xFF,             /* Usage Page (Vendor FF00) */
    0x09, 0x01,                   /* Usage (1) */
    0xA1, 0x01,                   /* Collection (Application) */
    0x75, 0x40,                   /*   Report Size (64) */
    0x95, 0x01,                   /*   Report Count (1) */
    0x09, 0x02,                   /*   Usage (2) */
    0x81, 0x02,                   /*   Input (Data,Var,Abs) */
    0xC0                          /* End Collection */
};

/* A zero-width data field must not create a report item (it would otherwise
 * produce a ByteOffset at the report end that the value APIs read past). The
 * descriptor is still valid and must parse. */
static UCHAR ZeroWidthFieldDescriptor[] = {
    0x05, 0x01,                   /* Usage Page (Generic Desktop) */
    0x09, 0x02,                   /* Usage (Mouse) */
    0xA1, 0x01,                   /* Collection (Application) */
    0x75, 0x08, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, /* one 8-bit field */
    0x75, 0x00, 0x95, 0x01, 0x09, 0x31, 0x81, 0x02, /* zero-width field */
    0xC0                          /* End Collection */
};

static
VOID
TestMalformedDescriptor(
    IN PUCHAR Descriptor,
    IN ULONG Length,
    IN PCSTR Name)
{
    NTSTATUS Status;
    HIDP_DEVICE_DESC DeviceDescription;

    //
    // poison the output so a failing call that forgets to clear it is caught
    //
    RtlFillMemory(&DeviceDescription, sizeof(DeviceDescription), 0x55);

    //
    // the only requirement is that this returns (no bugcheck / corruption)
    // and reports failure for a malformed descriptor
    //
    Status = HidP_GetCollectionDescription(Descriptor, Length, NonPagedPool, &DeviceDescription);
    ok(!NT_SUCCESS(Status), "%s: expected failure, got %lx\n", Name, Status);

    //
    // a clean failure leaves no stale pointers or lengths behind
    //
    if (!NT_SUCCESS(Status))
    {
        ok(DeviceDescription.CollectionDesc == NULL, "%s: CollectionDesc not cleared\n", Name);
        ok(DeviceDescription.CollectionDescLength == 0, "%s: CollectionDescLength not cleared\n", Name);
        ok(DeviceDescription.ReportIDs == NULL, "%s: ReportIDs not cleared\n", Name);
        ok(DeviceDescription.ReportIDsLength == 0, "%s: ReportIDsLength not cleared\n", Name);
    }
    else
    {
        //
        // if it unexpectedly succeeded, release whatever it built
        //
        HidP_FreeCollectionDescription(&DeviceDescription);
    }
}

static
VOID
TestAcceptedDescriptor(
    IN PUCHAR Descriptor,
    IN ULONG Length,
    IN PCSTR Name)
{
    NTSTATUS Status;
    HIDP_DEVICE_DESC DeviceDescription;

    RtlFillMemory(&DeviceDescription, sizeof(DeviceDescription), 0x55);
    Status = HidP_GetCollectionDescription(Descriptor, Length, NonPagedPool, &DeviceDescription);
    ok(NT_SUCCESS(Status), "%s: expected success, got %lx\n", Name, Status);
    if (NT_SUCCESS(Status))
        HidP_FreeCollectionDescription(&DeviceDescription);
}

static
VOID
TestDeepCollections(VOID)
{
    NTSTATUS Status;
    HIDP_DEVICE_DESC DeviceDescription;
    PUCHAR Descriptor;
    ULONG Length, Index, Offset;
    const ULONG Depth = 512; /* above the parser's nesting cap */

    //
    // 2 bytes of header + Depth open + Depth close collections
    //
    Length = 4 + Depth * 2 * 2;
    Descriptor = ExAllocatePoolWithTag(NonPagedPool, Length, 'tseT');
    if (skip(Descriptor != NULL, "out of memory\n"))
        return;

    Offset = 0;
    Descriptor[Offset++] = 0x05; Descriptor[Offset++] = 0x01; /* Usage Page */
    Descriptor[Offset++] = 0x09; Descriptor[Offset++] = 0x06; /* Usage */
    for (Index = 0; Index < Depth; Index++)
    {
        Descriptor[Offset++] = 0xA1; Descriptor[Offset++] = 0x01; /* Collection(App) */
    }
    for (Index = 0; Index < Depth; Index++)
    {
        Descriptor[Offset++] = 0xC0; Descriptor[Offset++] = 0x00; /* End Collection */
    }

    RtlFillMemory(&DeviceDescription, sizeof(DeviceDescription), 0x55);
    Status = HidP_GetCollectionDescription(Descriptor, Length, NonPagedPool, &DeviceDescription);
    ok(!NT_SUCCESS(Status), "DeepCollections: expected failure, got %lx\n", Status);
    if (NT_SUCCESS(Status))
        HidP_FreeCollectionDescription(&DeviceDescription);

    ExFreePoolWithTag(Descriptor, 'tseT');
}

static
VOID
TestPushOverflow(VOID)
{
    NTSTATUS Status;
    HIDP_DEVICE_DESC DeviceDescription;
    PUCHAR Descriptor;
    ULONG Length, Index, Offset;
    const ULONG Count = 512; /* above the parser's global push-stack cap */

    Length = 4 + Count;
    Descriptor = ExAllocatePoolWithTag(NonPagedPool, Length, 'tseT');
    if (skip(Descriptor != NULL, "out of memory\n"))
        return;

    Offset = 0;
    Descriptor[Offset++] = 0x05; Descriptor[Offset++] = 0x01; /* Usage Page */
    Descriptor[Offset++] = 0x09; Descriptor[Offset++] = 0x06; /* Usage */
    for (Index = 0; Index < Count; Index++)
        Descriptor[Offset++] = 0xA4; /* Push (global, size 0) */

    RtlFillMemory(&DeviceDescription, sizeof(DeviceDescription), 0x55);
    Status = HidP_GetCollectionDescription(Descriptor, Length, NonPagedPool, &DeviceDescription);
    ok(!NT_SUCCESS(Status), "PushOverflow: expected failure, got %lx\n", Status);
    if (NT_SUCCESS(Status))
        HidP_FreeCollectionDescription(&DeviceDescription);

    ExFreePoolWithTag(Descriptor, 'tseT');
}

static
VOID
TestGetCollectionDescription(VOID)
{
    NTSTATUS Status;
    HIDP_DEVICE_DESC DeviceDescription;

    /* Empty report descriptor */
    RtlFillMemory(&DeviceDescription, sizeof(DeviceDescription), 0x55);
    Status = HidP_GetCollectionDescription(NULL,
                                           0,
                                           NonPagedPool,
                                           &DeviceDescription);
    ok_eq_hex(Status, STATUS_NO_DATA_DETECTED);
    ok_eq_pointer(DeviceDescription.CollectionDesc, NULL);
    ok_eq_ulong(DeviceDescription.CollectionDescLength, 0);
    ok_eq_pointer(DeviceDescription.ReportIDs, NULL);
    ok_eq_ulong(DeviceDescription.ReportIDsLength, 0);
    if (NT_SUCCESS(Status)) HidP_FreeCollectionDescription(&DeviceDescription);

    /* Sample keyboard report descriptor from the HID spec */
    Status = HidP_GetCollectionDescription(ExampleKeyboardDescriptor,
                                           sizeof(ExampleKeyboardDescriptor),
                                           NonPagedPool,
                                           &DeviceDescription);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(DeviceDescription.CollectionDescLength, 1);
    ok_eq_ulong(DeviceDescription.ReportIDsLength, 1);
    if (!skip(NT_SUCCESS(Status), "Parsing failure\n"))
    {
        if (!skip(DeviceDescription.CollectionDescLength >= 1, "No collection\n"))
        {
            ok_eq_uint(DeviceDescription.CollectionDesc[0].UsagePage, HID_USAGE_PAGE_GENERIC);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].Usage, HID_USAGE_GENERIC_KEYBOARD);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].CollectionNumber, 1);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].InputLength, 9);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].OutputLength, 2);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].FeatureLength, 0);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].PreparsedDataLength, 476);
        }
        if (!skip(DeviceDescription.ReportIDsLength >= 1, "No report IDs\n"))
        {
            ok_eq_uint(DeviceDescription.ReportIDs[0].ReportID, 0);
            ok_eq_uint(DeviceDescription.ReportIDs[0].CollectionNumber, 1);
            ok_eq_uint(DeviceDescription.ReportIDs[0].InputLength, 8);
            ok_eq_uint(DeviceDescription.ReportIDs[0].OutputLength, 1);
            ok_eq_uint(DeviceDescription.ReportIDs[0].FeatureLength, 0);
        }
        HidP_FreeCollectionDescription(&DeviceDescription);
    }

    /* Regression test for CORE-11538 */
    Status = HidP_GetCollectionDescription(PowerProEliteDescriptor,
                                           sizeof(PowerProEliteDescriptor),
                                           NonPagedPool,
                                           &DeviceDescription);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(DeviceDescription.CollectionDescLength, 1);
    ok_eq_ulong(DeviceDescription.ReportIDsLength, 4);
    if (!skip(NT_SUCCESS(Status), "Parsing failure\n"))
    {
        if (!skip(DeviceDescription.CollectionDescLength >= 1, "No collection\n"))
        {
            ok_eq_uint(DeviceDescription.CollectionDesc[0].UsagePage, HID_USAGE_PAGE_GENERIC);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].Usage, HID_USAGE_GENERIC_JOYSTICK);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].CollectionNumber, 1);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].InputLength, 49);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].OutputLength, 49);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].FeatureLength, 49);
            ok_eq_uint(DeviceDescription.CollectionDesc[0].PreparsedDataLength, 1388);
        }
        if (!skip(DeviceDescription.ReportIDsLength >= 1, "No first report ID\n"))
        {
            ok_eq_uint(DeviceDescription.ReportIDs[0].ReportID, 1);
            ok_eq_uint(DeviceDescription.ReportIDs[0].CollectionNumber, 1);
            ok_eq_uint(DeviceDescription.ReportIDs[0].InputLength, 49);
            ok_eq_uint(DeviceDescription.ReportIDs[0].OutputLength, 49);
            ok_eq_uint(DeviceDescription.ReportIDs[0].FeatureLength, 49);
        }
        if (!skip(DeviceDescription.ReportIDsLength >= 2, "No second report ID\n"))
        {
            ok_eq_uint(DeviceDescription.ReportIDs[1].ReportID, 2);
            ok_eq_uint(DeviceDescription.ReportIDs[1].CollectionNumber, 1);
            ok_eq_uint(DeviceDescription.ReportIDs[1].InputLength, 0);
            ok_eq_uint(DeviceDescription.ReportIDs[1].OutputLength, 0);
            ok_eq_uint(DeviceDescription.ReportIDs[1].FeatureLength, 49);
        }
        if (!skip(DeviceDescription.ReportIDsLength >= 3, "No third report ID\n"))
        {
            ok_eq_uint(DeviceDescription.ReportIDs[2].ReportID, 238);
            ok_eq_uint(DeviceDescription.ReportIDs[2].CollectionNumber, 1);
            ok_eq_uint(DeviceDescription.ReportIDs[2].InputLength, 0);
            ok_eq_uint(DeviceDescription.ReportIDs[2].OutputLength, 0);
            ok_eq_uint(DeviceDescription.ReportIDs[2].FeatureLength, 49);
        }
        if (!skip(DeviceDescription.ReportIDsLength >= 4, "No fourth report ID\n"))
        {
            ok_eq_uint(DeviceDescription.ReportIDs[3].ReportID, 239);
            ok_eq_uint(DeviceDescription.ReportIDs[3].CollectionNumber, 1);
            ok_eq_uint(DeviceDescription.ReportIDs[3].InputLength, 0);
            ok_eq_uint(DeviceDescription.ReportIDs[3].OutputLength, 0);
            ok_eq_uint(DeviceDescription.ReportIDs[3].FeatureLength, 49);
        }
        HidP_FreeCollectionDescription(&DeviceDescription);
    }

    //
    // malformed descriptors: must fail cleanly, never corrupt pool or bugcheck
    //
    TestMalformedDescriptor(WrapReportCountDescriptor, sizeof(WrapReportCountDescriptor), "WrapReportCount");
    TestMalformedDescriptor(TruncatedItemDescriptor, sizeof(TruncatedItemDescriptor), "TruncatedItem");
    TestMalformedDescriptor(ReservedLongItemDescriptor, sizeof(ReservedLongItemDescriptor), "ReservedLongItem");
    TestMalformedDescriptor(OversizeContextDescriptor, sizeof(OversizeContextDescriptor), "OversizeContext");
    TestDeepCollections();
    TestPushOverflow();

    //
    // these are valid and must still parse after the hardening
    //
    TestAcceptedDescriptor(WideFieldDescriptor, sizeof(WideFieldDescriptor), "WideField");
    TestAcceptedDescriptor(ZeroWidthFieldDescriptor, sizeof(ZeroWidthFieldDescriptor), "ZeroWidthField");
}

NTSTATUS
TestHidPDescription(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG ControlCode,
    IN PVOID Buffer OPTIONAL,
    IN SIZE_T InLength,
    IN OUT PSIZE_T OutLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    PAGED_CODE();

    NT_VERIFY(ControlCode == IOCTL_TEST_DESCRIPTION);

    TestGetCollectionDescription();

    return STATUS_SUCCESS;
}
