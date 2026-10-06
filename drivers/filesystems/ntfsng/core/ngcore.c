// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ngcore.c - the fs/ntfs side of ngapi.h.  Drives the unmodified core the
 * way Linux's VFS does (fs_context mount, inode_operations->lookup,
 * file_operations->iterate_shared, address_space->read_folio through the
 * shim page cache) and hands the NT glue plain C values.  No NTFS format
 * logic lives here: writes go through the core's own attribute, cluster
 * allocation, index and MFT writeback functions, driven as the VFS would.
 */
#include <kshim.h>
#include "ntfs/ntfs.h"
#include "ntfs/attrib.h"
#include "ntfs/dir.h"
#include "ntfs/inode.h"
#include "ntfs/mft.h"
#include "ntfs/logfile.h"
#include "ntfs/lcnalloc.h"
#include "ngapi.h"

extern initcall_t kshim_module_init;
extern struct file_system_type *kshim_fs_type;
extern struct block_device *kshim_mount_bdev;
struct block_device *kshim_bdev_open(void *osdev, u64 size, unsigned int sector_size);
void kshim_bdev_close(struct block_device *b);
void kshim_mapping_shrink(struct address_space *m);
extern unsigned long kshim_pc_pages, kshim_inodes_live, kshim_counter_reads;
void kshim_dump_allocs(void);
int kshim_dev_rw(struct block_device *b, int write, u64 off, void *buf, size_t len);
int kshim_sync(struct super_block *sb);
bool kshim_sb_dirty(struct super_block *sb);
int kshim_mapping_writeback(struct address_space *m);
void kshim_mapping_update(struct address_space *m, loff_t pos, const void *buf, size_t len);
extern unsigned long kshim_counter_writes, kshim_counter_syncs, kshim_counter_dirty;
extern unsigned long long kshim_counter_write_bytes;

struct ngc_vol {
	struct super_block *sb;
	struct block_device *bdev;
	u8 *bounce;                     /* NGC_BOUNCE bytes of pool: device writes never touch caller memory */
};

#define NGC_BOUNCE (64 * 1024)

static DEFINE_MUTEX(ngc_mount_lock);
static int ngc_inited;

#define NT_EPOCH_DELTA 116444736000000000LL
static long long ts_to_nt(struct timespec64 t)
{
	return t.tv_sec * 10000000LL + t.tv_nsec / 100 + NT_EPOCH_DELTA;
}
static struct timespec64 nt_to_ts(long long nt)
{
	struct timespec64 t;
	long long u = nt - NT_EPOCH_DELTA;
	t.tv_sec = u / 10000000LL;
	t.tv_nsec = (long)(u % 10000000LL) * 100;
	if (t.tv_nsec < 0) {
		t.tv_nsec += 1000000000L;
		t.tv_sec--;
	}
	return t;
}

/* The core's own "remount-ro" error policy value, looked up by name. */
static int ngc_on_errors_remount_ro(void)
{
	for (const struct option_t *o = on_errors_arr; o->str; o++)
		if (!strcmp(o->str, "remount-ro"))
			return o->val;
	return 0;
}

/*
 * The core never checks that $LogFile was shut down cleanly before it empties it on a
 * read-write mount.  A restart area with a log client in use and without
 * RESTART_VOLUME_IS_CLEAN means another driver left transactions behind: stay read-only.
 */
static int ngc_logfile_clean(struct ntfs_volume *vol)
{
	struct restart_page_header *rp = NULL;
	struct restart_area *ra;
	int clean;
	if (!vol->logfile_ino)
		return 0;
	if (!ntfs_check_logfile(vol->logfile_ino, &rp))
		return 0;
	if (!rp)
		return 1;	/* empty log */
	ra = (struct restart_area *)((u8 *)rp + le16_to_cpu(rp->restart_area_offset));
	clean = ra->client_in_use_list == LOGFILE_NO_CLIENT || (ra->flags & RESTART_VOLUME_IS_CLEAN);
	kvfree(rp);
	return clean;
}

int ngc_init(void)
{
	int err = 0;
	mutex_lock(&ngc_mount_lock);
	if (!ngc_inited) {
		err = kshim_module_init();
		if (!err)
			ngc_inited = 1;
	}
	mutex_unlock(&ngc_mount_lock);
	return err;
}

