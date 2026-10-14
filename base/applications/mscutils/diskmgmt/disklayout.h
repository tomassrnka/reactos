/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Editing a partition layout in memory (no system calls, shared with the apitest)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _DISKLAYOUT_H_
#define _DISKLAYOUT_H_

#include "diskmodel.h"

#define DL_MAX_ENTRIES      160     /* 4 primary and the logical drives of an MBR disk, or the 128 slots of a GPT disk */
#define DL_ALIGN            DM_MIB  /* partitions start and end on a mebibyte, as in Windows 7 */
#define DL_MBR_PRIMARY_SLOTS 4

/* partition type the volume manager of Windows 7 gives a new, unformatted partition */
#define DL_MBR_TYPE_NEW     0x06
#define DL_MBR_TYPE_EXTENDED 0x0F

typedef enum _DL_STATUS
{
    DL_OK = 0,
    DL_E_BAD_ARGUMENT,
    DL_E_NOT_FOUND,         /* no partition starts there */
    DL_E_NO_SPACE,          /* the range is not free, or smaller than one mebibyte */
    DL_E_NO_PRIMARY_SLOT,   /* the four entries of the MBR are used */
    DL_E_HAVE_EXTENDED,     /* a second extended partition */
    DL_E_NO_EXTENDED,       /* a logical drive needs the extended partition */
    DL_E_EXTENDED_IN_USE,   /* the extended partition still holds logical drives */
    DL_E_TOO_MANY,          /* no entry left in the table */
    DL_E_WRONG_STYLE,
    DL_E_UNREPRESENTABLE    /* an MBR cannot hold the start or the length */
} DL_STATUS;

typedef struct _DL_ENTRY
{
    ULONGLONG Offset;       /* data start, bytes */
    ULONGLONG Length;       /* bytes */
    ULONG Number;           /* partition number the driver gave; 0 when the partition is new */
    ULONG Slot;             /* MBR primary table slot, 0..3 */
    UCHAR MbrType;
    BOOL Active;
    BOOL Logical;           /* MBR: a logical drive in the extended partition */
    BOOL Container;         /* MBR: the extended partition itself */
    GUID GptType;
    GUID GptId;
    ULONGLONG GptAttributes;
    WCHAR GptName[36];
} DL_ENTRY, *PDL_ENTRY;

typedef struct _DL_LAYOUT
{
    DM_STYLE Style;         /* DM_STYLE_MBR or DM_STYLE_GPT */
    ULONGLONG DiskLength;
    ULONG SectorSize;
    DWORD Signature;        /* MBR */
    GUID DiskId;            /* GPT */
    ULONGLONG UsableStart;  /* GPT: first byte the driver allows */
    ULONGLONG UsableEnd;    /* GPT: end of the usable range */
    ULONG MaxGptEntries;
    ULONG Count;
    DL_ENTRY Entry[DL_MAX_ENTRIES];
} DL_LAYOUT, *PDL_LAYOUT;

/* A stretch of the disk in which a partition may be created. */
typedef struct _DL_FREE
{
    BOOL InExtended;        /* logical drive territory */
    ULONGLONG Offset;       /* start of the gap, as the disk map shows it */
    ULONGLONG Length;       /* length of the gap, as the disk map shows it */
    ULONGLONG DataOffset;   /* where the data of a new partition starts */
    ULONGLONG MaxLength;    /* largest data length a new partition can have; 0 when none fits */
    ULONGLONG WizardMax;    /* the largest size Disk Management offers: one mebibyte less than the gap holds (measured on Windows) */
} DL_FREE, *PDL_FREE;

typedef enum _DL_KIND
{
    DL_NEW_PRIMARY = 0,     /* MBR primary or GPT partition */
    DL_NEW_EXTENDED,
    DL_NEW_LOGICAL
} DL_KIND;

/* sizes the volume manager uses for the reserved partition that the first GPT partition brings */
#define DL_MSR_SMALL        (32 * DM_MIB)
#define DL_MSR_LARGE        (128 * DM_MIB)
#define DL_MSR_LARGE_DISK   (16 * DM_GIB)

BOOL DlFromDriver(const DRIVE_LAYOUT_INFORMATION_EX *Driver, ULONGLONG DiskLength, ULONG SectorSize, PDL_LAYOUT Layout);
VOID DlInit(PDL_LAYOUT Layout, DM_STYLE Style, ULONGLONG DiskLength, ULONG SectorSize, DWORD Signature, const GUID *DiskId);
BOOL DlIsEncodable(const DL_LAYOUT *Layout);
PDRIVE_LAYOUT_INFORMATION_EX DlToDriver(const DL_LAYOUT *Layout, PULONG Size);

ULONGLONG DlAlignUp(ULONGLONG Value);
ULONGLONG DlAlignDown(ULONGLONG Value);
VOID DlUsableRange(const DL_LAYOUT *Layout, PULONGLONG Start, PULONGLONG End);
ULONG DlFindFree(const DL_LAYOUT *Layout, PDL_FREE Out, ULONG Max);
ULONG DlCountPrimaries(const DL_LAYOUT *Layout);        /* MBR: used entries of the primary table including the extended partition */
PDL_ENTRY DlFindEntry(PDL_LAYOUT Layout, ULONGLONG Offset);
PDL_ENTRY DlFindExtended(PDL_LAYOUT Layout);
BOOL DlGptNeedsReserved(const DL_LAYOUT *Layout);       /* the first partition of an empty GPT disk brings the reserved partition */
BOOL DlFindFreeFor(const DL_LAYOUT *Layout, ULONGLONG Offset, BOOL InExtended, PDL_FREE Result);
ULONGLONG DlReservedSize(ULONGLONG DiskLength);

DL_STATUS DlCreate(PDL_LAYOUT Layout, DL_KIND Kind, ULONGLONG Offset, ULONGLONG Length, UCHAR MbrType,
                   const GUID *GptType, const GUID *GptId, PDL_ENTRY *Created);
DL_STATUS DlCreateReserved(PDL_LAYOUT Layout, const GUID *Id);
DL_STATUS DlDelete(PDL_LAYOUT Layout, ULONGLONG Offset);
DL_STATUS DlSetActive(PDL_LAYOUT Layout, ULONGLONG Offset, BOOL Active);
DL_STATUS DlSetMbrType(PDL_LAYOUT Layout, ULONGLONG Offset, UCHAR Type);

/* Growing and shrinking: the gap right behind a partition limits how far it grows. */
ULONGLONG DlMaxGrow(const DL_LAYOUT *Layout, ULONGLONG Offset);
DL_STATUS DlResize(PDL_LAYOUT Layout, ULONGLONG Offset, ULONGLONG NewLength);

/* Windows creates the fourth volume of an MBR disk as an extended partition with a logical drive in it. */
BOOL DlWantsExtendedForNext(const DL_LAYOUT *Layout, const DL_FREE *Free);
/* Partition type of an MBR partition after a format, as Windows sets it. */
UCHAR DlMbrTypeForFileSystem(PCWSTR FileSystem, BOOL Fat12, BOOL Fat16, ULONGLONG Offset, ULONGLONG Length, UCHAR Current);

/* GPT type GUIDs */
extern const GUID DlGuidBasicData;
extern const GUID DlGuidReserved;
extern const GUID DlGuidEfi;

#endif /* _DISKLAYOUT_H_ */
