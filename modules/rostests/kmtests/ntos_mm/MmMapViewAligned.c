/*
 * PROJECT:     ReactOS kernel tests
 * LICENSE:     MIT
 * PURPOSE:     Explicit section bases are limited after mapping alignment
 */

#include <kmt_test.h>

START_TEST(MmMapViewAligned)
{
    LARGE_INTEGER Size, Offset;
    PVOID Section = NULL, Base;
    SIZE_T ViewSize;
    NTSTATUS Status;
    ULONG i, AllocationType;

    Size.QuadPart = PAGE_SIZE;
    Status = MmCreateSection(&Section, SECTION_ALL_ACCESS, NULL, &Size,
                             PAGE_READWRITE, SEC_COMMIT, NULL, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    for (i = 0; i < 2; ++i)
    {
        AllocationType = 0;
#ifdef _M_IX86
        if (i) AllocationType = MEM_DOS_LIM;
#else
        if (i) break;
#endif
        /* ZeroBits=6 caps the view at 0x03ffffff on both architectures.
         * The input's page-rounded end exceeds that cap by one byte, but
         * the mapping's aligned base and size fit. NtMapViewOfSection would
         * reject the unaligned input before it reaches this kernel API. */
        Base = (PVOID)0x03fff001;
        ViewSize = PAGE_SIZE;
        Offset.QuadPart = 0;
        Status = MmMapViewOfSection(Section, PsGetCurrentProcess(), &Base,
                                   6, 0, &Offset, &ViewSize, ViewUnmap,
                                   AllocationType, PAGE_READWRITE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok_eq_pointer(Base, i ? (PVOID)0x03fff000 : (PVOID)0x03ff0000);
            ok_eq_size(ViewSize, PAGE_SIZE);
            Status = MmUnmapViewOfSection(PsGetCurrentProcess(), Base);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
    }
    ObDereferenceObject(Section);
}
