/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS VFAT filesystem library
 * FILE:        fat12.c
 * PURPOSE:     Fat12 support
 * PROGRAMMERS: Casper S. Hornstrup (chorns@users.sourceforge.net)
 *              Eric Kohl
 */

/* INCLUDES *******************************************************************/

#include "vfatlib.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS ******************************************************************/

static NTSTATUS
Fat12WriteBootSector(IN HANDLE FileHandle,
                     IN PFAT16_BOOT_SECTOR BootSector,
                     IN OUT PFORMAT_CONTEXT Context)
{
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;
    PFAT16_BOOT_SECTOR NewBootSector;
    LARGE_INTEGER FileOffset;

    /* Allocate buffer for new bootsector */
    NewBootSector = (PFAT16_BOOT_SECTOR)RtlAllocateHeap(RtlGetProcessHeap (),
                                            0,
                                            BootSector->BytesPerSector);
    if (NewBootSector == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Zero the new bootsector */
    RtlZeroMemory(NewBootSector, BootSector->BytesPerSector);

    /* Copy FAT16 BPB to new bootsector */
    memcpy(NewBootSector, BootSector,
           FIELD_OFFSET(FAT16_BOOT_SECTOR, Res2) - FIELD_OFFSET(FAT16_BOOT_SECTOR, Jump));
           /* FAT16 BPB length (up to (not including) Res2) */

    /* Write the boot sector signature */
    NewBootSector->Signature1 = 0xAA550000;

    /* Write sector 0 */
    FileOffset.QuadPart = 0ULL;
    Status = NtWriteFile(FileHandle,
                         NULL,
                         NULL,
                         NULL,
                         &IoStatusBlock,
                         NewBootSector,
                         BootSector->BytesPerSector,
                         &FileOffset,
                         NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("NtWriteFile() failed (Status %lx)\n", Status);
        goto done;
    }

    UpdateProgress(Context, 1);

done:
    /* Free the buffer */
    RtlFreeHeap(RtlGetProcessHeap(), 0, NewBootSector);
    return Status;
}


static NTSTATUS
Fat12WriteFAT(IN HANDLE FileHandle,
              IN ULONG SectorOffset,
              IN PFAT16_BOOT_SECTOR BootSector,
              IN OUT PFORMAT_CONTEXT Context)
{
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;
    PUCHAR Buffer;
    LARGE_INTEGER FileOffset;
    ULONG i;
    ULONG Size;
    ULONG Sectors;

    /* Allocate buffer */
    Buffer = (PUCHAR)RtlAllocateHeap(RtlGetProcessHeap(),
                                     0,
                                     32 * 1024);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Zero the buffer */
    RtlZeroMemory(Buffer, 32 * 1024);

    /* FAT cluster 0 & 1*/
    Buffer[0] = 0xf8; /* Media type */
    Buffer[1] = 0xff;
    Buffer[2] = 0xff;

    /* Write first sector of the FAT */
    FileOffset.QuadPart = (SectorOffset + BootSector->ReservedSectors) * BootSector->BytesPerSector;
    Status = NtWriteFile(FileHandle,
                         NULL,
                         NULL,
                         NULL,
                         &IoStatusBlock,
                         Buffer,
                         BootSector->BytesPerSector,
                         &FileOffset,
                         NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("NtWriteFile() failed (Status %lx)\n", Status);
        goto done;
    }

    UpdateProgress(Context, 1);

    /* Zero the begin of the buffer */
    RtlZeroMemory(Buffer, 3);

    /* Zero the rest of the FAT */
    Sectors = 32 * 1024 / BootSector->BytesPerSector;
    for (i = 1; i < (ULONG)BootSector->FATSectors; i += Sectors)
    {
        /* Zero some sectors of the FAT */
        FileOffset.QuadPart = (SectorOffset + BootSector->ReservedSectors + i) * BootSector->BytesPerSector;
        if (((ULONG)BootSector->FATSectors - i) <= Sectors)
        {
            Sectors = (ULONG)BootSector->FATSectors - i;
        }

        Size = Sectors * BootSector->BytesPerSector;
        Status = NtWriteFile(FileHandle,
                             NULL,
                             NULL,
                             NULL,
                             &IoStatusBlock,
                             Buffer,
                             Size,
                             &FileOffset,
                             NULL);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("NtWriteFile() failed (Status %lx)\n", Status);
            goto done;
        }

        UpdateProgress(Context, Sectors);
    }

done:
    /* Free the buffer */
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    return Status;
}


