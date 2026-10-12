/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Interface between the imported checker (plain C, no Windows headers) and the
 *              ReactOS side of the port
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _NTFSCK_LIB_H
#define _NTFSCK_LIB_H

/* Return codes of ntfsck_lib_run(), the fsck exit codes of the upstream program */
#define NTFSCK_RET_NO_ERRORS 0
#define NTFSCK_RET_CORRECTED 1
#define NTFSCK_RET_LEFT_UNCORRECTED 4
#define NTFSCK_RET_OPERATIONAL_ERROR 8

struct ntfsck_lib_opts {
	int repair;		/* 0: check only, the volume opened read-only; 1: repair */
	int verbose;
	int dirty_only;		/* report the dirty flag, check nothing */
	int windows_tolerance;	/* stale FILE_NAME data sizes in index entries are no error */
	int info_only;		/* with dirty_only: also fill the $LogFile state and size */
};

/* $LogFile states that ntfsck_lib_collect() tells apart */
enum {
	NTFSCK_LOG_UNKNOWN = 0,
	NTFSCK_LOG_EMPTY,		/* no restart area, or an emptied log */
	NTFSCK_LOG_CLEAN,		/* NTFS log, shut down cleanly */
	NTFSCK_LOG_ACTIVE,		/* NTFS log with a client in use: not replayed */
	NTFSCK_LOG_NG_APPLIED,		/* ntfsng journal: last transaction applied */
	NTFSCK_LOG_NG_COMMITTED,	/* ntfsng journal: committed transaction not applied */
	NTFSCK_LOG_NG_OTHER,		/* ntfsng journal: errors, or not journaled */
	NTFSCK_LOG_UNREADABLE,
};

struct ntfsck_lib_result {
	int ret;
	int errors_found;
	int errors_fixed;
	int errors_left;
	int not_ntfs;
	int aborted;			/* a pass stopped part way (no memory, an I/O error): the check is incomplete */
	int mount_errno;
	int logfile_reset;
	int log_state;			/* NTFSCK_LOG_ */
	int volume_dirty;
	int hibernated;
	unsigned long long fn_data_size_stale;
	unsigned long long ng_seq_start;	/* ntfsng journal sequence when the check began, 0 if none */
	unsigned long long ng_seq_end;		/* and when it ended: a change means the volume was written meanwhile */
	unsigned long long lcn_bitmap_blocks;	/* $Bitmap blocks that differ from the check */
	unsigned long long mft_bitmap_blocks;	/* $MFT:$BITMAP blocks that differ */

	/* For the summary at the end of a check */
	unsigned int bytes_per_sector;
	unsigned int cluster_size;
	unsigned long long total_sectors;
	unsigned long long total_clusters;
	unsigned long long free_clusters;
	unsigned long long mft_records;
	unsigned long long files;
	unsigned long long files_clusters;
	unsigned long long indexes;
	unsigned long long index_clusters;
	unsigned long long bad_clusters;
	unsigned long long log_bytes;
	unsigned long long large_records;
	unsigned long long ea_records;
	unsigned long long reparse_records;
	unsigned long long index_entries;
	unsigned long long bad_records;		/* in-use records whose multi-sector protection fails */
	unsigned long long bad_record_numbers[16];	/* the first of them */
	unsigned long long orphans;		/* in-use records found in no directory */
	char label[129];		/* UTF-8 */
	unsigned int ntfs_major, ntfs_minor;
};

int ntfsck_lib_run(const char *path, const struct ntfsck_lib_opts *o,
		struct ntfsck_lib_result *r);

/* stats.c: fills the volume part of @r from the mounted volume @vol (an ntfs_volume) */
struct _ntfs_volume;
void ntfsck_lib_collect(struct _ntfs_volume *vol, struct ntfsck_lib_result *r);
/* stats.c: only the $LogFile state and size (CHKDSK /L) */
void ntfsck_lib_collect_log(struct _ntfs_volume *vol, struct ntfsck_lib_result *r);
/* stats.c: the sequence number of the ntfsng journal header of @vol, 0 if it has none */
unsigned long long ntfsck_lib_journal_seq(struct _ntfs_volume *vol);

#endif /* _NTFSCK_LIB_H */
