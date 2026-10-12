/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Volume figures for the summary at the end of a check, and the state of
 *              $LogFile, read from the volume the checker has mounted
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "layout.h"
#include "volume.h"
#include "inode.h"
#include "attrib.h"
#include "mst.h"
#include "logfile.h"
#include "ntfsck_lib.h"

/* ntfsng's metadata journal header in $LogFile (drivers/filesystems/ntfsng/shim/include/kshim_jnl.h) */
#define NG_JNL_PAGE 4096
#define NG_JNL_HDR_PAGE 3
#define NG_JNL_MAGIC "NTFSNGJ1"
enum { NG_JNL_ACTIVE = 1, NG_JNL_COMMITTED = 2 };

/* The ntfsng journal header of $LogFile, read from the disk (not through any cache) */
static int read_ng_header(ntfs_volume *vol, u8 *hdr, size_t len, unsigned long long *log_bytes)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	int ok = 0;

	ni = ntfs_inode_open(vol, FILE_LogFile);
	if (!ni)
		return -1;
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (na) {
		if (log_bytes)
			*log_bytes = na->allocated_size;
		ok = na->data_size >= (NG_JNL_HDR_PAGE + 1) * NG_JNL_PAGE &&
		     ntfs_attr_pread(na, NG_JNL_HDR_PAGE * NG_JNL_PAGE, len, hdr) == (s64)len &&
		     !memcmp(hdr, NG_JNL_MAGIC, 8);
		ntfs_attr_close(na);
	}
	ntfs_inode_close(ni);
	return ok;
}

unsigned long long ntfsck_lib_journal_seq(struct _ntfs_volume *vol)
{
	u8 hdr[24];

	if (read_ng_header(vol, hdr, sizeof(hdr), NULL) != 1)
		return 0;
	return le64_to_cpu(*(le64 *)(hdr + 16));
}

static int log_state(ntfs_volume *vol, unsigned long long *log_bytes)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	RESTART_PAGE_HEADER *rp = NULL;
	u8 hdr[16];
	int state = NTFSCK_LOG_UNREADABLE;

	ni = ntfs_inode_open(vol, FILE_LogFile);
	if (!ni)
		return state;
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		ntfs_inode_close(ni);
		return state;
	}
	*log_bytes = na->allocated_size;
	if (na->data_size >= (NG_JNL_HDR_PAGE + 1) * NG_JNL_PAGE &&
	    ntfs_attr_pread(na, NG_JNL_HDR_PAGE * NG_JNL_PAGE, sizeof(hdr), hdr) == sizeof(hdr) &&
	    !memcmp(hdr, NG_JNL_MAGIC, 8)) {
		u32 st = hdr[12] | hdr[13] << 8 | hdr[14] << 16 | (u32)hdr[15] << 24;
		state = st == NG_JNL_ACTIVE ? NTFSCK_LOG_NG_APPLIED :
			st == NG_JNL_COMMITTED ? NTFSCK_LOG_NG_COMMITTED : NTFSCK_LOG_NG_OTHER;
	} else if (!ntfs_check_logfile(na, &rp)) {
		state = NTFSCK_LOG_UNREADABLE;
	} else if (!rp) {
		state = NTFSCK_LOG_EMPTY;
	} else {
		const RESTART_AREA *ra = (const RESTART_AREA *)((const u8 *)rp +
				le16_to_cpu(rp->restart_area_offset));
		state = (ra->client_in_use_list == LOGFILE_NO_CLIENT ||
			 (ra->flags & RESTART_VOLUME_IS_CLEAN)) ?
			NTFSCK_LOG_CLEAN : NTFSCK_LOG_ACTIVE;
	}
	free(rp);
	ntfs_attr_close(na);
	ntfs_inode_close(ni);
	return state;
}

static unsigned long long count_free_clusters(ntfs_volume *vol)
{
	u8 *buf;
	s64 pos, total = vol->nr_clusters, bytes = (vol->nr_clusters + 7) >> 3;
	unsigned long long used = 0;
	const s64 chunk = 65536;

	buf = malloc(chunk);
	if (!buf || !vol->lcnbmp_na)
		goto fail;
	for (pos = 0; pos < bytes; pos += chunk) {
		s64 n = bytes - pos < chunk ? bytes - pos : chunk, i;
		if (ntfs_attr_pread(vol->lcnbmp_na, pos, n, buf) != n)
			goto fail;
		for (i = 0; i < n; i++) {
			u8 b = buf[i];
			s64 first = (pos + i) << 3;
			/* bits past the last cluster do not count */
			if (first + 8 > total)
				b &= (u8)((1u << (total - first)) - 1);
			while (b) {
				used++;
				b &= b - 1;
			}
		}
	}
	free(buf);
	return (unsigned long long)total - used;
fail:
	free(buf);
	return 0;
}

