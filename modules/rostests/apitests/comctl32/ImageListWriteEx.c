/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for ImageList_WriteEx of comctl32 version 6
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *              The first test (test_wine_case) is from Wine, dlls/comctl32/tests/imagelist.c
 *
 * The results of the argument and failure tests were measured on Windows Server 2008 R2 and Windows 10 22H2. ReactOS
 * writes the old stream format for every flag; Windows writes a newer format for the flag 0, which is not tested.
 */

#define COBJMACROS
#define CONST_VTABLE
#include "wine/test.h"
#include <windows.h>
#include <commctrl.h>
#include <ole2.h>

typedef HRESULT (WINAPI *PFN_ImageList_WriteEx)(HIMAGELIST, DWORD, IStream *);

static PFN_ImageList_WriteEx pImageList_WriteEx;

/* ------------------------------------------------------------------------------------------------------------- */
/* A stream that keeps nothing: it counts the writes and fails from the FailFrom-th one on, or only that one if Exact */

typedef struct
{
    IStream IStream_iface;
    LONG Ref;
    ULONG Writes;
    ULONG FailFrom;
    BOOL Exact;
} FAILSTREAM;

static FAILSTREAM *impl_from_IStream(IStream *iface)
{
    return CONTAINING_RECORD(iface, FAILSTREAM, IStream_iface);
}

static HRESULT WINAPI FailStream_QueryInterface(IStream *iface, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ISequentialStream) || IsEqualGUID(riid, &IID_IStream))
    {
        *out = iface;
        IStream_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI FailStream_AddRef(IStream *iface)
{
    return InterlockedIncrement(&impl_from_IStream(iface)->Ref);
}

static ULONG WINAPI FailStream_Release(IStream *iface)
{
    return InterlockedDecrement(&impl_from_IStream(iface)->Ref);
}

static HRESULT WINAPI FailStream_Read(IStream *iface, void *pv, ULONG cb, ULONG *read)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI FailStream_Write(IStream *iface, const void *pv, ULONG cb, ULONG *written)
{
    FAILSTREAM *This = impl_from_IStream(iface);

    This->Writes++;
    if (This->Exact ? This->Writes == This->FailFrom : This->Writes >= This->FailFrom)
    {
        if (written)
            *written = 0;
        return STG_E_MEDIUMFULL;
    }
    if (written)
        *written = cb;
    return S_OK;
}

static HRESULT WINAPI FailStream_Seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *newpos)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI FailStream_SetSize(IStream *iface, ULARGE_INTEGER size)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI FailStream_CopyTo(IStream *iface, IStream *stm, ULARGE_INTEGER cb, ULARGE_INTEGER *read,
                                        ULARGE_INTEGER *written)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI FailStream_Commit(IStream *iface, DWORD flags)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI FailStream_Revert(IStream *iface)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI FailStream_LockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER cb, DWORD type)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI FailStream_UnlockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER cb, DWORD type)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI FailStream_Stat(IStream *iface, STATSTG *stat, DWORD flags)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI FailStream_Clone(IStream *iface, IStream **stm)
{
    return E_NOTIMPL;
}

static const IStreamVtbl FailStreamVtbl =
{
    FailStream_QueryInterface,
    FailStream_AddRef,
    FailStream_Release,
    FailStream_Read,
    FailStream_Write,
    FailStream_Seek,
    FailStream_SetSize,
    FailStream_CopyTo,
    FailStream_Commit,
    FailStream_Revert,
    FailStream_LockRegion,
    FailStream_UnlockRegion,
    FailStream_Stat,
    FailStream_Clone
};

