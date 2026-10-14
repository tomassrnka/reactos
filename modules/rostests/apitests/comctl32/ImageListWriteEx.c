/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for ImageList_WriteEx of comctl32 version 6 (Windows Vista and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows Server 2008 R2 (x64 and WOW64) and Windows 10 22H2. The stream that a flag
 * of 0 writes there has the format of Windows Vista and later, and the stream of ILP_DOWNLEVEL the older one:
 * only that both are read back is tested, not their bytes.
 */

#define COBJMACROS
#define CONST_VTABLE
#include "wine/test.h"
#include <windows.h>
#include <commctrl.h>
#include <ole2.h>

typedef HRESULT (WINAPI *PFN_ImageList_WriteEx)(HIMAGELIST, DWORD, LPSTREAM);

#define IMAGE_SIZE 16

/* a stream that keeps nothing: it counts the writes and fails from the FailFrom-th on, or only that one if Exact */
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

/* Two images of different colours, the second one with a mask that hides half of it. */
static HIMAGELIST CreateList(void)
{
    HIMAGELIST List = ImageList_Create(IMAGE_SIZE, IMAGE_SIZE, ILC_COLOR32 | ILC_MASK, 2, 2);
    HDC Dc = GetDC(NULL);
    HBITMAP Red = CreateCompatibleBitmap(Dc, IMAGE_SIZE, IMAGE_SIZE);
    HBITMAP Green = CreateCompatibleBitmap(Dc, IMAGE_SIZE, IMAGE_SIZE);
    HBITMAP Mask = CreateBitmap(IMAGE_SIZE, IMAGE_SIZE, 1, 1, NULL);
    HDC Memory = CreateCompatibleDC(Dc);
    HGDIOBJ Old;
    HBRUSH Brush;
    RECT Rect = { 0, 0, IMAGE_SIZE, IMAGE_SIZE };

    Old = SelectObject(Memory, Red);
    Brush = CreateSolidBrush(RGB(255, 0, 0));
    FillRect(Memory, &Rect, Brush);
    DeleteObject(Brush);
    SelectObject(Memory, Green);
    Brush = CreateSolidBrush(RGB(0, 255, 0));
    FillRect(Memory, &Rect, Brush);
    DeleteObject(Brush);
    SelectObject(Memory, Mask);
    Rect.right = IMAGE_SIZE;
    FillRect(Memory, &Rect, GetStockObject(BLACK_BRUSH));
    Rect.right = IMAGE_SIZE / 2;
    FillRect(Memory, &Rect, GetStockObject(WHITE_BRUSH));
    SelectObject(Memory, Old);
    DeleteDC(Memory);

    if (List)
    {
        ok(ImageList_Add(List, Red, NULL) == 0, "the first image was not added\n");
        ok(ImageList_Add(List, Green, Mask) == 1, "the second image was not added\n");
    }
    DeleteObject(Red);
    DeleteObject(Green);
    DeleteObject(Mask);
    ReleaseDC(NULL, Dc);
    return List;
}

/* The bits of a bitmap as 32-bit pixels, and the size that they have; the caller frees them. */
static BYTE *GetBits(HBITMAP Bitmap, ULONG *Size, LONG *Width, LONG *Height)
{
    BITMAP Info;
    BITMAPINFO Bmi;
    HDC Dc;
    BYTE *Bits;

    if (!Bitmap || !GetObjectW(Bitmap, sizeof(Info), &Info))
        return NULL;
    memset(&Bmi, 0, sizeof(Bmi));
    Bmi.bmiHeader.biSize = sizeof(Bmi.bmiHeader);
    Bmi.bmiHeader.biWidth = Info.bmWidth;
    Bmi.bmiHeader.biHeight = -Info.bmHeight;
    Bmi.bmiHeader.biPlanes = 1;
    Bmi.bmiHeader.biBitCount = 32;
    Bmi.bmiHeader.biCompression = BI_RGB;
    *Width = Info.bmWidth;
    *Height = Info.bmHeight;
    *Size = Info.bmWidth * Info.bmHeight * 4;
    Bits = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *Size);
    if (!Bits)
        return NULL;
    Dc = GetDC(NULL);
    if (GetDIBits(Dc, Bitmap, 0, Info.bmHeight, Bits, &Bmi, DIB_RGB_COLORS) != Info.bmHeight)
    {
        HeapFree(GetProcessHeap(), 0, Bits);
        Bits = NULL;
    }
    ReleaseDC(NULL, Dc);
    return Bits;
}