/*
 * Allocated bytes of the attribute @a of @len bytes (the first extent of a non-resident one;
 * the compressed size for compressed and sparse ones), 0 for anything malformed. @limit is
 * the volume size: nothing larger is plausible.
 */
static s64 attr_alloc(const ATTR_RECORD *a, u32 len, s64 limit)
{
	s64 v;

	if (!a->non_resident || len < 0x40 || sle64_to_cpu(a->lowest_vcn))
		return 0;
	if ((a->flags & (ATTR_IS_COMPRESSED | ATTR_IS_SPARSE)) &&
	    le16_to_cpu(a->mapping_pairs_offset) >= 0x48 && len >= 0x48)
		v = sle64_to_cpu(a->compressed_size);
	else
		v = sle64_to_cpu(a->allocated_size);
	return (v < 0 || v > limit) ? 0 : v;
}

struct rec_info {
	u16 flags;		/* MFT_RECORD_ flags of the base record */
	u8 in_use;
	u8 is_base;
	u32 names;		/* $FILE_NAME attributes outside the DOS namespace */
	s64 data_alloc;		/* allocated bytes of the $DATA attributes */
	s64 index_alloc;	/* allocated bytes of the $INDEX_ALLOCATION attributes */
	u32 index_roots;
};

/*
 * One pass over the raw $MFT. The counting rules give the figures Windows chkdsk prints for
 * the same volume (checked against Windows Server 2008 R2 on 15 volumes): "files" are the
 * hard links of the non-directory, non-view-index records from 16 on, with the allocated
 * size of their $DATA streams; "indexes" are the directory and view indexes with their
 * index allocations; "large file records" are the extension records.
 */
static void count_records(ntfs_volume *vol, struct ntfsck_lib_result *r)
{
	const u32 rs = vol->mft_record_size;
	const s64 limit = vol->nr_clusters * (s64)vol->cluster_size;
	s64 nrec, n;
	struct rec_info *ri = NULL;
	u8 *buf = NULL;
	const s64 per_chunk = 64;

	if (!vol->mft_na || rs < 512 || rs > 65536)
		return;
	r->mft_records = vol->mft_na->data_size / rs;
	/* A damaged $MFT can claim any size: count only what fits the volume and memory. */
	nrec = vol->mft_na->initialized_size / rs;
	if (nrec < 0 || vol->mft_na->initialized_size > vol->mft_na->allocated_size ||
	    vol->mft_na->initialized_size > limit ||
	    (u64)nrec >= (u64)(SIZE_MAX / sizeof(*ri)) - 1)
		return;
	ri = calloc((size_t)nrec + 1, sizeof(*ri));
	buf = malloc(rs * per_chunk);
	if (!ri || !buf)
		goto out;
	for (n = 0; n < nrec; n += per_chunk) {
		s64 cnt = nrec - n < per_chunk ? nrec - n : per_chunk, i;
		if (ntfs_attr_pread(vol->mft_na, n * rs, cnt * rs, buf) != cnt * rs)
			goto out;
		for (i = 0; i < cnt; i++) {
			MFT_RECORD *m = (MFT_RECORD *)(buf + i * rs);
			ATTR_RECORD *a;
			u32 off, used, len;
			u64 base_ref;
			s64 base;
			struct rec_info *target;

			if (!ntfs_is_file_record(m->magic) && !ntfs_is_baad_record(m->magic))
				continue;
			/* The flags lie in the first sector, before any protected sector end. */
			if (!(m->flags & MFT_RECORD_IN_USE))
				continue;
			if (!ntfs_is_file_record(m->magic) ||
			    ntfs_mst_post_read_fixup_warn((NTFS_RECORD *)m, rs, FALSE)) {
				if (r->bad_records < sizeof(r->bad_record_numbers) / sizeof(r->bad_record_numbers[0]))
					r->bad_record_numbers[r->bad_records] = n + i;
				r->bad_records++;
				continue;
			}
			used = le32_to_cpu(m->bytes_in_use);
			off = le16_to_cpu(m->attrs_offset);
			if (used > rs || off < sizeof(MFT_RECORD_OLD) || off >= used)
				continue;
			/* An extension record of record 0 ($MFT) has a nonzero reference with number 0. */
			base_ref = le64_to_cpu(m->base_mft_record);
			base = MREF(base_ref);
			ri[n + i].in_use = 1;
			if (base_ref) {
				r->large_records++;
				if (base >= nrec)
					continue;
				target = &ri[base];
			} else {
				ri[n + i].is_base = 1;
				ri[n + i].flags = le16_to_cpu(m->flags);
				target = &ri[n + i];
			}
			while (off + 16 <= used && !(off & 7)) {
				a = (ATTR_RECORD *)((u8 *)m + off);
				len = le32_to_cpu(a->length);
				if (a->type == AT_END || len < 16 || len > used - off)
					break;
				if (len < (a->non_resident ? 0x40u : 0x18u)) {
					off += len;
					continue;
				}
				switch (le32_to_cpu(a->type)) {
				case 0x30: /* $FILE_NAME */
					if (!a->non_resident &&
					    (u32)le16_to_cpu(a->value_offset) + 66 <= len) {
						FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)((u8 *)a +
							le16_to_cpu(a->value_offset));
						if (fn->file_name_type != FILE_NAME_DOS)
							target->names++;
					}
					break;
				case 0x80: /* $DATA */
					target->data_alloc += attr_alloc(a, len, limit);
					break;
				case 0x90: /* $INDEX_ROOT */
					target->index_roots++;
					break;
				case 0xa0: /* $INDEX_ALLOCATION */
					target->index_alloc += attr_alloc(a, len, limit);
					break;
				case 0xc0: /* $REPARSE_POINT */
					if (!base_ref)
						r->reparse_records++;
					break;
				case 0xe0: /* $EA */
					if (!base_ref)
						r->ea_records++;
					break;
				}
				off += len;
			}
		}
	}
	for (n = 0; n < nrec; n++) {
		if (!ri[n].in_use || !ri[n].is_base)
			continue;
		/* Each total is at most a few attributes of at most the volume size: no overflow. */
		if (ri[n].index_alloc > limit)
			ri[n].index_alloc = limit;
		if (ri[n].data_alloc > limit)
			ri[n].data_alloc = limit;
		if (ri[n].index_roots) {
			r->indexes += ri[n].index_roots;
			r->index_clusters += ((u64)ri[n].index_alloc + vol->cluster_size - 1) / vol->cluster_size;
		}
		if (n < 16 || (ri[n].flags & (MFT_RECORD_IS_DIRECTORY | MFT_RECORD_IS_VIEW_INDEX)))
			continue;
		r->files += ri[n].names ? ri[n].names : 1;
		r->files_clusters += ((u64)ri[n].data_alloc + vol->cluster_size - 1) / vol->cluster_size;
	}
