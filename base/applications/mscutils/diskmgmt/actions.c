/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The actions of the window: which menu items exist and when they are enabled, and what each one runs
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The menus, their order and the enable rules follow Disk Management on Windows Server 2008 R2.
 */

#include "uicommon.h"

#define DM_MENU_TEXT    96
#define PROBE_CACHE     16

/* ------------------------------------------------------------ driver support */

/* What the drivers below answered about growing and shrinking a volume. A probe opens the volume, so the answers
 * are kept per volume until an action changes the disks. */
typedef struct _PROBE
{
    WCHAR Guid[64];
    BOOL ExtendKnown, ShrinkKnown;
    DWORD Extend, Shrink;
} PROBE;

static PROBE g_Probe[PROBE_CACHE];
static ULONG g_ProbeCount;

static PROBE *
ProbeFor(PCWSTR Guid)
{
    ULONG i;

    for (i = 0; i < g_ProbeCount; i++)
        if (_wcsicmp(g_Probe[i].Guid, Guid) == 0)
            return &g_Probe[i];
    if (g_ProbeCount == PROBE_CACHE)
        g_ProbeCount = 0;
    ZeroMemory(&g_Probe[g_ProbeCount], sizeof(PROBE));
    StringCchCopyW(g_Probe[g_ProbeCount].Guid, ARRAYSIZE(g_Probe[0].Guid), Guid);
    return &g_Probe[g_ProbeCount++];
}

static BOOL
ExtendSupported(const DM_DISK *Disk, const DM_REGION *Region)
{
    PROBE *Probe = ProbeFor(Region->VolumeGuid);

    if (!Probe->ExtendKnown)
    {
        Probe->Extend = DmProbeExtend(Disk->Number, Region->VolumeGuid);
        Probe->ExtendKnown = TRUE;
    }
    return Probe->Extend != DM_ERR_NOT_SUPPORTED;
}

