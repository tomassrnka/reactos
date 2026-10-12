/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Interface of ntfsck.dll, the NTFS checker that untfs.dll (and later autochk)
 *              loads for one check at a time
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _NTFSCK_H_
#define _NTFSCK_H_

#include <fmifs/fmifs.h>

/* What the caller asked for: the CHKDSK switches the NTFS checker knows */
#define NTFSCK_FLAG_FIX             0x0001  /* /F */
#define NTFSCK_FLAG_VERBOSE         0x0002  /* /V */
#define NTFSCK_FLAG_SCAN            0x0004  /* /R: bad sectors (implies /F) */
#define NTFSCK_FLAG_DIRTY_ONLY      0x0008  /* report whether the volume is dirty, check nothing */
#define NTFSCK_FLAG_FORCE_DISMOUNT  0x0010  /* /X (implies /F) */
#define NTFSCK_FLAG_SKIP_INDEX      0x0020  /* /I: less vigorous index check */
#define NTFSCK_FLAG_SKIP_CYCLES     0x0040  /* /C: no cycle check in the folder tree */
#define NTFSCK_FLAG_LOG_SIZE        0x0080  /* /L: show the log file size */
#define NTFSCK_FLAG_SET_LOG_SIZE    0x0100  /* /L:size: change it (LogSizeKB) */
#define NTFSCK_FLAG_REEVAL_BAD      0x0200  /* /B: re-evaluate bad clusters (implies /R) */

#define NTFSCK_PARAMS_VERSION 1

typedef struct _NTFSCK_PARAMS
{
    ULONG Size;                 /* sizeof(NTFSCK_PARAMS) */
    ULONG Version;              /* NTFSCK_PARAMS_VERSION */
    PCUNICODE_STRING Volume;    /* NT path of the volume, without a trailing backslash */
    ULONG Flags;                /* NTFSCK_FLAG_ */
    ULONG LogSizeKB;            /* with NTFSCK_FLAG_SET_LOG_SIZE */
    PFMIFSCALLBACK Callback;    /* OUTPUT, PROGRESS; may be NULL */
    /* Results */
    ULONG ExitCode;             /* CHKDSK exit code: 0 no errors, 1 fixed, 2 cleanup, 3 errors left or not checked */
    NTSTATUS Status;            /* STATUS_SUCCESS, STATUS_DISK_CORRUPT_ERROR, or why the check could not run */
    BOOLEAN VolumeDirty;        /* the dirty flag was set */
} NTFSCK_PARAMS, *PNTFSCK_PARAMS;

/* Returns TRUE when the check ran to the end (found errors or not); see ExitCode. */
typedef BOOLEAN (NTAPI *PNTFSCK_RUN)(_Inout_ PNTFSCK_PARAMS Params);

BOOLEAN NTAPI NtfsckRun(_Inout_ PNTFSCK_PARAMS Params);

/*
 * Extension of the fmifs Chkdsk call: chkdsk.exe passes it in the Unused2 argument of
 * fmifs!Chkdsk, which hands it to the provider as pUnknown1. A provider that does not know
 * it ignores it; one that does fills ExitCode.
 */
#define CHKDSK_EXTENSION_MAGIC 0x4B444843 /* 'CHDK' */

typedef struct _CHKDSK_EXTENSION
{
    ULONG Magic;                /* CHKDSK_EXTENSION_MAGIC */
    ULONG Size;                 /* sizeof(CHKDSK_EXTENSION) */
    ULONG Flags;                /* NTFSCK_FLAG_ switches beyond the fmifs arguments */
    ULONG LogSizeKB;
    ULONG ExitCode;             /* filled by the provider; ~0 if it did not */
} CHKDSK_EXTENSION, *PCHKDSK_EXTENSION;

#endif /* _NTFSCK_H_ */