out:
	free(buf);
	free(ri);
}

static unsigned long long count_bad_clusters(ntfs_volume *vol)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	runlist_element *rl;
	unsigned long long bad = 0;
	ntfschar bad_name[] = { const_cpu_to_le16('$'), const_cpu_to_le16('B'),
				const_cpu_to_le16('a'), const_cpu_to_le16('d'), 0 };

	ni = ntfs_inode_open(vol, FILE_BadClus);
	if (!ni)
		return 0;
	na = ntfs_attr_open(ni, AT_DATA, bad_name, 4);
	if (na) {
		if (NAttrNonResident(na) && !ntfs_attr_map_whole_runlist(na)) {
			for (rl = na->rl; rl && rl->length; rl++) {
				if (rl->lcn >= 0)
					bad += rl->length;
			}
		}
		ntfs_attr_close(na);
	}
	ntfs_inode_close(ni);
	return bad;
}

void ntfsck_lib_collect_log(struct _ntfs_volume *vol, struct ntfsck_lib_result *r)
{
	r->log_state = log_state(vol, &r->log_bytes);
}

void ntfsck_lib_collect(struct _ntfs_volume *vol, struct ntfsck_lib_result *r)
{
	u8 bs[512];

	r->mft_records = r->files = r->files_clusters = 0;
	r->indexes = r->index_clusters = 0;
	r->large_records = r->ea_records = r->reparse_records = 0;
	r->index_entries = 0;
	r->bad_records = 0;

	r->bytes_per_sector = vol->sector_size;
	r->cluster_size = vol->cluster_size;
	r->total_clusters = vol->nr_clusters;
	r->ntfs_major = vol->major_ver;
	r->ntfs_minor = vol->minor_ver;
	r->volume_dirty = (vol->flags & VOLUME_IS_DIRTY) ? 1 : 0;
	if (vol->vol_name) {
		strncpy(r->label, vol->vol_name, sizeof(r->label) - 1);
		r->label[sizeof(r->label) - 1] = '\0';
	}
	if (ntfs_pread(vol->dev, 0, sizeof(bs), bs) == sizeof(bs))
		r->total_sectors = le64_to_cpu(((NTFS_BOOT_SECTOR *)bs)->number_of_sectors);
	else
		r->total_sectors = (unsigned long long)vol->nr_clusters *
			(vol->cluster_size / vol->sector_size);
	r->log_state = log_state(vol, &r->log_bytes);
	if (!r->ng_seq_start)
		r->ng_seq_start = ntfsck_lib_journal_seq(vol);
	r->free_clusters = count_free_clusters(vol);
	r->bad_clusters = count_bad_clusters(vol);
	count_records(vol, r);
}