static void InitFailStream(FAILSTREAM *Stream, ULONG FailFrom, BOOL Exact)
{
    Stream->IStream_iface.lpVtbl = &FailStreamVtbl;
    Stream->Ref = 1;
    Stream->Writes = 0;
    Stream->FailFrom = FailFrom;
    Stream->Exact = Exact;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* Image lists: image k is filled with its own colour (left half) and the complement (right half); in a list with a mask the odd images show only their right half */

static COLORREF LeftColor(INT Index)
{
    return RGB(30 + 40 * Index, 250 - 50 * Index, 20 + 30 * Index);
}

static COLORREF RightColor(INT Index)
{
    return RGB(255 - (30 + 40 * Index), 255 - (250 - 50 * Index), 255 - (20 + 30 * Index));
}

/* A 32-bit bitmap of the size whose left half has one colour and whose right half another, whatever the desktop is */
static HBITMAP CreateColorImage(INT Size, COLORREF Left, COLORREF Right)
{
    BITMAPINFO Info = { { sizeof(BITMAPINFOHEADER), Size, -Size, 1, 32, BI_RGB } };
    DWORD *Bits = NULL;
    HDC Dc = GetDC(NULL);
    HBITMAP Bitmap = CreateDIBSection(Dc, &Info, DIB_RGB_COLORS, (VOID **)&Bits, NULL, 0);
    INT X, Y;

    ReleaseDC(NULL, Dc);
    if (!Bitmap || !Bits)
        return Bitmap;
    for (Y = 0; Y < Size; Y++)
        for (X = 0; X < Size; X++)
            Bits[Y * Size + X] = (X < Size / 2) ? ((GetRValue(Left) << 16) | (GetGValue(Left) << 8) | GetBValue(Left))
                                                : ((GetRValue(Right) << 16) | (GetGValue(Right) << 8) | GetBValue(Right));
    return Bitmap;
}

static HIMAGELIST CreateList(INT Size, UINT ColorFlags, BOOL HasMask, INT Count)
{
    HIMAGELIST List = ImageList_Create(Size, Size, ColorFlags | (HasMask ? ILC_MASK : 0), Count, Count);
    HDC Dc = GetDC(NULL), Memory = CreateCompatibleDC(Dc);
    INT Index;

    ok(List != NULL, "ImageList_Create(%d, %#x) failed\n", Size, ColorFlags);
    for (Index = 0; List && Index < Count; Index++)
    {
        HBITMAP Image = CreateColorImage(Size, LeftColor(Index), RightColor(Index));
        HBITMAP Mask = CreateBitmap(Size, Size, 1, 1, NULL);
        HGDIOBJ Old = SelectObject(Memory, Mask);
        RECT Left = { 0, 0, Size / 2, Size }, Right = { Size / 2, 0, Size, Size };

        /* the mask is white where the image does not show; the whole bitmap is painted */
        FillRect(Memory, &Right, GetStockObject(BLACK_BRUSH));
        FillRect(Memory, &Left, GetStockObject((Index & 1) ? WHITE_BRUSH : BLACK_BRUSH));
        SelectObject(Memory, Old);

        ok(Image != NULL, "image %d was not created\n", Index);
        if (Image)
            ok(ImageList_Add(List, Image, HasMask ? Mask : NULL) == Index, "image %d was not added\n", Index);
        if (Image)
            DeleteObject(Image);
        DeleteObject(Mask);
    }
    DeleteDC(Memory);
    ReleaseDC(NULL, Dc);
    return List;
}

/* Draws an image over a background colour into Pixels (Size x Size, 32 bits); FALSE if that was not possible */
static BOOL DrawImage(HIMAGELIST List, INT Index, INT Size, COLORREF Background, DWORD *Pixels)
{
    BITMAPINFO Info = { { sizeof(BITMAPINFOHEADER), Size, -Size, 1, 32, BI_RGB } };
    HDC Dc = GetDC(NULL), Memory = CreateCompatibleDC(Dc);
    VOID *Bits = NULL;
    HBITMAP Target = CreateDIBSection(Dc, &Info, DIB_RGB_COLORS, &Bits, NULL, 0);
    HGDIOBJ Old;
    HBRUSH Brush = CreateSolidBrush(Background);
    RECT Rect = { 0, 0, Size, Size };
    BOOL Ret = FALSE;

    if (Target && Bits)
    {
        Old = SelectObject(Memory, Target);
        FillRect(Memory, &Rect, Brush);
        Ret = ImageList_Draw(List, Index, Memory, 0, 0, ILD_NORMAL);
        GdiFlush();
        if (Ret)
            memcpy(Pixels, Bits, Size * Size * 4);
        SelectObject(Memory, Old);
    }
    DeleteObject(Brush);
    if (Target)
        DeleteObject(Target);
    DeleteDC(Memory);
    ReleaseDC(NULL, Dc);
    return Ret;
}

static DWORD PixelOf(COLORREF Color)
{
    return (GetRValue(Color) << 16) | (GetGValue(Color) << 8) | GetBValue(Color);
}

/* The high byte of a 32-bit pixel is not part of what is drawn */
static BOOL SamePixels(const DWORD *First, const DWORD *Second, INT Count)
{
    INT i;

    for (i = 0; i < Count; i++)
    {
        if ((First[i] & 0xFFFFFF) != (Second[i] & 0xFFFFFF))
            return FALSE;
    }
    return TRUE;
}

/* The two lists must show the same pixels for every image, over a white and over a black background */
static void CompareLists(HIMAGELIST Expected, HIMAGELIST Actual, INT Size, INT Count, PCSTR Label)
{
    static const COLORREF Backgrounds[] = { RGB(255, 255, 255), RGB(0, 0, 0) };
    INT Cx = 0, Cy = 0, Index, Background;
    DWORD *First, *Second;

    ok(ImageList_GetImageCount(Actual) == Count, "%s: %d images, expected %d\n", Label, ImageList_GetImageCount(Actual), Count);
    ok(ImageList_GetIconSize(Actual, &Cx, &Cy) && Cx == Size && Cy == Size, "%s: image size %d x %d, expected %d\n", Label, Cx, Cy, Size);
    if (ImageList_GetImageCount(Actual) != Count || Cx != Size || Cy != Size)
        return;

    First = HeapAlloc(GetProcessHeap(), 0, Size * Size * 4);
    Second = HeapAlloc(GetProcessHeap(), 0, Size * Size * 4);
    ok(First && Second, "No memory\n");
    for (Index = 0; First && Second && Index < Count; Index++)
    {
        for (Background = 0; Background < 2; Background++)
        {
            BOOL Drawn = DrawImage(Expected, Index, Size, Backgrounds[Background], First) &&
                         DrawImage(Actual, Index, Size, Backgrounds[Background], Second);

            ok(Drawn, "%s: image %d cannot be drawn\n", Label, Index);
            if (Drawn)
                ok(SamePixels(First, Second, Size * Size), "%s: image %d differs over background %d\n", Label, Index, Background);
        }
    }
    HeapFree(GetProcessHeap(), 0, First);
    HeapFree(GetProcessHeap(), 0, Second);
}

/* The pixel (X, Y) of an image drawn over a background; 0xFFFFFFFF if it cannot be drawn */
static DWORD PixelAt(HIMAGELIST List, INT Index, INT Size, COLORREF Background, INT X, INT Y)
{
    DWORD *Pixels = HeapAlloc(GetProcessHeap(), 0, Size * Size * 4), Pixel = 0xFFFFFFFF;

    if (Pixels && DrawImage(List, Index, Size, Background, Pixels))
        Pixel = Pixels[Y * Size + X] & 0xFFFFFF;
    HeapFree(GetProcessHeap(), 0, Pixels);
    return Pixel;
}

/* A 32-bit list of two images: what is visible and what is hidden, the right way round */
static void CheckVisible(HIMAGELIST List, INT Size, PCSTR Label)
{
    COLORREF Background = RGB(255, 0, 255);
    INT Left = Size / 4, Right = Size - Size / 4, Middle = Size / 2;

    /* the first image shows completely */
    ok(PixelAt(List, 0, Size, Background, Left, Middle) == PixelOf(LeftColor(0)), "%s: image 0, left half: %06lx\n", Label,
       PixelAt(List, 0, Size, Background, Left, Middle));
    ok(PixelAt(List, 0, Size, Background, Right, Middle) == PixelOf(RightColor(0)), "%s: image 0, right half: %06lx\n", Label,
       PixelAt(List, 0, Size, Background, Right, Middle));
    /* the second hides its left half */
    ok(PixelAt(List, 1, Size, Background, Left, Middle) == PixelOf(Background), "%s: image 1, left half is not hidden: %06lx\n", Label,
       PixelAt(List, 1, Size, Background, Left, Middle));
    ok(PixelAt(List, 1, Size, Background, Right, Middle) == PixelOf(RightColor(1)), "%s: image 1, right half: %06lx\n", Label,
       PixelAt(List, 1, Size, Background, Right, Middle));
}

static HIMAGELIST ReadBack(IStream *Stream)
{
    LARGE_INTEGER Start = { { 0 } };

    IStream_Seek(Stream, Start, STREAM_SEEK_SET, NULL);
    return ImageList_Read(Stream);
}

/* ------------------------------------------------------------------------------------------------------------- */

/* The test of Wine */
static void test_wine_case(void)
{
    HIMAGELIST himl;
    IStream *stream;
    HRESULT hr;

    himl = ImageList_Create(24, 24, ILC_COLOR24, 1, 1);
    ok(himl != 0, "Failed to create an imagelist.\n");

    hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        ImageList_Destroy(himl);
        return;
    }

    hr = pImageList_WriteEx(himl, ILP_NORMAL, stream);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = pImageList_WriteEx(himl, ILP_DOWNLEVEL, stream);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    IStream_Release(stream);
    ImageList_Destroy(himl);
}

