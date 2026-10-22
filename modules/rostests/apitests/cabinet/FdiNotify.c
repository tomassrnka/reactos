/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the memory layout of the FDINOTIFICATION structure that FDICopy hands to its callback
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#include <windef.h>
#include <winbase.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <fci.h>
#include <fdi.h>

#define P ((ULONG)sizeof(void *))

/* Windows aligns the pointers and INT_PTR members naturally: on x64 the structure is not packed to 4 */
#define OFF_PSZ1     (P)
#define OFF_PSZ2     (2 * P)
#define OFF_PSZ3     (3 * P)
#define OFF_PV       (4 * P)
#define OFF_HF       (5 * P)
#define OFF_DATE     (6 * P)
#define OFF_TIME     (OFF_DATE + 2)
#define OFF_ATTRIBS  (OFF_DATE + 4)
#define OFF_SETID    (OFF_DATE + 6)
#define OFF_ICABINET (OFF_DATE + 8)
#define OFF_IFOLDER  (OFF_DATE + 10)

#include <pshpack1.h>

struct CFHEADER
{
    UCHAR signature[4];  /* file signature */
    ULONG reserved1;    /* reserved */
    ULONG cbCabinet;    /* size of this cabinet file in bytes */
    ULONG reserved2;    /* reserved */
    ULONG coffFiles;    /* offset of the first CFFILE entry */
    ULONG reserved3;    /* reserved */
    UCHAR versionMinor;  /* cabinet file format version, minor */
    UCHAR versionMajor;  /* cabinet file format version, major */
    USHORT cFolders;    /* number of CFFOLDER entries in this cabinet */
    USHORT cFiles;      /* number of CFFILE entries in this cabinet */
    USHORT flags;       /* cabinet file option indicators */
    USHORT setID;       /* must be the same for all cabinets in a set */
    USHORT iCabinet;    /* number of this cabinet file in a set */
#if 0
    USHORT cbCFHeader;  /* (optional) size of per-cabinet reserved area */
    UCHAR cbCFFolder;    /* (optional) size of per-folder reserved area */
    UCHAR cbCFData;      /* (optional) size of per-datablock reserved area */
    UCHAR abReserve;     /* (optional) per-cabinet reserved area */
    UCHAR szCabinetPrev; /* (optional) name of previous cabinet file */
    UCHAR szDiskPrev;    /* (optional) name of previous disk */
    UCHAR szCabinetNext; /* (optional) name of next cabinet file */
    UCHAR szDiskNext;    /* (optional) name of next disk */
#endif
};

struct CFFOLDER
{
    ULONG coffCabStart;  /* offset of the first CFDATA block in this folder */
    USHORT cCFData;      /* number of CFDATA blocks in this folder */
    USHORT typeCompress; /* compression type indicator */
#if 0
    UCHAR abReserve[];    /* (optional) per-folder reserved area */
#endif
};

struct CFFILE
{
    ULONG cbFile;          /* uncompressed size of this file in bytes */
    ULONG uoffFolderStart; /* uncompressed offset of this file in the folder */
    USHORT iFolder;        /* index into the CFFOLDER area */
    USHORT date;           /* date stamp for this file */
    USHORT time;           /* time stamp for this file */
    USHORT attribs;        /* attribute flags for this file */
#if 0
    UCHAR szName[];         /* name of this file */
#endif
};

struct CFDATA
{
    ULONG csum;       /* checksum of this CFDATA entry */
    USHORT cbData;    /* number of compressed bytes in this block */
    USHORT cbUncomp;  /* number of uncompressed bytes in this block */
#if 0
    UCHAR abReserve[]; /* (optional) per-datablock reserved area */
    UCHAR ab[cbData];  /* compressed data bytes */
#endif
};

static const struct
{
    struct CFHEADER header;
    struct CFFOLDER folder;
    struct CFFILE file;
    UCHAR szName[sizeof("file.dat")];
    struct CFDATA data;
    UCHAR ab[sizeof("Hello World!")-1];
} cab_data =
{
    { {'M','S','C','F'}, 0, 0x59, 0, sizeof(struct CFHEADER) + sizeof(struct CFFOLDER), 0, 3,1, 1, 1, 0, 0x1225, 0 },
    { sizeof(struct CFHEADER) + sizeof(struct CFFOLDER) + sizeof(struct CFFILE) + sizeof("file.dat"), 1, tcompTYPE_NONE },
    { sizeof("Hello World!")-1, 0, 0, 0x1225, 0x2013, 0x0020 },
    { 'f','i','l','e','.','d','a','t',0 },
    { 0, sizeof("Hello World!")-1, sizeof("Hello World!")-1 },
    { 'H','e','l','l','o',' ','W','o','r','l','d','!' }
};

#include <poppack.h>

struct mem_data
{
    const char *base;
    LONG size, pos;
};

static int g_copy_count;
static int g_close_count;

static void * CDECL fdi_alloc(ULONG cb)
{
    return HeapAlloc(GetProcessHeap(), 0, cb);
}

static void CDECL fdi_free(void *pv)
{
    HeapFree(GetProcessHeap(), 0, pv);
}