int ngc_mount(void *osdev, unsigned long long size, unsigned int sector_size, int want_rw,
		ngc_vol **out, const char **why_ro)
{
	struct fs_context *fc;
	struct block_device *b;
	struct ngc_vol *v;
	struct ntfs_volume *vol;
	int err;

	*out = NULL;
	err = ngc_init();
	if (err)
		return err;
	v = kzalloc(sizeof(*v), GFP_KERNEL);
	fc = kzalloc(sizeof(*fc), GFP_KERNEL);
	b = kshim_bdev_open(osdev, size, sector_size);
	if (!v || !fc || !b) {
		err = -ENOMEM;
		goto fail;
	}
	fc->fs_type = kshim_fs_type;
	fc->sb_flags = SB_RDONLY;
	fc->purpose = FS_CONTEXT_FOR_MOUNT;
	fc->source = "ntfsng";
	err = kshim_fs_type->init_fs_context(fc);
	if (err)
		goto fail;
	vol = fc->s_fs_info;
	/* NT name lookups are case-insensitive; the core defaults to case-sensitive. */
	NVolClearCaseSensitive(vol);
	/* Errors turn the volume read-only instead of being ignored (the default "continue"). */
	vol->on_errors = ngc_on_errors_remount_ro();
	/* Growing a non-sparse file allocates real clusters, as NTFS does; holes only in sparse files. */
	NVolSetDisableSparse(vol);
	mutex_lock(&ngc_mount_lock);
	kshim_mount_bdev = b;
	err = fc->ops->get_tree(fc);
	kshim_mount_bdev = NULL;
	mutex_unlock(&ngc_mount_lock);
	if (err)
		goto fail_fc;
	v->sb = fc->kshim_sb;
	v->bdev = b;
	b->bd_super = v->sb;
	v->sb->s_flags |= SB_ACTIVE;
	*why_ro = want_rw ? NULL : "read-only requested";
	if (want_rw) {
		/* The core's own remount checks run in ntfs_reconfigure; these add what it skips. */
		if (NVolErrors(vol))
			*why_ro = "the core found errors at mount (MFTMirr, $LogFile or hibernation)";
		else if (vol->vol_flags & VOLUME_MUST_MOUNT_RO_MASK)
			*why_ro = (vol->vol_flags & VOLUME_IS_DIRTY) ? "volume is marked dirty" :
				"volume has flags that force read-only (chkdsk/upgrade/resize)";
		else if (!ngc_logfile_clean(vol))
			*why_ro = "$LogFile was not shut down cleanly";
		if (!*why_ro) {
			struct fs_context *rc = kzalloc(sizeof(*rc), GFP_KERNEL);
			if (!rc) {
				*why_ro = "out of memory";
			} else {
				rc->ops = fc->ops;
				rc->fs_type = fc->fs_type;
				rc->root = v->sb->s_root;
				rc->purpose = FS_CONTEXT_FOR_RECONFIGURE;
				rc->sb_flags = 0;
				b->kshim_remounting = 1;
				err = fc->ops->reconfigure(rc);
				b->kshim_remounting = 0;
				kfree(rc);
				if (err)
					*why_ro = "the core refused to remount read-write";
				else
					v->sb->s_flags &= ~SB_RDONLY;
			}
		}
	}
	if (!sb_rdonly(v->sb)) {
		v->bounce = kmalloc(NGC_BOUNCE, GFP_KERNEL);
		if (!v->bounce) {
			v->sb->s_flags |= SB_RDONLY;
			*why_ro = "out of memory";
		}
	}
	if (fc->ops->free)
		fc->ops->free(fc);
	kfree(fc);
	*out = v;
	return 0;
fail_fc:
	if (fc->ops && fc->ops->free)
		fc->ops->free(fc);
fail:
	kfree(fc);
	kshim_bdev_close(b);
	kfree(v);
	return err;
}

void ngc_umount(ngc_vol *v)
{
	struct super_block *sb = v->sb;
	if (sb->s_root) {
		iput(sb->s_root->d_inode);
		kfree(sb->s_root);
		sb->s_root = NULL;
	}
	if (sb->s_op->put_super)
		sb->s_op->put_super(sb);
	kfree(sb);
	kshim_bdev_close(v->bdev);
	kfree(v);
}