static void TestArguments(void)
{
    static const DWORD Flags[] = { 0, ILP_DOWNLEVEL, 2, 3, 4, 0x10, 0xffffffff };
    HIMAGELIST List = CreateList(16, ILC_COLOR24, FALSE, 1);
    IStream *Stream;
    HRESULT hr;
    ULONG i;

    hr = pImageList_WriteEx(NULL, 0, NULL);
    ok(hr == E_POINTER, "no list and no stream: hr %#lx\n", hr);
    hr = CreateStreamOnHGlobal(NULL, TRUE, &Stream);
    ok(hr == S_OK, "CreateStreamOnHGlobal failed: %#lx\n", hr);
    if (FAILED(hr))
        goto done;

    hr = pImageList_WriteEx(NULL, 0, Stream);
    ok(hr == E_POINTER, "no list: hr %#lx\n", hr);
    hr = pImageList_WriteEx(List, 0, NULL);
    ok(hr == E_INVALIDARG, "no stream: hr %#lx\n", hr);
    hr = pImageList_WriteEx(List, ILP_DOWNLEVEL, NULL);
    ok(hr == E_INVALIDARG, "no stream, downlevel: hr %#lx\n", hr);

    /* the flags are accepted, whatever they are */
    for (i = 0; i < ARRAYSIZE(Flags); i++)
    {
        hr = pImageList_WriteEx(List, Flags[i], Stream);
        ok(hr == S_OK, "flags %#lx: hr %#lx\n", Flags[i], hr);
    }
    IStream_Release(Stream);

done:
    if (List)
        ImageList_Destroy(List);
}

