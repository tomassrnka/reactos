/*
 * PROJECT:     ReactOS VFAT filesystem library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Growing a FAT volume offline, FAT tables included
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#pragma once

/* Plain C, no system headers: the same file is built into vfatlib and into a host test. */

typedef struct fatgrow_io
{
    void *ctx;
    /* Byte offsets from the start of the volume; 0 on success. */
    int (*read)(void *ctx, unsigned long long offset, void *buffer, unsigned int length);
    int (*write)(void *ctx, unsigned long long offset, const void *buffer, unsigned int length);
    /* Zero-filled memory. */
    void *(*alloc)(void *ctx, unsigned long long size);
    void (*free)(void *ctx, void *p);
} fatgrow_io;

#define FG_OK           0
#define FG_EIO          1   /* a read or write failed */
#define FG_ENOMEM       2
#define FG_EINVAL       3   /* the new size does not grow the volume */
#define FG_EBADFS       4   /* not a FAT volume, or one this code will not touch (inconsistent, bad clusters in the way) */
#define FG_ENOSPC       5   /* not enough free clusters to make room for the larger FATs */
#define FG_ETYPE        6   /* the new cluster count would change the FAT type */

typedef struct fatgrow_result
{
    unsigned int old_fat_sectors, new_fat_sectors;
    unsigned int old_clusters, new_clusters;
    unsigned int moved_clusters;    /* clusters whose data moved out of the way of the FATs */
} fatgrow_result;

/*
 * Grows the FAT volume to new_sectors sectors.  The caller has made those
 * sectors available (the partition grew) and holds the volume exclusively
 * (locked, nothing cached).  When the FATs cannot describe the new clusters,
 * they grow too: the clusters at the start of the data area that the larger
 * FATs cover are moved to free clusters, and every cluster number on the
 * volume shifts down by the number of clusters the FATs took.  Not crash-safe:
 * an interruption between the first FAT write and the boot sector write leaves
 * the volume unusable.
 */
int fatgrow(const fatgrow_io *io, unsigned long long new_sectors, fatgrow_result *result);
