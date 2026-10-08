// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ngfsck.c - a fast read-only consistency check of a mounted (still read-only) volume, run before
 * the volume goes read-write.  It reads $MFT, $MFT:$BITMAP, $Bitmap and every directory index
 * straight from the device and looks for the damage that makes further writes unsafe:
 *   - an in-use MFT record that is unreadable (magic, fixups, header, attribute chain);
 *   - an in-use record whose bit in $MFT:$BITMAP is clear (the record would be handed out again);
 *   - a cluster referenced by a record but free in $Bitmap, or referenced twice (cross-link);
 *   - a mapping pair outside the volume;
 *   - a directory index entry that names a free record or a record of another sequence number;
 *   - an extension record whose base record is not in use.
 * Leaks (clusters or records marked used that nothing references) are counted and reported but do
 * not make the volume read-only: they cannot cause a double allocation.
 * The NTFS format knowledge here comes from the documented on-disk layout (the core's layout.h);
 * mapping pairs and fixups are decoded by the core's own functions.
 */
#include <kshim.h>
#include "ntfs/ntfs.h"
#include "ntfs/attrib.h"
#include "ntfs/inode.h"
#include "ntfs/mft.h"
#include "ntfs/runlist.h"
#include "ngfsck.h"
#include <ngos.h>

int kshim_dev_rw(struct block_device *b, int write, u64 off, void *buf, size_t len);

/* Cluster bitmaps above this size (16 MB: 512 GB of 4 KB clusters) are not checked cluster by cluster. */
#define NGF_MAX_CBMP (16u << 20)
#define NGF_CHUNK (256u << 10)
#define NGF_MAX_REPORT 12

struct ngf {
	struct ntfs_volume *vol;
	struct block_device *b;
	struct ngc_fsck_res *r;
	u8 *mftbmp;		/* $MFT:$BITMAP */
	u64 mftbmp_bits;
	u8 *lcnbmp;		/* $Bitmap */
	u8 *ref;		/* clusters referenced by records */
	u16 *seq;		/* per record: sequence number, 0 = not in use */
	u8 *isdir;
	u64 nrec;
	u8 *rec;		/* one record */
	u8 *chunk;		/* records being walked */
	u8 *ib;			/* one index block */
};

static void ngf_fatal(struct ngf *f, const char *fmt, ...)
{
	va_list ap;
	char line[160];
	f->r->fatal++;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (f->r->fatal <= NGF_MAX_REPORT)
		printk(KERN_ERR "CHECK: %s\n", line);
	if (!f->r->why[0])
		strscpy(f->r->why, line, sizeof(f->r->why));
}

static void ngf_leak(struct ngf *f, const char *fmt, ...)
{
	va_list ap;
	char line[160];
	f->r->leaks++;
	if (f->r->leaks > NGF_MAX_REPORT)
		return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printk(KERN_WARNING "CHECK (leak): %s\n", line);
}

static inline int tbit(const u8 *m, u64 i) { return (m[i >> 3] >> (i & 7)) & 1; }
static inline void sbit(u8 *m, u64 i) { m[i >> 3] |= (u8)(1u << (i & 7)); }

/* Reads [off, off+len) of the stream described by @rl (cluster runs) into @buf; holes read as zero. */
static int ngf_read_rl(struct ngf *f, struct runlist_element *rl, u64 off, u8 *buf, u32 len)
{
	struct ntfs_volume *vol = f->vol;
	u64 pos = off, end = off + len;
	while (pos < end) {
		s64 vcn = (s64)(pos >> vol->cluster_size_bits);
		u64 run_end, n;
		while (rl && rl->length && rl->vcn + rl->length <= vcn)
			rl++;
		if (!rl || !rl->length || rl->vcn > vcn)
			return -EIO;
		run_end = (u64)(rl->vcn + rl->length) << vol->cluster_size_bits;
		n = min_t(u64, run_end, end) - pos;
		if (rl->lcn == LCN_HOLE) {
			memset(buf + (pos - off), 0, n);
		} else if (rl->lcn < 0) {
			return -EIO;
		} else {
			int err = kshim_dev_rw(f->b, 0, ((u64)rl->lcn << vol->cluster_size_bits) +
					       (pos - ((u64)rl->vcn << vol->cluster_size_bits)), buf + (pos - off), (size_t)n);
			if (err)
				return err;
		}
		pos += n;
	}
	return 0;
}

/* The whole stream of a system inode ($Bitmap, $MFT:$BITMAP) through the core's page cache. */
static u8 *ngf_read_inode(struct ngf *f, struct inode *vi, u64 *size)
{
	u64 sz = (u64)i_size_read(vi), pos;
	u8 *buf;
	if (sz == 0 || sz > (64u << 20))
		return NULL;
	buf = kvmalloc((size_t)((sz + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1)), GFP_KERNEL);
	if (!buf)
		return NULL;
	for (pos = 0; pos < sz; pos += PAGE_SIZE) {
		struct folio *fo = read_mapping_folio(vi->i_mapping, (pgoff_t)(pos >> PAGE_SHIFT), NULL);
		if (IS_ERR(fo)) {
			kvfree(buf);
			return NULL;
		}
		memcpy(buf + pos, fo->data, min_t(u64, PAGE_SIZE, sz - pos));
		folio_put(fo);
	}
	*size = sz;
	return buf;
}

/* Marks the clusters of one non-resident attribute extent as referenced. */
static void ngf_runs(struct ngf *f, u64 mft_no, struct attr_record *a)
{
	struct ntfs_volume *vol = f->vol;
	struct runlist_element *rl, *r;
	size_t cnt = 0;
	rl = ntfs_mapping_pairs_decompress(vol, a, NULL, &cnt);
	if (IS_ERR(rl)) {
		ngf_fatal(f, "record %llu: attribute 0x%x has bad mapping pairs", mft_no, le32_to_cpu(a->type));
		return;
	}
	for (r = rl; r && r->length; r++) {
		s64 c;
		if (r->lcn < 0)
			continue;
		if (r->lcn + r->length > vol->nr_clusters) {
			ngf_fatal(f, "record %llu: run %lld+%lld past the end of the volume", mft_no, r->lcn, r->length);
			continue;
		}
		f->r->clusters += r->length;
		if (!f->ref)
			continue;
		/* One finding of each kind per run; every cluster of the run is still marked referenced. */
		int twice = 0, free = 0;
		for (c = r->lcn; c < r->lcn + r->length; c++) {
			if (tbit(f->ref, c) && !twice++)
				ngf_fatal(f, "record %llu: cluster %lld referenced twice", mft_no, c);
			sbit(f->ref, c);
			if (!tbit(f->lcnbmp, c) && !free++)
				ngf_fatal(f, "record %llu: cluster %lld in use but free in $Bitmap", mft_no, c);
		}
	}
	kvfree(rl);
}

/* Validates one in-use record (fixups already applied) and walks its attributes. */
static void ngf_record(struct ngf *f, u64 mft_no, struct mft_record *m)
{
	struct ntfs_volume *vol = f->vol;
	u32 used = le32_to_cpu(m->bytes_in_use), ofs = le16_to_cpu(m->attrs_offset);
	if (ntfs_mft_record_check(vol, m, mft_no)) {
		ngf_fatal(f, "record %llu: corrupt header", mft_no);
		return;
	}
	while (ofs + 8 <= used) {
		struct attr_record *a = (struct attr_record *)((u8 *)m + ofs);
		u32 alen;
		if (a->type == AT_END)
			return;
		alen = le32_to_cpu(a->length);
		if (alen < 16 || (alen & 7) || ofs + alen > used) {
			ngf_fatal(f, "record %llu: attribute at %u has length %u", mft_no, ofs, alen);
			return;
		}
		if (a->non_resident) {
			if (alen < 64 || le16_to_cpu(a->data.non_resident.mapping_pairs_offset) >= alen) {
				ngf_fatal(f, "record %llu: non-resident attribute at %u is truncated", mft_no, ofs);
				return;
			}
			ngf_runs(f, mft_no, a);
		} else if (le16_to_cpu(a->data.resident.value_offset) + le32_to_cpu(a->data.resident.value_length) > alen) {
			ngf_fatal(f, "record %llu: resident value at %u overruns its attribute", mft_no, ofs);
			return;
		}
		ofs += alen;
	}
	ngf_fatal(f, "record %llu: no end marker", mft_no);
}

/* One index entry list (in the root or in an index block): every entry must name an in-use record. */
static void ngf_entries(struct ngf *f, u64 dir, struct index_header *ih, u32 room)
{
	u32 ofs = le32_to_cpu(ih->entries_offset), end = le32_to_cpu(ih->index_length);
	if (end > room || ofs > end) {
		ngf_fatal(f, "directory %llu: index header out of bounds", dir);
		return;
	}
	while (ofs + sizeof(struct index_entry_header) <= end) {
		struct index_entry *ie = (struct index_entry *)((u8 *)ih + ofs);
		u32 len = le16_to_cpu(ie->length);
		u64 t;
		u16 s;
		if (len < sizeof(struct index_entry_header) || ofs + len > end) {
			ngf_fatal(f, "directory %llu: index entry at %u has length %u", dir, ofs, len);
			return;
		}
		if (ie->flags & INDEX_ENTRY_END)
			return;
		t = MREF_LE(ie->data.dir.indexed_file);
		s = MSEQNO_LE(ie->data.dir.indexed_file);
		f->r->ientries++;
		if (t >= f->nrec || !f->seq[t])
			ngf_fatal(f, "directory %llu: entry names free record %llu", dir, t);
		else if (s && s != f->seq[t])
			ngf_fatal(f, "directory %llu: entry names record %llu seq %u, record has seq %u", dir, t, s, f->seq[t]);
		ofs += len;
	}
}

/* Checks the $I30 index of directory @dir (record @m, fixups applied). */
static void ngf_dir(struct ngf *f, u64 dir, struct mft_record *m)
{
	struct ntfs_volume *vol = f->vol;
	u32 used = le32_to_cpu(m->bytes_in_use), ofs = le16_to_cpu(m->attrs_offset);
	u32 ibs = 0, blen = 0;
	struct attr_record *alloc = NULL, *bmp = NULL;
	u8 *bits = NULL;
	while (ofs + 8 <= used) {
		struct attr_record *a = (struct attr_record *)((u8 *)m + ofs);
		if (a->type == AT_END || le32_to_cpu(a->length) < 16)
			break;
		if (a->name_length == 4 && a->type == AT_INDEX_ROOT && !a->non_resident) {
			struct index_root *ir = (struct index_root *)((u8 *)a + le16_to_cpu(a->data.resident.value_offset));
			u32 vlen = le32_to_cpu(a->data.resident.value_length);
			if (vlen < sizeof(*ir)) {
				ngf_fatal(f, "directory %llu: $INDEX_ROOT too small", dir);
				return;
			}
			ibs = le32_to_cpu(ir->index_block_size);
			ngf_entries(f, dir, &ir->index, vlen - offsetof(struct index_root, index));
		} else if (a->name_length == 4 && a->type == AT_INDEX_ALLOCATION && a->non_resident &&
			   !(s64)le64_to_cpu(a->data.non_resident.lowest_vcn)) {
			alloc = a;
		} else if (a->name_length == 4 && a->type == AT_BITMAP) {
			bmp = a;
		}
		ofs += le32_to_cpu(a->length);
	}
	if (alloc && ibs >= 512 && ibs <= 65536 && !(ibs & (ibs - 1))) {
		size_t cnt = 0;
		u64 size = le64_to_cpu(alloc->data.non_resident.data_size), pos;
		struct runlist_element *rl;
		if (le64_to_cpu(alloc->data.non_resident.highest_vcn) + 1 <
		    (s64)((le64_to_cpu(alloc->data.non_resident.allocated_size)) >> vol->cluster_size_bits)) {
			/* The allocation continues in extension records: its blocks are not walked here. */
			f->r->dirs_skipped++;
			return;
		}
		/* Only blocks whose bit is set in the index $BITMAP are in use; freed blocks keep stale entries. */
		if (!bmp) {
			ngf_fatal(f, "directory %llu: $INDEX_ALLOCATION without $BITMAP", dir);
			return;
		}
		if (!bmp->non_resident) {
			bits = (u8 *)bmp + le16_to_cpu(bmp->data.resident.value_offset);
			blen = le32_to_cpu(bmp->data.resident.value_length);
		} else {
			struct runlist_element *brl;
			u64 bsz = le64_to_cpu(bmp->data.non_resident.data_size);
			if (bsz > 65536 || (s64)le64_to_cpu(bmp->data.non_resident.lowest_vcn)) {
				f->r->dirs_skipped++;
				return;
			}
			brl = ntfs_mapping_pairs_decompress(vol, bmp, NULL, &cnt);
			if (IS_ERR(brl))
				return;
			bits = kmalloc(65536, GFP_KERNEL);
			if (!bits || ngf_read_rl(f, brl, 0, bits, (u32)bsz)) {
				kvfree(brl);
				kfree(bits);
				ngf_fatal(f, "directory %llu: index $BITMAP unreadable", dir);
				return;
			}
			kvfree(brl);
			blen = (u32)bsz;
		}
		rl = ntfs_mapping_pairs_decompress(vol, alloc, NULL, &cnt);
		if (IS_ERR(rl)) {
			if (bmp->non_resident)
				kfree(bits);
			return;		/* reported by the record pass */
		}
		for (pos = 0; pos + ibs <= size; pos += ibs) {
			struct index_block *ib = (struct index_block *)f->ib;
			u64 bn = pos / ibs;
			if (bn >= (u64)blen * 8 || !tbit(bits, bn))
				continue;
			if (ngf_read_rl(f, rl, pos, f->ib, ibs)) {
				ngf_fatal(f, "directory %llu: index block at %llu unreadable", dir, pos);
				break;
			}
			if (!ntfs_is_indx_record(ib->magic)) {
				ngf_fatal(f, "directory %llu: index block at %llu in use but has no INDX magic", dir, pos);
				continue;
			}
			if (post_read_mst_fixup((struct ntfs_record *)ib, ibs)) {
				ngf_fatal(f, "directory %llu: index block at %llu has bad fixups", dir, pos);
				continue;
			}
			ngf_entries(f, dir, &ib->index, ibs - offsetof(struct index_block, index));
		}
		kvfree(rl);
		if (bmp->non_resident)
			kfree(bits);
	}
}

int ngc_fsck(struct ntfs_volume *vol, struct block_device *b, struct ngc_fsck_res *r)
{
	struct ngf f = { .vol = vol, .b = b, .r = r };
	u64 mftsize = 0, lcnsize = 0, i, cbytes;
	u32 rs = vol->mft_record_size;
	struct ntfs_inode *mni = NTFS_I(vol->mft_ino);
	int pass, err = 0;
	long long s0, s1;
	long n0, n1;

	memset(r, 0, sizeof(*r));
	ngos_time(&s0, &n0);
	f.rec = kmalloc(rs, GFP_KERNEL);
	f.chunk = kmalloc(NGF_CHUNK, GFP_KERNEL);
	f.ib = kmalloc(65536, GFP_KERNEL);
	f.mftbmp = ngf_read_inode(&f, vol->mftbmp_ino, &f.mftbmp_bits);
	f.lcnbmp = ngf_read_inode(&f, vol->lcnbmp_ino, &lcnsize);
	if (!f.rec || !f.chunk || !f.ib || !f.mftbmp || !f.lcnbmp) {
		err = -ENOMEM;
		goto out;
	}
	f.mftbmp_bits *= 8;
	mftsize = (u64)i_size_read(vol->mft_ino);
	f.nrec = min_t(u64, mftsize / rs, f.mftbmp_bits);
	if (f.nrec > (4u << 20) || rs > NGF_CHUNK) {
		err = -EFBIG;
		goto out;
	}
	f.seq = kvmalloc((size_t)f.nrec * sizeof(u16), GFP_KERNEL);
	f.isdir = kvmalloc((size_t)(f.nrec + 7) / 8, GFP_KERNEL);
	if (!f.seq || !f.isdir) {
		err = -ENOMEM;
		goto out;
	}
	memset(f.seq, 0, (size_t)f.nrec * sizeof(u16));
	memset(f.isdir, 0, (size_t)(f.nrec + 7) / 8);
	cbytes = ((u64)vol->nr_clusters + 7) / 8;
	if (cbytes <= NGF_MAX_CBMP && lcnsize >= cbytes) {
		f.ref = kvmalloc((size_t)cbytes, GFP_KERNEL);
		if (f.ref)
			memset(f.ref, 0, (size_t)cbytes);
	}
	if (!f.ref)
		r->clusters_skipped = 1;
	down_write(&mni->runlist.lock);
	err = ntfs_attr_map_whole_runlist(mni);
	up_write(&mni->runlist.lock);
	if (err)
		goto out;
	/* Pass 0: sequence numbers of in-use records.  Pass 1: records, then directory indexes. */
	for (pass = 0; pass < 3 && !err; pass++) {
		for (i = 0; i < f.nrec; i += NGF_CHUNK / rs) {
			u64 n = min_t(u64, NGF_CHUNK / rs, f.nrec - i), k;
			/* Passes 0 and 2 need only chunks with records marked in use; pass 1 reads everything. */
			if (pass != 1) {
				for (k = 0; k < n && !tbit(f.mftbmp, i + k); k++)
					;
				if (k == n)
					continue;
			}
			down_read(&mni->runlist.lock);
			err = ngf_read_rl(&f, mni->runlist.rl, i * rs, f.chunk, (u32)(n * rs));
			up_read(&mni->runlist.lock);
			if (err) {
				ngf_fatal(&f, "$MFT unreadable at record %llu", i);
				break;
			}
			for (k = 0; k < n; k++) {
				u64 no = i + k;
				struct mft_record *m = (struct mft_record *)f.rec;
				int inbmp = tbit(f.mftbmp, no), inuse;
				memcpy(f.rec, f.chunk + k * rs, rs);
				inuse = ntfs_is_file_record(m->magic) && (m->flags & MFT_RECORD_IN_USE);
				if (!inuse && !inbmp)
					continue;
				if (inuse && post_read_mst_fixup((struct ntfs_record *)m, rs)) {
					if (pass == 1)
						ngf_fatal(&f, "record %llu: in use, bad fixups", no);
					continue;
				}
				if (pass == 0) {
					if (inuse)
						f.seq[no] = le16_to_cpu(m->sequence_number) ? le16_to_cpu(m->sequence_number) : 0xffff;
					if (inuse && (m->flags & MFT_RECORD_IS_DIRECTORY))
						sbit(f.isdir, no);
					continue;
				}
				if (pass == 1) {
					if (inuse && !inbmp) {
						ngf_fatal(&f, "record %llu: in use but free in $MFT:$BITMAP", no);
					} else if (!inuse) {
						if (no >= 24)
							ngf_leak(&f, "record %llu: marked in $MFT:$BITMAP but not in use", no);
						continue;
					}
					r->inuse++;
					ngf_record(&f, no, m);
					if (MREF_LE(m->base_mft_record)) {
						u64 base = MREF_LE(m->base_mft_record);
						if (base >= f.nrec || !f.seq[base])
							ngf_fatal(&f, "record %llu: extension of free record %llu", no, base);
					}
					continue;
				}
				if (tbit(f.isdir, no) && !MREF_LE(m->base_mft_record)) {
					r->dirs++;
					ngf_dir(&f, no, m);
				}
			}
			if (r->fatal > 1000)
				break;
		}
	}
	r->records = f.nrec;
	/* Clusters marked used that no record references: leaks. */
	if (!err && f.ref) {
		u64 c, run = 0, first = 0;
		for (c = 0; c < (u64)vol->nr_clusters; c++) {
			if (tbit(f.lcnbmp, c) && !tbit(f.ref, c)) {
				if (!run++)
					first = c;
			} else if (run) {
				r->leaked_clusters += run;
				ngf_leak(&f, "clusters %llu+%llu used in $Bitmap, referenced by no record", first, run);
				run = 0;
			}
		}
		if (run) {
			r->leaked_clusters += run;
			ngf_leak(&f, "clusters %llu+%llu used in $Bitmap, referenced by no record", first, run);
		}
	}
out:
	ngos_time(&s1, &n1);
	r->ms = (unsigned int)((s1 - s0) * 1000 + (n1 - n0) / 1000000);
	kvfree(f.ref);
	kvfree(f.seq);
	kvfree(f.isdir);
	kvfree(f.mftbmp);
	kvfree(f.lcnbmp);
	kfree(f.ib);
	kfree(f.chunk);
	kfree(f.rec);
	return err;
}