/* The list is written and read back; every image has to show the same pixels */
static void TestRoundTrip(INT Size, UINT ColorFlags, BOOL HasMask, DWORD Flags)
{
    HIMAGELIST List = CreateList(Size, ColorFlags, HasMask, 5), Back;
    IStream *Stream = NULL;
    HRESULT hr;
    char Label[64];

    sprintf(Label, "size %d, colour %#x, mask %d, flags %#lx", Size, ColorFlags, HasMask, Flags);
    if (!List)
        return;
    hr = CreateStreamOnHGlobal(NULL, TRUE, &Stream);
    ok(hr == S_OK, "%s: CreateStreamOnHGlobal failed: %#lx\n", Label, hr);
    if (SUCCEEDED(hr))
    {
        hr = pImageList_WriteEx(List, Flags, Stream);
        ok(hr == S_OK, "%s: not written: %#lx\n", Label, hr);
        if (hr == S_OK)
        {
            Back = ReadBack(Stream);
            ok(Back != NULL, "%s: does not read back\n", Label);
            if (Back)
            {
                CompareLists(List, Back, Size, 5, Label);
                ImageList_Destroy(Back);
            }
        }
        IStream_Release(Stream);
    }
    ImageList_Destroy(List);
}

static void TestContents(DWORD Flags)
{
    HIMAGELIST List = CreateList(16, ILC_COLOR32, TRUE, 2), Back;
    IStream *Stream;
    char Label[32];

    sprintf(Label, "flags %#lx", Flags);
    if (!List)
        return;
    CheckVisible(List, 16, Label);
    if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &Stream)))
    {
        ok(pImageList_WriteEx(List, Flags, Stream) == S_OK, "%s: not written\n", Label);
        Back = ReadBack(Stream);
        ok(Back != NULL, "%s: does not read back\n", Label);
        if (Back)
        {
            CheckVisible(Back, 16, Label);
            CompareLists(List, Back, 16, 2, Label);
            ImageList_Destroy(Back);
        }
        IStream_Release(Stream);
    }
    ImageList_Destroy(List);
}