void ngc_volinfo(ngc_vol *v, struct ngc_volinfo *vi)
{
	struct ntfs_volume *vol = NTFS_SB(v->sb);
	s64 fc = atomic64_read(&vol->free_clusters);
	memset(vi, 0, sizeof(*vi));
	vi->total_clusters = vol->nr_clusters;
	vi->free_clusters = fc > 0 ? fc : 0;
	vi->serial = vol->serial_no;
	vi->cluster_size = vol->cluster_size;
	vi->sector_size = vol->sector_size;
	vi->major = vol->major_ver;
	vi->minor = vol->minor_ver;
	vi->read_only = sb_rdonly(v->sb) ? 1 : 0;
	vi->dirty = (vol->vol_flags & VOLUME_IS_DIRTY) ? 1 : 0;
	mutex_lock(&vol->volume_label_lock);
	if (vol->volume_label) {
		int n = utf8s_to_utf16s(vol->volume_label, strlen((char *)vol->volume_label),
				UTF16_LITTLE_ENDIAN, vi->label, 64);
		vi->label_len = n > 0 ? n : 0;
	}
	mutex_unlock(&vol->volume_label_lock);
}

ngc_node *ngc_root(ngc_vol *v)
{
	return (ngc_node *)igrab(d_inode(v->sb->s_root));
}

int ngc_lookup(ngc_vol *v, ngc_node *dirn, const unsigned short *name, unsigned int len, ngc_node **out)
{
	struct inode *dir = (struct inode *)dirn, *vi;
	struct dentry *d, *r;
	u8 *u8name;
	int n;

	*out = NULL;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	if (!len || len > NTFS_MAX_NAME_LEN)
		return -ENAMETOOLONG;
	u8name = kmalloc(len * 4 + 1, GFP_NOFS);
	d = kzalloc(sizeof(*d), GFP_NOFS);
	if (!u8name || !d) {
		kfree(u8name);
		kfree(d);
		return -ENOMEM;
	}
	n = utf16s_to_utf8s(name, len, UTF16_LITTLE_ENDIAN, u8name, len * 4);
	u8name[n > 0 ? n : 0] = 0;
	d->d_name.name = u8name;
	d->d_name.len = n > 0 ? n : 0;
	d->d_sb = v->sb;
	d->d_parent = d;
	r = dir->i_op->lookup(dir, d, 0);
	if (IS_ERR(r)) {
		kfree(u8name);
		kfree(d);
		return PTR_ERR(r);
	}
	vi = r ? r->d_inode : d->d_inode;
	kfree(u8name);
	kfree(d->kshim_ci_name);
	kfree(d);
	if (!vi)
		return -ENOENT;
	*out = (ngc_node *)vi;
	return 0;
}

int ngc_open_stream(ngc_vol *v, ngc_node *basen, const unsigned short *sname, unsigned int len, ngc_node **out)
{
	struct inode *base = (struct inode *)basen, *vi;
	__le16 *uname;
	(void)v;
	*out = NULL;
	if (len > NTFS_MAX_NAME_LEN)
		return -ENAMETOOLONG;
	uname = kmalloc((len + 1) * sizeof(__le16), GFP_NOFS);
	if (!uname)
		return -ENOMEM;
	memcpy(uname, sname, len * sizeof(__le16));
	uname[len] = 0;
	vi = ntfs_attr_iget(base, AT_DATA, uname, len);
	kfree(uname);
	if (IS_ERR(vi))
		return PTR_ERR(vi);
	*out = (ngc_node *)vi;
	return 0;
}

int ngc_iget(ngc_vol *v, unsigned long long mft_no, ngc_node **out)
{
	struct inode *vi = ntfs_iget(v->sb, mft_no);
	*out = NULL;
	if (IS_ERR(vi))
		return PTR_ERR(vi);
	*out = (ngc_node *)vi;
	return 0;
}

void ngc_put(ngc_node *n)
{
	iput((struct inode *)n);
}