static INT_PTR CDECL fdi_open(char *name, int oflag, int pmode)
{
    struct mem_data *data = HeapAlloc(GetProcessHeap(), 0, sizeof(*data));

    if (!data)
        return -1;
    data->base = (const char *)&cab_data;
    data->size = sizeof(cab_data);
    data->pos = 0;
    return (INT_PTR)data;
}

static UINT CDECL fdi_read(INT_PTR hf, void *pv, UINT cb)
{
    struct mem_data *data = (struct mem_data *)hf;
    UINT available = data->size - data->pos;
    UINT count = (available >= cb) ? cb : available;

    memcpy(pv, data->base + data->pos, count);
    data->pos += count;
    return count;
}

static UINT CDECL fdi_write(INT_PTR hf, void *pv, UINT cb)
{
    return cb;
}

static int CDECL fdi_close(INT_PTR hf)
{
    HeapFree(GetProcessHeap(), 0, (void *)hf);
    return 0;
}

static LONG CDECL fdi_seek(INT_PTR hf, LONG dist, int seektype)
{
    struct mem_data *data = (struct mem_data *)hf;

    switch (seektype)
    {
        case SEEK_SET: data->pos = dist; break;
        case SEEK_CUR: data->pos += dist; break;
        default: return -1;
    }
    if (data->pos < 0)
        data->pos = 0;
    if (data->pos > data->size)
        data->pos = data->size;
    return data->pos;
}

static void check_raw_notification(FDINOTIFICATIONTYPE fdint, PFDINOTIFICATION pfdin, INT_PTR expected_hf)
{
    const BYTE *raw = (const BYTE *)pfdin;
    const char *psz1 = *(const char * const *)(raw + OFF_PSZ1);

    ok(!IsBadReadPtr(psz1, sizeof("file.dat")) && !strcmp(psz1, "file.dat"),
       "type %d: raw psz1 at offset %lu is %p, expected \"file.dat\"\n", fdint, OFF_PSZ1, psz1);
    ok(*(const USHORT *)(raw + OFF_DATE) == 0x1225,
       "type %d: raw date at offset %lu is %#x, expected 0x1225\n", fdint, OFF_DATE, *(const USHORT *)(raw + OFF_DATE));
    ok(*(const USHORT *)(raw + OFF_TIME) == 0x2013,
       "type %d: raw time at offset %lu is %#x, expected 0x2013\n", fdint, OFF_TIME, *(const USHORT *)(raw + OFF_TIME));
    ok(*(const USHORT *)(raw + OFF_ATTRIBS) == 0x20,
       "type %d: raw attribs at offset %lu is %#x, expected 0x20\n", fdint, OFF_ATTRIBS, *(const USHORT *)(raw + OFF_ATTRIBS));
    ok(*(const USHORT *)(raw + OFF_IFOLDER) == 0,
       "type %d: raw iFolder at offset %lu is %#x, expected 0\n", fdint, OFF_IFOLDER, *(const USHORT *)(raw + OFF_IFOLDER));
    if (expected_hf)
        ok(*(const INT_PTR *)(raw + OFF_HF) == expected_hf,
           "type %d: raw hf at offset %lu is %#Ix, expected %#Ix\n", fdint, OFF_HF, *(const INT_PTR *)(raw + OFF_HF), expected_hf);
}

static INT_PTR CDECL fdi_notify(FDINOTIFICATIONTYPE fdint, PFDINOTIFICATION pfdin)
{
    switch (fdint)
    {
        case fdintCOPY_FILE:
            g_copy_count++;
            ok(*(const LONG *)pfdin == 12, "expected cb 12, got %ld\n", *(const LONG *)pfdin);
            check_raw_notification(fdint, pfdin, 0);
            return 0x12345678;

        case fdintCLOSE_FILE_INFO:
            g_close_count++;
            check_raw_notification(fdint, pfdin, 0x12345678);
            return 1;

        default:
            return 0;
    }
}

START_TEST(FdiNotify)
{
    ERF erf;
    HFDI hfdi;
    BOOL ret;

    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, psz1), (int)OFF_PSZ1);
    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, psz2), (int)OFF_PSZ2);
    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, psz3), (int)OFF_PSZ3);
    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, pv), (int)OFF_PV);
    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, hf), (int)OFF_HF);
    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, date), (int)OFF_DATE);
    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, time), (int)OFF_TIME);
    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, attribs), (int)OFF_ATTRIBS);
    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, setID), (int)OFF_SETID);
    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, iCabinet), (int)OFF_ICABINET);
    ok_int((int)FIELD_OFFSET(FDINOTIFICATION, iFolder), (int)OFF_IFOLDER);

    hfdi = FDICreate(fdi_alloc, fdi_free, fdi_open, fdi_read, fdi_write, fdi_close, fdi_seek, cpuUNKNOWN, &erf);
    ok(hfdi != NULL, "FDICreate failed, erfOper %d\n", erf.erfOper);
    if (!hfdi)
        return;

    ret = FDICopy(hfdi, "block", "memory\\", 0, fdi_notify, NULL, NULL);
    ok(ret, "FDICopy failed, erfOper %d\n", erf.erfOper);
    ok_int(g_copy_count, 1);
    ok_int(g_close_count, 1);

    FDIDestroy(hfdi);
}