/* The pixels of one image inside its bitmap, as rows of 32-bit pixels; the caller frees them */
static BYTE *GetImagePixels(HBITMAP Bitmap, const RECT *Rect)
{
    ULONG Size;
    LONG Width, Height, Row;
    BYTE *All, *Pixels;
    LONG ImageWidth = Rect->right - Rect->left, ImageHeight = Rect->bottom - Rect->top;

    All = GetBits(Bitmap, &Size, &Width, &Height);
    if (!All)
        return NULL;
    if (Rect->left < 0 || Rect->top < 0 || Rect->right > Width || Rect->bottom > Height || ImageWidth <= 0 || ImageHeight <= 0)
    {
        HeapFree(GetProcessHeap(), 0, All);
        return NULL;
    }
    Pixels = HeapAlloc(GetProcessHeap(), 0, ImageWidth * ImageHeight * 4);
    if (Pixels)
    {
        for (Row = 0; Row < ImageHeight; Row++)
            memcpy(Pixels + Row * ImageWidth * 4, All + ((Rect->top + Row) * Width + Rect->left) * 4, ImageWidth * 4);
    }
    HeapFree(GetProcessHeap(), 0, All);
    return Pixels;
}

/* The pixels of the colour bitmap and of the mask of each image of two image lists must agree. */
static void CompareLists(HIMAGELIST Expected, HIMAGELIST Actual, DWORD Flags)
{
    IMAGEINFO ExpectedInfo, ActualInfo;
    BYTE *ExpectedPixels, *ActualPixels, *First = NULL;
    INT Cx = 0, Cy = 0, Index, Part;
    ULONG Size;

    ok(ImageList_GetImageCount(Actual) == 2, "flags %#lx: %d images\n", Flags, ImageList_GetImageCount(Actual));
    ok(ImageList_GetIconSize(Actual, &Cx, &Cy) && Cx == IMAGE_SIZE && Cy == IMAGE_SIZE,
       "flags %#lx: image size %d x %d\n", Flags, Cx, Cy);

    for (Index = 0; Index < 2; Index++)
    {
        if (!ImageList_GetImageInfo(Expected, Index, &ExpectedInfo) || !ImageList_GetImageInfo(Actual, Index, &ActualInfo))
        {
            ok(0, "flags %#lx: no info of image %d\n", Flags, Index);
            continue;
        }
        Size = (ExpectedInfo.rcImage.right - ExpectedInfo.rcImage.left) * (ExpectedInfo.rcImage.bottom - ExpectedInfo.rcImage.top) * 4;
        /* an image that came back with other dimensions is compared by neither: its buffer has another size */
        if (ActualInfo.rcImage.right - ActualInfo.rcImage.left != ExpectedInfo.rcImage.right - ExpectedInfo.rcImage.left ||
            ActualInfo.rcImage.bottom - ActualInfo.rcImage.top != ExpectedInfo.rcImage.bottom - ExpectedInfo.rcImage.top)
        {
            ok(0, "flags %#lx: image %d is %ld x %ld, expected %ld x %ld\n", Flags, Index,
               ActualInfo.rcImage.right - ActualInfo.rcImage.left, ActualInfo.rcImage.bottom - ActualInfo.rcImage.top,
               ExpectedInfo.rcImage.right - ExpectedInfo.rcImage.left, ExpectedInfo.rcImage.bottom - ExpectedInfo.rcImage.top);
            continue;
        }

        for (Part = 0; Part < 2; Part++)
        {
            HBITMAP ExpectedBitmap = Part ? ExpectedInfo.hbmMask : ExpectedInfo.hbmImage;
            HBITMAP ActualBitmap = Part ? ActualInfo.hbmMask : ActualInfo.hbmImage;

            ExpectedPixels = GetImagePixels(ExpectedBitmap, &ExpectedInfo.rcImage);
            ActualPixels = GetImagePixels(ActualBitmap, &ActualInfo.rcImage);
            ok(ExpectedPixels && ActualPixels, "flags %#lx: the %s pixels of image %d are not available\n", Flags,
               Part ? "mask" : "colour", Index);
            if (ExpectedPixels && ActualPixels)
            {
                ok(!memcmp(ExpectedPixels, ActualPixels, Size), "flags %#lx: the %s pixels of image %d differ\n", Flags,
                   Part ? "mask" : "colour", Index);
                if (Part == 0 && Index == 0)
                {
                    First = ExpectedPixels;
                    ExpectedPixels = NULL;
                }
                else if (Part == 0 && Index == 1 && First)
                    ok(memcmp(First, ExpectedPixels, Size) != 0, "the two images of the original do not differ\n");
            }
            HeapFree(GetProcessHeap(), 0, ExpectedPixels);
            HeapFree(GetProcessHeap(), 0, ActualPixels);
        }
    }
    HeapFree(GetProcessHeap(), 0, First);
}