void ngc_stat(ngc_node *n, struct ngc_stat *st)
{
	struct inode *vi = (struct inode *)n, *bvi = vi;
	struct ntfs_inode *ni = NTFS_I(vi), *bni = ni;
	unsigned long flags;

	if (NInoAttr(ni)) {
		bni = ni->ext.base_ntfs_ino;
		bvi = VFS_I(bni);
	}
	memset(st, 0, sizeof(*st));
	st->mft_ref = bni->mft_no | ((u64)bni->seq_no << 48);
	read_lock_irqsave(&ni->size_lock, flags);
	st->size = i_size_read(vi);
	if (NInoNonResident(ni)) {
		if (NInoCompressed(ni) || NInoSparse(ni))
			st->alloc = ni->itype.compressed.size;
		else
			st->alloc = ni->allocated_size;
	} else {
		st->alloc = (st->size + 7) & ~7ULL;
	}
	read_unlock_irqrestore(&ni->size_lock, flags);
	st->crtime = ts_to_nt(bni->i_crtime);
	st->atime = ts_to_nt(inode_get_atime(bvi));
	st->mtime = ts_to_nt(inode_get_mtime(bvi));
	st->ctime = ts_to_nt(inode_get_ctime(bvi));
	st->file_attributes = le32_to_cpu(bni->flags) & 0xffff;
	st->nlink = bvi->i_nlink;
	st->is_dir = S_ISDIR(bvi->i_mode) && vi == bvi;
	st->is_link = S_ISLNK(bvi->i_mode);
	if (NInoCompressed(ni)) st->flags |= NGC_ATTR_COMPRESSED;
	if (NInoSparse(ni)) st->flags |= NGC_ATTR_SPARSE;
	if (NInoEncrypted(ni)) st->flags |= NGC_ATTR_ENCRYPTED;
}

struct ngc_dirctx {
	struct dir_context ctx;
	ngc_filldir_t fn;
	void *arg;
	u16 *buf;
	int stop, n;
};

static bool ngc_actor(struct dir_context *c, const char *name, int len, loff_t pos, u64 ino, unsigned type)
{
	struct ngc_dirctx *d = container_of(c, struct ngc_dirctx, ctx);
	int ulen = utf8s_to_utf16s((const u8 *)name, len, UTF16_LITTLE_ENDIAN, d->buf, NTFS_MAX_NAME_LEN + 2);
	(void)pos;
	d->n++;
	if (ulen <= 0)
		return true;
	if (d->fn(d->arg, d->buf, ulen, ino, type)) {
		d->stop = 1;
		return false;
	}
	return true;
}

int ngc_readdir(ngc_vol *v, ngc_node *dirn, ngc_filldir_t fn, void *arg)
{
	struct inode *dir = (struct inode *)dirn;
	struct file *f;
	struct ngc_dirctx *d;
	int err = 0;
	(void)v;

	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	f = kzalloc(sizeof(*f), GFP_NOFS);
	d = kzalloc(sizeof(*d), GFP_NOFS);
	if (!f || !d || !(d->buf = kmalloc((NTFS_MAX_NAME_LEN + 2) * sizeof(u16), GFP_NOFS))) {
		if (d) kfree(d->buf);
		kfree(d);
		kfree(f);
		return -ENOMEM;
	}
	f->f_inode = dir;
	f->f_mapping = dir->i_mapping;
	f->f_mode = FMODE_READ;
	d->ctx.actor = ngc_actor;
	d->fn = fn;
	d->arg = arg;
	if (dir->i_fop->open && (err = dir->i_fop->open(dir, f)))
		goto out;
	/* Drive iterate_shared the way getdents() does, until it makes no progress. */
	for (;;) {
		loff_t before = d->ctx.pos;
		int n = d->n;
		err = dir->i_fop->iterate_shared(f, &d->ctx);
		if (err || d->stop || (d->ctx.pos == before && d->n == n))
			break;
	}
	if (dir->i_fop->release)
		dir->i_fop->release(dir, f);
out:
	kfree(d->buf);
	kfree(d);
	kfree(f);
	return err;
}