static BOOL
ShrinkSupported(const DM_REGION *Region)
{
    PROBE *Probe = ProbeFor(Region->VolumeGuid);

    if (!Probe->ShrinkKnown)
    {
        Probe->Shrink = DmProbeShrink(Region->VolumeGuid);
        Probe->ShrinkKnown = TRUE;
    }
    return Probe->Shrink != DM_ERR_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------- the rules */

static BOOL
ExtendedIsEmpty(const DM_DISK *Disk)
{
    ULONG i;

    for (i = 0; i < Disk->RegionCount; i++)
        if (Disk->Regions[i].Kind == DM_KIND_LOGICAL)
            return FALSE;
    return Disk->HasExtended;
}

/* the gap that follows the block directly and that the block may grow into */
static BOOL
GapBehind(const DM_DISK *Disk, const DM_REGION *Region)
{
    ULONG i;

    for (i = 0; i < Disk->RegionCount; i++)
    {
        const DM_REGION *Next = &Disk->Regions[i];

        if (Next->Offset != Region->Offset + Region->Length)
            continue;
        if (Region->InExtended)
            return Next->Kind == DM_KIND_FREE;
        return Next->Kind == DM_KIND_UNALLOCATED && !Next->InExtended;
    }
    return FALSE;
}

static BOOL
FileSystemIs(const DM_REGION *Region, PCWSTR Name)
{
    return Region->HasFsInfo && _wcsicmp(Region->FileSystem, Name) == 0;
}

/* Gives 0 when the item is enabled, else the string that says why it is not. */
static UINT
CheckAction(UINT Id, const DM_DISK *Disk, const DM_REGION *Region)
{
    BOOL Writable;
    BOOL Partition;

    switch (Id)
    {
        case ID_RESCAN:
            return 0;
        case ID_HELP_TOPIC:
            return IDS_WHY_NO_HELP;
        case ID_NEW_SPANNED:
        case ID_NEW_STRIPED:
        case ID_NEW_MIRRORED:
        case ID_NEW_RAID5:
        case ID_ADD_MIRROR:
        case ID_CONVERT_DYNAMIC:
            return IDS_WHY_NO_DYNAMIC;
        case ID_OFFLINE:
            return IDS_WHY_NO_OFFLINE;
    }
    if (Disk == NULL)
        return IDS_WHY_NO_SELECTION;
    Writable = DiskWritable(Disk);
    Partition = RegionIsPartition(Region);

    switch (Id)
    {
        case ID_OPEN:
        case ID_EXPLORE:
            if (!Partition || !Region->HasVolume || !Region->HasFsInfo)
                return IDS_WHY_NO_FS;
            if (!Region->Letter[0] && !Region->MountPaths[0])
                return IDS_WHY_NO_PATH;
            return 0;

        case ID_MARK_ACTIVE:
            if (!Writable)
                return IDS_WHY_DISK_STATE;
            if (Disk->Style != DM_STYLE_MBR || !Partition || Region->Kind != DM_KIND_PRIMARY || Region->IsDynamic)
                return IDS_WHY_NOT_PRIMARY;
            return Region->Active ? IDS_WHY_ALREADY_ACTIVE : 0;

        case ID_CHANGE_LETTER:
            if (!Partition || !Region->HasVolume || Region->VolumeGuid[0] == 0)
                return IDS_WHY_NO_VOLUME;
            return 0;

        case ID_FORMAT:
            if (!Writable)
                return IDS_WHY_DISK_STATE;
            if (!Partition || !Region->HasVolume || Region->VolumeGuid[0] == 0 || Region->IsDynamic)
                return IDS_WHY_NO_VOLUME;
            return (RegionProtected(Region) || Region->IsEfi || DmIsBootPartition(Disk->Number, Region->Offset)) ? IDS_WHY_PROTECTED : 0;

        case ID_DELETE_VOLUME:
            if (!Writable)
                return IDS_WHY_DISK_STATE;
            if (!Partition || Region->IsDynamic)
                return IDS_WHY_NO_VOLUME;
            return (RegionProtected(Region) || Region->IsEfi || DmIsBootPartition(Disk->Number, Region->Offset)) ? IDS_WHY_PROTECTED : 0;

        case ID_DELETE_PARTITION:
            if (!Writable)
                return IDS_WHY_DISK_STATE;
            if (Region == NULL || Region->Kind != DM_KIND_FREE || Disk->Style != DM_STYLE_MBR)
                return IDS_WHY_NO_VOLUME;
            return ExtendedIsEmpty(Disk) ? 0 : IDS_WHY_EXTENDED_USED;

        case ID_NEW_VOLUME:
            if (!Writable)
                return IDS_WHY_DISK_STATE;
            if (Region == NULL || (Region->Kind != DM_KIND_UNALLOCATED && Region->Kind != DM_KIND_FREE))
                return IDS_WHY_NO_SPACE;
            return (Disk->Style == DM_STYLE_MBR || Disk->Style == DM_STYLE_GPT) ? 0 : IDS_WHY_DISK_STATE;

        case ID_EXTEND_VOLUME:
            if (!Writable)
                return IDS_WHY_DISK_STATE;
            if (!Partition || !Region->HasVolume || Region->VolumeGuid[0] == 0 || Region->IsDynamic || Region->IsEfi)
                return IDS_WHY_NO_VOLUME;
            if (!FileSystemIs(Region, L"NTFS") && !(Region->HasFsInfo && Region->FileSystem[0] == 0))
                return IDS_WHY_EXTEND_FS;
            if (!GapBehind(Disk, Region))
                return IDS_WHY_NO_GAP_BEHIND;
            return ExtendSupported(Disk, Region) ? 0 : IDS_WHY_EXTEND_DRIVER;

        case ID_SHRINK_VOLUME:
            if (!Writable)
                return IDS_WHY_DISK_STATE;
            if (!Partition || !Region->HasVolume || Region->VolumeGuid[0] == 0 || Region->IsDynamic || Region->IsEfi)
                return IDS_WHY_NO_VOLUME;
            if (!FileSystemIs(Region, L"NTFS") && !(Region->HasFsInfo && Region->FileSystem[0] == 0))
                return IDS_WHY_SHRINK_FS;
            return ShrinkSupported(Region) ? 0 : IDS_WHY_SHRINK_DRIVER;

        case ID_INIT_DISK:
            if (!Writable)
                return IDS_WHY_DISK_STATE;
            return (Region == NULL && DiskNeedsInit(Disk)) ? 0 : IDS_WHY_INITIALIZED;

        case ID_CONVERT_GPT:
            if (!Writable)
                return IDS_WHY_DISK_STATE;
            if (Region != NULL || Disk->Style != DM_STYLE_MBR)
                return IDS_WHY_INITIALIZED;
            return DiskHasPartitions(Disk) ? IDS_WHY_HAS_PARTITIONS : 0;

        case ID_CONVERT_MBR:
            if (!Writable)
                return IDS_WHY_DISK_STATE;
            if (Region != NULL || Disk->Style != DM_STYLE_GPT)
                return IDS_WHY_INITIALIZED;
            return DiskHasPartitions(Disk) ? IDS_WHY_HAS_PARTITIONS : 0;
    }
    return IDS_WHY_NO_SELECTION;
}

BOOL
ActionsEnabled(UINT Id, const DM_DISK *Disk, const DM_REGION *Region)
{
    return CheckAction(Id, Disk, Region) == 0;
}

BOOL
ActionsReason(UINT Id, const DM_DISK *Disk, const DM_REGION *Region, PWSTR Buffer, int Count)
{
    UINT Why = CheckAction(Id, Disk, Region);

    if (Why == 0)
        return FALSE;
    return LoadStringBuffer(Why, Buffer, Count);
}

/* ------------------------------------------------------------------- menus */

static VOID
AddItem(HMENU hMenu, UINT Id, UINT TextId, const DM_DISK *Disk, const DM_REGION *Region)
{
    WCHAR Text[DM_MENU_TEXT];

    LoadStringBuffer(TextId, Text, ARRAYSIZE(Text));
    AppendMenuW(hMenu, MF_STRING | (ActionsEnabled(Id, Disk, Region) ? MF_ENABLED : MF_GRAYED), Id, Text);
}

static VOID
AddSeparator(HMENU hMenu)
{
    int Count = GetMenuItemCount(hMenu);

    if (Count > 0 && !(GetMenuState(hMenu, Count - 1, MF_BYPOSITION) & MF_SEPARATOR))
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
}

static VOID
AddDynamicNew(HMENU hMenu, const DM_DISK *Disk, const DM_REGION *Region)
{
    AddItem(hMenu, ID_NEW_SPANNED, IDS_ACT_SPANNED, Disk, Region);
    AddItem(hMenu, ID_NEW_STRIPED, IDS_ACT_STRIPED, Disk, Region);
    AddItem(hMenu, ID_NEW_MIRRORED, IDS_ACT_MIRRORED, Disk, Region);
    AddItem(hMenu, ID_NEW_RAID5, IDS_ACT_RAID5, Disk, Region);
}

VOID
ActionsBuildMenu(HMENU hMenu, const DM_DISK *Disk, const DM_REGION *Region, BOOL Context)
{
    while (GetMenuItemCount(hMenu) > 0)
        DeleteMenu(hMenu, 0, MF_BYPOSITION);
    if (Disk == NULL)
    {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"(none)");
        return;
    }

    if (Disk->Device == DM_DEV_CDROM && Region == NULL)
    {
        /* the label of an optical drive: only the generic items exist */
    }
    else if (Region == NULL)
    {
        /* the label of a disk */
        if (DiskNeedsInit(Disk))
        {
            AddItem(hMenu, ID_INIT_DISK, IDS_ACT_INIT, Disk, Region);
        }
        else
        {
            AddDynamicNew(hMenu, Disk, Region);
            AddSeparator(hMenu);
            AddItem(hMenu, ID_CONVERT_DYNAMIC, IDS_ACT_DYNAMIC, Disk, Region);
            if (Disk->Style == DM_STYLE_GPT)
                AddItem(hMenu, ID_CONVERT_MBR, IDS_ACT_MBR, Disk, Region);
            else
                AddItem(hMenu, ID_CONVERT_GPT, IDS_ACT_GPT, Disk, Region);
        }
        AddSeparator(hMenu);
        AddItem(hMenu, ID_OFFLINE, IDS_ACT_OFFLINE, Disk, Region);
    }
    else if (RegionIsPartition(Region))
    {
        AddItem(hMenu, ID_OPEN, IDS_ACT_OPEN, Disk, Region);
        AddItem(hMenu, ID_EXPLORE, IDS_ACT_EXPLORE, Disk, Region);
        AddSeparator(hMenu);
        AddItem(hMenu, ID_MARK_ACTIVE, IDS_ACT_ACTIVE, Disk, Region);
        AddItem(hMenu, ID_CHANGE_LETTER, IDS_ACT_LETTER, Disk, Region);
        AddItem(hMenu, ID_FORMAT, IDS_ACT_FORMAT, Disk, Region);
        AddSeparator(hMenu);
        AddItem(hMenu, ID_EXTEND_VOLUME, IDS_ACT_EXTEND, Disk, Region);
        AddItem(hMenu, ID_SHRINK_VOLUME, IDS_ACT_SHRINK, Disk, Region);
        AddItem(hMenu, ID_ADD_MIRROR, IDS_ACT_ADD_MIRROR, Disk, Region);
        AddItem(hMenu, ID_DELETE_VOLUME, IDS_ACT_DELETE_VOLUME, Disk, Region);
    }
    else
    {
        AddItem(hMenu, ID_NEW_VOLUME, IDS_ACT_NEW, Disk, Region);
        AddDynamicNew(hMenu, Disk, Region);
        if (Region->Kind == DM_KIND_FREE)
        {
            AddSeparator(hMenu);
            AddItem(hMenu, ID_DELETE_PARTITION, IDS_ACT_DELETE_PARTITION, Disk, Region);
        }
    }

    if (Context)
    {
        WCHAR Text[DM_MENU_TEXT];
        /* Windows lists the properties of a partition and of a disk, and an unallocated block shows those of its disk;
         * free space inside the extended partition has none */
        if (Region == NULL || Region->Kind != DM_KIND_FREE)
        {
            AddSeparator(hMenu);
            LoadStringBuffer(IDS_ACT_PROPERTIES, Text, ARRAYSIZE(Text));
            AppendMenuW(hMenu, MF_STRING | MF_ENABLED, ID_PROPERTIES, Text);
        }
        AddSeparator(hMenu);
        AddItem(hMenu, ID_HELP_TOPIC, IDS_ACT_HELP, Disk, Region);
    }
}