/* One pixel of one image of a list: the colour bitmap (Part 0) or the mask (Part 1), as red, green and blue */
static BOOL GetImagePixel(HIMAGELIST List, INT Index, INT Part, INT X, INT Y, BYTE *Rgb)
{
    IMAGEINFO Info;
    BYTE *Pixels;

    if (!ImageList_GetImageInfo(List, Index, &Info))
        return FALSE;
    Pixels = GetImagePixels(Part ? Info.hbmMask : Info.hbmImage, &Info.rcImage);
    if (!Pixels)
        return FALSE;
    /* the bits are blue, green, red and a byte that is not looked at */
    Rgb[0] = Pixels[(Y * (Info.rcImage.right - Info.rcImage.left) + X) * 4 + 2];
    Rgb[1] = Pixels[(Y * (Info.rcImage.right - Info.rcImage.left) + X) * 4 + 1];
    Rgb[2] = Pixels[(Y * (Info.rcImage.right - Info.rcImage.left) + X) * 4];
    HeapFree(GetProcessHeap(), 0, Pixels);
    return TRUE;
}

static void CheckPixel(HIMAGELIST List, INT Index, INT Part, INT X, INT Y, BYTE R, BYTE G, BYTE B, DWORD Flags)
{
    BYTE Rgb[3];

    if (!GetImagePixel(List, Index, Part, X, Y, Rgb))
    {
        ok(0, "flags %#lx: no pixel (%d, %d) of the %s of image %d\n", Flags, X, Y, Part ? "mask" : "colour", Index);
        return;
    }
    ok(Rgb[0] == R && Rgb[1] == G && Rgb[2] == B, "flags %#lx: the %s of image %d at (%d, %d) is %02x%02x%02x, expected %02x%02x%02x\n",
       Flags, Part ? "mask" : "colour", Index, X, Y, Rgb[0], Rgb[1], Rgb[2], R, G, B);
}

/* What the original holds and what must come back: red, then green of which only the right half shows (the mask is white where it hides) */
static void CheckContents(HIMAGELIST List, DWORD Flags)
{
    CheckPixel(List, 0, 0, 8, 8, 0xff, 0, 0, Flags);
    CheckPixel(List, 1, 0, 12, 8, 0, 0xff, 0, Flags);
    CheckPixel(List, 1, 1, 12, 8, 0, 0, 0, Flags);
    CheckPixel(List, 1, 1, 4, 8, 0xff, 0xff, 0xff, Flags);
    /* the first image has no mask of its own: all of it shows */
    CheckPixel(List, 0, 1, 8, 8, 0, 0, 0, Flags);
}