int ngc_streams(ngc_node *n, ngc_stream_t fn, void *arg)
{
	struct inode *vi = (struct inode *)n;
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_attr_search_ctx *ctx;
	struct mft_record *m;
	int err = 0;

	if (NInoAttr(ni))
		return -EINVAL;
	m = map_mft_record(ni);
	if (IS_ERR(m))
		return PTR_ERR(m);
	ctx = ntfs_attr_get_search_ctx(ni, m);
	if (!ctx) {
		unmap_mft_record(ni);
		return -ENOMEM;
	}
	/* NULL name means "unnamed" in this driver, so walk all attributes (as file.c does). */
	while (!ntfs_attr_lookup(AT_UNUSED, NULL, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
		struct attr_record *a = ctx->attr;
		u64 size, alloc;
		if (a->type != AT_DATA)
			continue;
		if (a->non_resident && a->data.non_resident.lowest_vcn)
			continue;
		if (a->non_resident) {
			size = le64_to_cpu(a->data.non_resident.data_size);
			alloc = le64_to_cpu(a->data.non_resident.allocated_size);
		} else {
			size = le32_to_cpu(a->data.resident.value_length);
			alloc = (size + 7) & ~7ULL;
		}
		if (fn(arg, (const unsigned short *)((u8 *)a + le16_to_cpu(a->name_offset)), a->name_length, size, alloc)) {
			err = 1;
			break;
		}
	}
	ntfs_attr_put_search_ctx(ctx);
	unmap_mft_record(ni);
	return err < 0 ? err : 0;
}

long ngc_read(ngc_node *n, unsigned long long off, unsigned int len, void *buf, int drop_cache)
{
	struct inode *vi = (struct inode *)n;
	loff_t size = i_size_read(vi);
	u8 *out = buf;
	unsigned int done = 0;

	while (done < len) {
		u64 pos = off + done;
		unsigned int in_page = pos & (PAGE_SIZE - 1);
		unsigned int chunk = min_t(unsigned int, PAGE_SIZE - in_page, len - done);
		struct folio *f;
		if ((loff_t)pos >= size) {
			memset(out + done, 0, len - done);
			done = len;
			break;
		}
		f = read_mapping_folio(vi->i_mapping, (pgoff_t)(pos >> PAGE_SHIFT), NULL);
		if (IS_ERR(f))
			return PTR_ERR(f);
		memcpy(out + done, (u8 *)f->data + in_page, chunk);
		folio_put(f);
		done += chunk;
	}
	/* The NT Cache Manager holds file data; keep only a bounded window here. */
	if (drop_cache && vi->i_mapping->nrpages > 256)
		kshim_mapping_shrink(vi->i_mapping);
	return done;
}

void ngc_stats(unsigned long *pages, unsigned long *inodes, unsigned long *reads)
{
	*pages = kshim_pc_pages;
	*inodes = kshim_inodes_live;
	*reads = kshim_counter_reads;
}

void ngc_trim(ngc_node *n)
{
	kshim_mapping_shrink(((struct inode *)n)->i_mapping);
}

void ngc_debug_dump(void)
{
	printk("shim: %lu cache pages, %lu live inodes, %lu device reads\n",
			kshim_pc_pages, kshim_inodes_live, kshim_counter_reads);
	kshim_dump_allocs();
}

/* ------------------------------------------------------------------ write side */

int ngc_is_rw(ngc_vol *v)
{
	return !sb_rdonly(v->sb);
}

/*
 * Sets VOLUME_IS_DIRTY before the first change after a clean point, and puts it on disk
 * (with a device flush) before the change proceeds, so a crash always leaves it behind.
 */
int ngc_mark_dirty(ngc_vol *v)
{
	struct ntfs_volume *vol = NTFS_SB(v->sb);
	int err;
	if (sb_rdonly(v->sb))
		return -EROFS;
	if (vol->vol_flags & VOLUME_IS_DIRTY)
		return 0;
	err = ntfs_set_volume_flags(vol, VOLUME_IS_DIRTY);
	if (!err)
		err = write_inode_now(vol->vol_ino, 1);
	if (!err)
		err = blkdev_issue_flush(v->bdev);
	return err;
}

/*
 * The flusher: writes back every dirty mapping and inode, then (when nothing is left and the
 * core saw no error) clears VOLUME_IS_DIRTY, so the flag on disk means "unsynced changes".
 */
int ngc_sync(ngc_vol *v)
{
	struct ntfs_volume *vol = NTFS_SB(v->sb);
	int err;
	if (sb_rdonly(v->sb))
		return 0;
	err = kshim_sync(v->sb);
	if (!err)
		err = blkdev_issue_flush(v->bdev);
	if (!err && !NVolErrors(vol) && (vol->vol_flags & VOLUME_IS_DIRTY) && !kshim_sb_dirty(v->sb)) {
		err = ntfs_clear_volume_flags(vol, VOLUME_IS_DIRTY);
		if (!err)
			err = kshim_sync(v->sb);
		if (!err)
			err = blkdev_issue_flush(v->bdev);
	}
	return err;
}

int ngc_dirty(ngc_vol *v)
{
	struct ntfs_volume *vol = NTFS_SB(v->sb);
	if (sb_rdonly(v->sb))
		return 0;
	return kshim_sb_dirty(v->sb) || (vol->vol_flags & VOLUME_IS_DIRTY);
}

static int ngc_dev_write(struct ngc_vol *v, u64 off, const u8 *buf, u64 len)
{
	while (len) {
		size_t n = (size_t)min_t(u64, len, NGC_BOUNCE);
		int err;
		if (buf)
			memcpy(v->bounce, buf, n);
		else
			memset(v->bounce, 0, n);
		err = kshim_dev_rw(v->bdev, 1, off, v->bounce, n);
		if (err)
			return err;
		off += n;
		len -= n;
		if (buf)
			buf += n;
	}
	return 0;
}

/* Writes [pos, pos+len) of a non-resident attribute in place; buf NULL writes zeros. */
static int ngc_nr_write(struct ngc_vol *v, struct inode *vi, u64 pos, u64 len, const u8 *buf)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;
	u32 bits = vol->cluster_size_bits;
	u64 cs = vol->cluster_size;
	while (len) {
		s64 vcn = pos >> bits, lcn = 0, cnt = 0;
		u64 vofs = pos & (cs - 1), n, dev;
		s64 maxc = (s64)((vofs + len + cs - 1) >> bits);
		bool balloc = false;
		int err;
		mutex_lock(&ni->mrec_lock);
		down_write(&ni->runlist.lock);
		err = ntfs_attr_map_cluster(ni, vcn, &lcn, &cnt, maxc, &balloc, true, false);
		up_write(&ni->runlist.lock);
		mutex_unlock(&ni->mrec_lock);
		if (err)
			return err;
		if (lcn < 0 || cnt <= 0)
			return -EIO;
		n = min_t(u64, len, ((u64)cnt << bits) - vofs);
		dev = ((u64)lcn << bits) + vofs;
		if (balloc) {
			/* Fresh clusters for a hole: zero what this write does not cover. */
			u64 first = (u64)lcn << bits, end = vofs + n;
			u64 last = (u64)lcn << bits;
			last += (end + cs - 1) & ~(cs - 1);
			if (vofs && (err = ngc_dev_write(v, first, NULL, vofs)))
				return err;
			if (dev + n < last && (err = ngc_dev_write(v, dev + n, NULL, last - (dev + n))))
				return err;
		}
		err = ngc_dev_write(v, dev, buf, n);
		if (err)
			return err;
		pos += n;
		len -= n;
		if (buf)
			buf += n;
	}
	return 0;
}