/* ----------------------------------------------------------------- running */

static BOOL
RunConvert(HWND hOwner, const DM_DISK *Disk, DM_STYLE To)
{
    DWORD Error;

    /* Windows asks nothing here: the item exists only for a disk that holds no volume */
    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    Error = DmConvertDisk(Disk->Number, To);
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, NULL);
    return TRUE;
}

static BOOL
RunDelete(HWND hOwner, const TARGET *Target, BOOL ExtendedPartition, ULONGLONG ExtOffset, ULONGLONG ExtLength)
{
    DWORD Error;

    if (!ConfirmBox(hOwner, ExtendedPartition ? IDS_CONFIRM_DELETE_PART_TITLE : IDS_CONFIRM_DELETE_TITLE,
                    ExtendedPartition ? IDS_CONFIRM_DELETE_PART : IDS_CONFIRM_DELETE, NULL))
        return FALSE;
    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    if (ExtendedPartition)
    {
        Error = DmDeletePartition(Target->Disk, ExtOffset, ExtLength, NULL, FALSE);
    }
    else
    {
        Error = DmDeletePartition(Target->Disk, Target->Offset, Target->Length, Target->Guid, FALSE);
        if (Error == DM_ERR_VOLUME_IN_USE)
        {
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            if (!ConfirmBox(hOwner, IDS_CONFIRM_TITLE, IDS_CONFIRM_FORCE, NULL))
                return FALSE;
            SetCursor(LoadCursorW(NULL, IDC_WAIT));
            Error = DmDeletePartition(Target->Disk, Target->Offset, Target->Length, Target->Guid, TRUE);
        }
    }
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, NULL);
    return TRUE;
}