/* Several lists in one stream come out one after the other */
static void TestConsecutive(void)
{
    HIMAGELIST First = CreateList(16, ILC_COLOR32, TRUE, 3), Second = CreateList(24, ILC_COLOR24, FALSE, 4), Back;
    LARGE_INTEGER Start = { { 0 } };
    IStream *Stream;

    if (First && Second && SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &Stream)))
    {
        ok(pImageList_WriteEx(First, ILP_DOWNLEVEL, Stream) == S_OK, "the first list was not written\n");
        ok(pImageList_WriteEx(Second, ILP_DOWNLEVEL, Stream) == S_OK, "the second list was not written\n");
        IStream_Seek(Stream, Start, STREAM_SEEK_SET, NULL);
        Back = ImageList_Read(Stream);
        ok(Back != NULL, "the first list does not read back\n");
        if (Back)
        {
            CompareLists(First, Back, 16, 3, "first of two");
            ImageList_Destroy(Back);
        }
        Back = ImageList_Read(Stream);
        ok(Back != NULL, "the second list does not read back\n");
        if (Back)
        {
            CompareLists(Second, Back, 24, 4, "second of two");
            ImageList_Destroy(Back);
        }
        IStream_Release(Stream);
    }
    if (First)
        ImageList_Destroy(First);
    if (Second)
        ImageList_Destroy(Second);
}

/* A write that fails must fail the call, whichever it is and whatever the others do */
static void TestFailingStream(HIMAGELIST List, DWORD Flags)
{
    FAILSTREAM Stream;
    ULONG Total, FailAt, Exact;
    HRESULT hr;

    InitFailStream(&Stream, ~0u, FALSE);
    hr = pImageList_WriteEx(List, Flags, &Stream.IStream_iface);
    /* Windows writes the newer format of the flag 0 with more of the stream than writes (E_NOTIMPL from this one) */
    if (hr != S_OK && !is_reactos())
    {
        skip("flags %#lx: a stream that only takes writes is not enough (hr %#lx)\n", Flags, hr);
        return;
    }
    ok(hr == S_OK, "flags %#lx: a stream that takes everything: hr %#lx\n", Flags, hr);
    Total = Stream.Writes;
    ok(Total >= 2, "flags %#lx: %lu writes\n", Flags, Total);
    ok(Stream.Ref == 1, "flags %#lx: the stream has %ld references\n", Flags, Stream.Ref);

    /* from each write on, and then that one write alone: an error that is ignored shows when the later writes work */
    for (Exact = 0; Exact < 2; Exact++)
    {
        for (FailAt = 1; FailAt <= Total; FailAt++)
        {
            InitFailStream(&Stream, FailAt, Exact);
            hr = pImageList_WriteEx(List, Flags, &Stream.IStream_iface);
            ok(FAILED(hr), "flags %#lx: write %lu of %lu failed (alone %lu), hr %#lx\n", Flags, FailAt, Total, Exact, hr);
            ok(Stream.Ref == 1, "flags %#lx: the stream has %ld references\n", Flags, Stream.Ref);
        }
    }
}