static NTSTATUS
Fat12WriteRootDirectory(IN HANDLE FileHandle,
                        IN PFAT16_BOOT_SECTOR BootSector,
                        IN BOOLEAN HasLabel,
                        IN OUT PFORMAT_CONTEXT Context)
{
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status = STATUS_SUCCESS;
    PUCHAR Buffer;
    LARGE_INTEGER FileOffset;
    ULONG FirstRootDirSector;
    ULONG RootDirSectors;
    ULONG Sectors;
    ULONG Size;
    ULONG i;

    DPRINT("BootSector->ReservedSectors = %hu\n", BootSector->ReservedSectors);
    DPRINT("BootSector->FATSectors = %hu\n", BootSector->FATSectors);
    DPRINT("BootSector->SectorsPerCluster = %u\n", BootSector->SectorsPerCluster);

    /* Write cluster */
    RootDirSectors = ((BootSector->RootEntries * 32) +
        (BootSector->BytesPerSector - 1)) / BootSector->BytesPerSector;
    FirstRootDirSector =
        BootSector->ReservedSectors + (BootSector->FATCount * BootSector->FATSectors);

    DPRINT("RootDirSectors = %lu\n", RootDirSectors);
    DPRINT("FirstRootDirSector = %lu\n", FirstRootDirSector);

    /* Allocate buffer for the cluster */
    Buffer = (PUCHAR)RtlAllocateHeap(RtlGetProcessHeap(),
                                     0,
                                     32 * 1024);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Zero the buffer */
    RtlZeroMemory(Buffer, 32 * 1024);

    Sectors = 32 * 1024 / BootSector->BytesPerSector;
    for (i = 0; i < RootDirSectors; i += Sectors)
    {
        /* Zero some sectors of the root directory */
        FileOffset.QuadPart = (FirstRootDirSector + i) * BootSector->BytesPerSector;

        if ((RootDirSectors - i) <= Sectors)
        {
            Sectors = RootDirSectors - i;
        }

        Size = Sectors * BootSector->BytesPerSector;

        /* The volume label is the first entry of the root directory */
        if (i == 0 && HasLabel)
            FatInitLabelEntry(Buffer, BootSector->VolumeLabel);
        else
            RtlZeroMemory(Buffer, 32);

        Status = NtWriteFile(FileHandle,
                             NULL,
                             NULL,
                             NULL,
                             &IoStatusBlock,
                             Buffer,
                             Size,
                             &FileOffset,
                             NULL);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("NtWriteFile() failed (Status %lx)\n", Status);
            goto done;
        }

        UpdateProgress(Context, Sectors);
    }

done:
    /* Free the buffer */
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    return Status;
}