static BOOL
RunMarkActive(HWND hOwner, const TARGET *Target, const DM_DISK *Disk)
{
    DWORD Error;
    ULONG i;
    BOOL OtherActive = FALSE;

    /* Windows asks only when another partition of the disk is active at the moment */
    for (i = 0; i < Disk->RegionCount; i++)
        if (Disk->Regions[i].Active && Disk->Regions[i].Offset != Target->Offset)
            OtherActive = TRUE;
    if (OtherActive && !ConfirmBox(hOwner, IDS_CONFIRM_TITLE, IDS_CONFIRM_ACTIVE, NULL))
        return FALSE;
    Error = DmMarkActive(Target->Disk, Target->Offset);
    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, NULL);
    return TRUE;
}

static VOID
RunOpen(HWND hOwner, const TARGET *Target, BOOL Explore)
{
    WCHAR Path[MAX_PATH];

    if (Target->Letter[0])
        StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\", Target->Letter);
    else
        StringCchCopyW(Path, ARRAYSIZE(Path), Target->Paths);
    ShellExecuteW(hOwner, Explore ? L"explore" : L"open", Path, NULL, NULL, SW_SHOWNORMAL);
}

BOOL
ActionsRun(HWND hOwner, UINT Id, const DM_DISK *Disk, const DM_REGION *Region, PDM_MODEL Model)
{
    TARGET Target;
    ULONGLONG ExtOffset = Disk ? Disk->ExtOffset : 0, ExtLength = Disk ? Disk->ExtLength : 0;
    BOOL Changed = FALSE;

    if (Id == ID_RESCAN)
    {
        SetCursor(LoadCursorW(NULL, IDC_WAIT));
        DmRescanDisks();
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        g_ProbeCount = 0;
        return TRUE;
    }
    if (Disk == NULL || !ActionsEnabled(Id, Disk, Region))
        return FALSE;
    FillTarget(&Target, Disk, Region);

    switch (Id)
    {
        case ID_OPEN:           RunOpen(hOwner, &Target, FALSE); break;
        case ID_EXPLORE:        RunOpen(hOwner, &Target, TRUE); break;
        case ID_MARK_ACTIVE:    Changed = RunMarkActive(hOwner, &Target, Disk); break;
        case ID_CHANGE_LETTER:  Changed = DlgLetters(hOwner, &Target); break;
        case ID_FORMAT:         Changed = DlgFormat(hOwner, &Target); break;
        case ID_DELETE_VOLUME:  Changed = RunDelete(hOwner, &Target, FALSE, 0, 0); break;
        case ID_DELETE_PARTITION: Changed = RunDelete(hOwner, &Target, TRUE, ExtOffset, ExtLength); break;
        case ID_NEW_VOLUME:     Changed = WizNewVolume(hOwner, &Target); break;
        case ID_EXTEND_VOLUME:  Changed = WizExtendVolume(hOwner, &Target, Disk); break;
        case ID_SHRINK_VOLUME:  Changed = DlgShrink(hOwner, &Target); break;
        case ID_INIT_DISK:      Changed = DlgInitialize(hOwner, Model, Disk); break;
        case ID_CONVERT_GPT:    Changed = RunConvert(hOwner, Disk, DM_STYLE_GPT); break;
        case ID_CONVERT_MBR:    Changed = RunConvert(hOwner, Disk, DM_STYLE_MBR); break;
    }
    if (Changed)
        g_ProbeCount = 0;
    return Changed;
}