/* ImageList_Write must not report success for a write that failed */
static void TestWriteFailure(HIMAGELIST List)
{
    FAILSTREAM Stream;
    ULONG Total, FailAt;
    BOOL Ret;

    InitFailStream(&Stream, ~0u, FALSE);
    Ret = ImageList_Write(List, &Stream.IStream_iface);
    /* Windows writes with more of the stream than writes: it gives up on this one */
    if (!Ret && !is_reactos())
    {
        skip("ImageList_Write needs more than a stream that only takes writes\n");
        return;
    }
    ok(Ret, "ImageList_Write on a stream that takes everything: %d\n", Ret);
    Total = Stream.Writes;
    ok(Total >= 3, "%lu writes for a list with a mask\n", Total);
    for (FailAt = 1; FailAt <= Total; FailAt++)
    {
        InitFailStream(&Stream, FailAt, TRUE);
        Ret = ImageList_Write(List, &Stream.IStream_iface);
        ok(!Ret, "ImageList_Write: write %lu of %lu failed alone, the result is %d\n", FailAt, Total, Ret);
    }
}

START_TEST(ImageListWriteEx)
{
    HMODULE ComCtl32 = GetModuleHandleW(L"comctl32.dll");
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    HIMAGELIST List;
    static const INT Sizes[] = { 8, 16, 20, 24, 32 };
    static const UINT Colors[] = { ILC_COLOR16, ILC_COLOR24, ILC_COLOR32 };
    ULONG Size, Color, Mask, Flag;

    if (!ComCtl32)
    {
        skip("comctl32.dll is not loaded\n");
        return;
    }

    /* ImageList_Write is older than the new function and is checked first */
    List = CreateList(16, ILC_COLOR32, TRUE, 3);
    if (List)
    {
        TestWriteFailure(List);
        ImageList_Destroy(List);
    }

    /* ReactOS has the function from the export level 0x600, which kernel32 shows with GetTickCount64 */
    pImageList_WriteEx = (PFN_ImageList_WriteEx)GetProcAddress(ComCtl32, "ImageList_WriteEx");
    ok(pImageList_WriteEx != NULL || !(is_reactos() && GetProcAddress(Kernel32, "GetTickCount64")),
       "ImageList_WriteEx is not exported\n");
    if (!pImageList_WriteEx)
    {
        skip("ImageList_WriteEx is not exported (comctl32 version 5)\n");
        return;
    }

    /*
     * The ordinals of Windows differ between its versions and bitness, so only the names are common. ReactOS gives the
     * exports without a number of their own the next free one in the order of its spec file: a new export must not move an
     * old one. DrawShadowText has been number 92 there, and ImageList_WriteEx comes after it.
     */
    if (is_reactos())
    {
        FARPROC Shadow = GetProcAddress(ComCtl32, "DrawShadowText");

        ok(Shadow != NULL && GetProcAddress(ComCtl32, MAKEINTRESOURCEA(92)) == Shadow, "export 92 is not DrawShadowText\n");
        ok(GetProcAddress(ComCtl32, MAKEINTRESOURCEA(93)) == (FARPROC)pImageList_WriteEx, "export 93 is not ImageList_WriteEx\n");
    }

    CoInitialize(NULL);

    test_wine_case();
    TestArguments();

    TestContents(0);
    TestContents(ILP_DOWNLEVEL);

    for (Size = 0; Size < ARRAYSIZE(Sizes); Size++)
        for (Color = 0; Color < ARRAYSIZE(Colors); Color++)
            for (Mask = 0; Mask < 2; Mask++)
                for (Flag = 0; Flag < 2; Flag++)
                    TestRoundTrip(Sizes[Size], Colors[Color], Mask, Flag);

    TestConsecutive();

    List = CreateList(16, ILC_COLOR32, TRUE, 3);
    if (List)
    {
        TestFailingStream(List, 0);
        TestFailingStream(List, ILP_DOWNLEVEL);
        ImageList_Destroy(List);
    }

    CoUninitialize();
}