static int ngc_res_write(struct inode *vi, u64 pos, u64 len, const u8 *buf)
{
	struct ntfs_inode *ni = NTFS_I(vi), *base = NInoAttr(ni) ? ni->ext.base_ntfs_ino : ni;
	struct ntfs_attr_search_ctx *ctx;
	int err;
	mutex_lock(&base->mrec_lock);
	ctx = ntfs_attr_get_search_ctx(base, NULL);
	if (!ctx) {
		mutex_unlock(&base->mrec_lock);
		return -ENOMEM;
	}
	err = ntfs_attr_lookup(ni->type, ni->name, ni->name_len, CASE_SENSITIVE, 0, NULL, 0, ctx);
	if (!err) {
		u8 *val = (u8 *)ctx->attr + le16_to_cpu(ctx->attr->data.resident.value_offset);
		u32 vlen = le32_to_cpu(ctx->attr->data.resident.value_length);
		if (ctx->attr->non_resident || pos + len > vlen) {
			err = -EIO;
		} else {
			memcpy(val + pos, buf, len);
			mark_mft_record_dirty(ctx->ntfs_ino);
		}
	}
	ntfs_attr_put_search_ctx(ctx);
	mutex_unlock(&base->mrec_lock);
	return err;
}

/*
 * The NT data path (non-cached and paging writes): in-place write of [off, off+len) clipped to
 * the stream size.  Returns the bytes written (0 past EOF) or <0.
 */