/* Whatever the stream does with its writes, the list must not be reported as written when one of them fails. */
static void TestFailingStream(PFN_ImageList_WriteEx pImageList_WriteEx, HIMAGELIST List, DWORD Flags)
{
    FAILSTREAM Stream;
    ULONG Total, FailAt, Exact;
    HRESULT hr;

    InitFailStream(&Stream, ~0u, FALSE);
    hr = pImageList_WriteEx(List, Flags, &Stream.IStream_iface);
    /* the format of Windows Vista and later is written with more of the stream than writes (measured: E_NOTIMPL from this stream) */
    if (hr != S_OK && !is_reactos())
    {
        skip("flags %#lx: the stream that only takes writes is not enough (hr %#lx)\n", Flags, hr);
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

START_TEST(ImageListWriteEx)
{
    HMODULE ComCtl32 = GetModuleHandleW(L"comctl32.dll");
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PFN_ImageList_WriteEx pImageList_WriteEx;
    static const DWORD Flags[] = { 0, 1, 2, 3, 4, 0x10, 0xffffffff };
    HIMAGELIST List, Back;
    IStream *Stream;
    LARGE_INTEGER Start = { { 0 } };
    HRESULT hr;
    ULONG i;

    if (!ComCtl32)
    {
        skip("comctl32.dll is not loaded\n");
        return;
    }

    /* ReactOS has the function from export level 0x600, which a kernel32 with GetTickCount64 shows */
    pImageList_WriteEx = (PFN_ImageList_WriteEx)GetProcAddress(ComCtl32, "ImageList_WriteEx");
    ok(pImageList_WriteEx != NULL || !(is_reactos() && GetProcAddress(Kernel32, "GetTickCount64")),
       "ImageList_WriteEx is not exported\n");
    if (!pImageList_WriteEx)
    {
        skip("ImageList_WriteEx is not exported\n");
        return;
    }

    /* Version 5 does not have the function */
    if (!GetProcAddress(ComCtl32, "LoadIconMetric"))
    {
        skip("comctl32 version 5 is loaded\n");
        return;
    }

    /*
     * ReactOS gives the exports without an ordinal of their own the next free one in the order of its spec file: a new
     * export must not move an old one. DrawShadowText has been number 92 there, and ImageList_WriteEx comes after it.
     */
    if (is_reactos())
    {
        FARPROC ByOrdinal = GetProcAddress(ComCtl32, MAKEINTRESOURCEA(92));
        FARPROC ByName = GetProcAddress(ComCtl32, "DrawShadowText");

        ok(ByName != NULL, "DrawShadowText is not exported\n");
        ok(ByOrdinal == ByName, "export 92 is %p, DrawShadowText is %p\n", ByOrdinal, ByName);
        ok(GetProcAddress(ComCtl32, MAKEINTRESOURCEA(93)) == (FARPROC)pImageList_WriteEx, "export 93 is not ImageList_WriteEx\n");
    }

    CoInitialize(NULL);
    List = CreateList();
    ok(List != NULL, "ImageList_Create failed\n");
    if (!List)
        return;

    /* the stream of each flag is written and the list reads back from it with the same pixels; the flags beyond 1 are accepted too */
    for (i = 0; i < RTL_NUMBER_OF(Flags); i++)
    {
        hr = CreateStreamOnHGlobal(NULL, TRUE, &Stream);
        ok(hr == S_OK, "CreateStreamOnHGlobal failed: %#lx\n", hr);
        if (FAILED(hr))
            continue;
        hr = pImageList_WriteEx(List, Flags[i], Stream);
        ok(hr == S_OK, "flags %#lx: hr %#lx\n", Flags[i], hr);
        if (Flags[i] <= 1 && hr == S_OK)
        {
            IStream_Seek(Stream, Start, STREAM_SEEK_SET, NULL);
            Back = ImageList_Read(Stream);
            ok(Back != NULL, "flags %#lx: the stream does not read back\n", Flags[i]);
            if (Back)
            {
                CheckContents(List, Flags[i]);
                CompareLists(List, Back, Flags[i]);
                CheckContents(Back, Flags[i]);
                ImageList_Destroy(Back);
            }
        }
        IStream_Release(Stream);
    }

    /* a stream that fails part of the way */
    TestFailingStream(pImageList_WriteEx, List, 0);
    TestFailingStream(pImageList_WriteEx, List, 1);

    /*
     * ImageList_Write of ReactOS answers TRUE when the write of the colour bitmap fails and goes on to write the
     * mask, which Windows does not do. The new export reports the failure, and the old one is kept as it was.
     */
    if (is_reactos())
    {
        FAILSTREAM Failing;
        BOOL Ret;

        InitFailStream(&Failing, 2, TRUE);
        Ret = ImageList_Write(List, &Failing.IStream_iface);
        ok(Ret == TRUE, "ImageList_Write with a failing second write: %d\n", Ret);
        ok(Failing.Writes == 3, "ImageList_Write made %lu writes\n", Failing.Writes);
    }

    /* the arguments */
    hr = pImageList_WriteEx(NULL, 0, NULL);
    ok(hr == E_POINTER, "no list and no stream: hr %#lx\n", hr);
    hr = CreateStreamOnHGlobal(NULL, TRUE, &Stream);
    ok(hr == S_OK, "CreateStreamOnHGlobal failed: %#lx\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = pImageList_WriteEx(NULL, 0, Stream);
        ok(hr == E_POINTER, "no list: hr %#lx\n", hr);
        IStream_Release(Stream);
    }
    hr = pImageList_WriteEx(List, 0, NULL);
    ok(hr == E_INVALIDARG, "no stream: hr %#lx\n", hr);

    ImageList_Destroy(List);
}