NTSTATUS
Fat12Format(IN HANDLE FileHandle,
            IN PPARTITION_INFORMATION_EX PartitionInfo,
            IN PDISK_GEOMETRY DiskGeometry,
            IN PUNICODE_STRING Label,
            IN BOOLEAN QuickFormat,
            IN ULONG ClusterSize,
            IN OUT PFORMAT_CONTEXT Context)
{
    FAT16_BOOT_SECTOR BootSector;
    BOOLEAN AutoClusterSize;
    BOOLEAN HasLabel;
    ULONG SectorCount;
    ULONG RootDirSectors;
    ULONG FatSectors;
    ULONG DataClusters;
    NTSTATUS Status;

    /* Calculate cluster size */
    AutoClusterSize = (ClusterSize == 0);
    if (AutoClusterSize)
    {
        if (DiskGeometry->MediaType == FixedMedia)
        {
            /* 4KB Cluster (Harddisk only) */
            ClusterSize = 4096;
        }
        else
        {
            /* 512 byte cluster (floppy) */
            ClusterSize = 512;
        }
    }

    SectorCount = PartitionInfo->PartitionLength.QuadPart >>
        GetShiftCount(DiskGeometry->BytesPerSector); /* Use shifting to avoid 64-bit division */

    DPRINT("SectorCount = %lu\n", SectorCount);

    /* The 512 root directory entries */
    RootDirSectors = ((512 * 32) + (DiskGeometry->BytesPerSector - 1)) / DiskGeometry->BytesPerSector;

    if (AutoClusterSize && ClusterSize < DiskGeometry->BytesPerSector)
        ClusterSize = DiskGeometry->BytesPerSector;

    /*
     * FAT12 keeps at most 4078 clusters, so that no cluster number reaches
     * the reserved values 0xFF0 and up. A default size grows to stay below.
     */
    for (;;)
    {
        if (!FatIsValidClusterSize(ClusterSize, DiskGeometry->BytesPerSector))
        {
            DPRINT1("Invalid cluster size %lu\n", ClusterSize);
            return STATUS_INVALID_PARAMETER;
        }

        DataClusters = FatComputeLayout(SectorCount,
                                        1 + RootDirSectors,
                                        2,
                                        ClusterSize / DiskGeometry->BytesPerSector,
                                        DiskGeometry->BytesPerSector,
                                        12,
                                        &FatSectors);
        if (DataClusters >= 1 && DataClusters <= 4078)
            break;

        if (AutoClusterSize && DataClusters > 4078 && ClusterSize < 65536)
            ClusterSize *= 2;
        else
        {
            DPRINT1("Cluster count %lu is invalid for FAT12\n", DataClusters);
            return STATUS_INVALID_PARAMETER;
        }
    }

    RtlZeroMemory(&BootSector, sizeof(FAT16_BOOT_SECTOR));
    memcpy(&BootSector.OEMName[0], "MSWIN4.1", 8);
    /* FIXME: Add dummy bootloader for real */
    BootSector.Jump[0] = 0xeb;
    BootSector.Jump[1] = 0x3c;
    BootSector.Jump[2] = 0x90;
    BootSector.BytesPerSector = DiskGeometry->BytesPerSector;
    BootSector.SectorsPerCluster = ClusterSize / BootSector.BytesPerSector;
    BootSector.ReservedSectors = 1;
    BootSector.FATCount = 2;
    BootSector.RootEntries = 512;
    BootSector.Sectors = (SectorCount < 0x10000) ? (unsigned short)SectorCount : 0;
    BootSector.Media = 0xf8;
    BootSector.FATSectors = (unsigned short)FatSectors;
    BootSector.SectorsPerTrack = DiskGeometry->SectorsPerTrack;
    BootSector.Heads = DiskGeometry->TracksPerCylinder;
    BootSector.HiddenSectors = (PartitionInfo->PartitionStyle == PARTITION_STYLE_MBR) ? PartitionInfo->Mbr.HiddenSectors : 0;
    BootSector.SectorsHuge = (SectorCount >= 0x10000) ? (unsigned long)SectorCount : 0;
    BootSector.Drive = (DiskGeometry->MediaType == FixedMedia) ? 0x80 : 0x00;
    BootSector.ExtBootSignature = 0x29;
    BootSector.VolumeID = CalcVolumeSerialNumber();
    Status = FatInitVolumeLabel(Label, BootSector.VolumeLabel, &HasLabel);
    if (!NT_SUCCESS(Status))
        return Status;

    memcpy(&BootSector.SysType[0], "FAT12   ", 8);

    DPRINT("BootSector.FATSectors = %hx\n", BootSector.FATSectors);

    /* Init context data */
    Context->TotalSectorCount =
        1 + (BootSector.FATSectors * 2) + RootDirSectors;

    if (!QuickFormat)
    {
        Context->TotalSectorCount += SectorCount;

        Status = FatWipeSectors(FileHandle,
                                SectorCount,
                                (ULONG)BootSector.SectorsPerCluster,
                                (ULONG)BootSector.BytesPerSector,
                                Context);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("FatWipeSectors() failed with status 0x%.08x\n", Status);
            return Status;
        }
    }

    Status = Fat12WriteBootSector(FileHandle,
                                  &BootSector,
                                  Context);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Fat12WriteBootSector() failed with status 0x%.08x\n", Status);
        return Status;
    }

    /* Write first FAT copy */
    Status = Fat12WriteFAT(FileHandle,
                           0,
                           &BootSector,
                           Context);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Fat12WriteFAT() failed with status 0x%.08x\n", Status);
        return Status;
    }

    /* Write second FAT copy */
    Status = Fat12WriteFAT(FileHandle,
                           (ULONG)BootSector.FATSectors,
                           &BootSector,
                           Context);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Fat12WriteFAT() failed with status 0x%.08x.\n", Status);
        return Status;
    }

    Status = Fat12WriteRootDirectory(FileHandle,
                                     &BootSector,
                                     HasLabel,
                                     Context);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Fat12WriteRootDirectory() failed with status 0x%.08x\n", Status);
    }

    return Status;
}