long ngc_write(ngc_vol *v, ngc_node *n, unsigned long long off, unsigned int len, const void *buf)
{
	struct inode *vi = (struct inode *)n;
	struct ntfs_inode *ni = NTFS_I(vi);
	loff_t size = i_size_read(vi);
	u64 end;
	int err;

	if (sb_rdonly(v->sb))
		return -EROFS;
	if ((loff_t)off >= size || !len)
		return 0;
	if (off + len > (u64)size)
		len = (unsigned int)(size - off);
	end = off + len;
	if (NInoCompressed(ni) || NInoEncrypted(ni) || NInoWofCompressed(ni))
		return -EOPNOTSUPP;
	err = ngc_mark_dirty(v);
	if (err)
		return err;
	if (!NInoNonResident(ni)) {
		err = ngc_res_write(vi, off, len, buf);
	} else {
		loff_t init = ni->initialized_size;
		if ((loff_t)off > init)
			err = ngc_nr_write(v, vi, init, off - init, NULL);
		if (!err)
			err = ngc_nr_write(v, vi, off, len, buf);
		if (!err && (loff_t)end > ni->initialized_size) {
			mutex_lock(&ni->mrec_lock);
			err = ntfs_attr_set_initialized_size(ni, end);
			mutex_unlock(&ni->mrec_lock);
		}
	}
	if (err)
		return err;
	kshim_mapping_update(vi->i_mapping, off, buf, len);
	return len;
}

/* EOF change through the core (resident resize, conversion to non-resident, expand, shrink). */
int ngc_set_size(ngc_vol *v, ngc_node *n, unsigned long long newsize)
{
	struct inode *vi = (struct inode *)n;
	struct ntfs_inode *ni = NTFS_I(vi);
	loff_t old = i_size_read(vi);
	int err;

	if (sb_rdonly(v->sb))
		return -EROFS;
	if ((loff_t)newsize == old)
		return 0;
	if (NInoCompressed(ni) || NInoEncrypted(ni) || NInoWofCompressed(ni))
		return -EOPNOTSUPP;
	err = ngc_mark_dirty(v);
	if (err)
		return err;
	if ((loff_t)newsize > old) {
		mutex_lock(&ni->mrec_lock);
		err = ntfs_attr_expand(ni, newsize, 0);
		mutex_unlock(&ni->mrec_lock);
	} else {
		truncate_setsize(vi, newsize);
		err = ntfs_truncate_vfs(vi, newsize, old);
		if (err)
			i_size_write(vi, old);
	}
	/* A resident->non-resident conversion left the old value in a dirty page-0 folio. */
	if (kshim_mapping_writeback(vi->i_mapping) && !err)
		err = -EIO;
	mark_inode_dirty(VFS_I(NInoAttr(ni) ? ni->ext.base_ntfs_ino : ni));
	return err;
}

#define NGC_SETTABLE_ATTRS (FILE_ATTR_READONLY | FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM | FILE_ATTR_ARCHIVE | \
			    FILE_ATTR_TEMPORARY | FILE_ATTR_OFFLINE | FILE_ATTR_NOT_CONTENT_INDEXED)

/*
 * Times (NT format; 0 keeps a time, -1 means "now") and $STANDARD_INFORMATION attributes
 * (attrs_mask selects which settable bits change).  Written back by write_inode.
 */
int ngc_set_info(ngc_vol *v, ngc_node *n, const long long times[4], unsigned int attrs, unsigned int attrs_mask)
{
	struct inode *vi = (struct inode *)n;
	struct ntfs_inode *ni = NTFS_I(vi);
	struct timespec64 now = current_time(vi), t;
	int err;

	if (NInoAttr(ni)) {
		ni = ni->ext.base_ntfs_ino;
		vi = VFS_I(ni);
	}
	if (sb_rdonly(v->sb))
		return -EROFS;
	err = ngc_mark_dirty(v);
	if (err)
		return err;
	if (times) {
#define NGC_T(x) (t = (x) == -1 ? now : nt_to_ts(x))
		if (times[0]) ni->i_crtime = NGC_T(times[0]);
		if (times[1]) inode_set_atime_to_ts(vi, NGC_T(times[1]));
		if (times[2]) inode_set_mtime_to_ts(vi, NGC_T(times[2]));
		if (times[3]) inode_set_ctime_to_ts(vi, NGC_T(times[3]));
#undef NGC_T
	}
	attrs_mask &= le32_to_cpu(NGC_SETTABLE_ATTRS);
	if (attrs_mask)
		ni->flags = (ni->flags & ~cpu_to_le32(attrs_mask)) | cpu_to_le32(attrs & attrs_mask);
	NInoSetFileNameDirty(ni);
	mark_inode_dirty(vi);
	return 0;
}

void ngc_write_stats(unsigned long *writes, unsigned long long *bytes, unsigned long *syncs, unsigned long *dirties)
{
	*writes = kshim_counter_writes;
	*bytes = kshim_counter_write_bytes;
	*syncs = kshim_counter_syncs;
	*dirties = kshim_counter_dirty;
}

