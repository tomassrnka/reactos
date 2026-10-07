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
#include <kshim_jnl.h>
#include "ngjrec.h"

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
extern bool (*kshim_is_data_inode)(struct inode *i);

struct ngc_vol {
	struct super_block *sb;
	struct block_device *bdev;
	u8 *bounce;                     /* NGC_BOUNCE bytes of pool: device writes never touch caller memory */
	struct ngj_vol *jv;             /* raw $LogFile location while the metadata journal is in use */
	atomic64_t *watched;            /* the core's free-cluster count, watched for frees */
	unsigned long frees_seen;       /* its increments at the last commit */
};

/* Pages held by the journal overlay that make the next operation commit first. */
#define NGC_JNL_COMMIT_PAGES 2048

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

/* One of the core's error policy values ("remount-ro", "continue"), looked up by name. */
static int ngc_on_errors(const char *name)
{
	for (const struct option_t *o = on_errors_arr; o->str; o++)
		if (!strcmp(o->str, name))
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

/* File data goes in place without the journal: $DATA of anything but the system files. */
static bool ngc_is_data_inode(struct inode *i)
{
	struct ntfs_inode *ni = NTFS_I(i);
	return ni->type == AT_DATA && ni->mft_no >= FILE_first_user;
}

int ngc_init(void)
{
	int err = 0;
	mutex_lock(&ngc_mount_lock);
	if (!ngc_inited) {
		err = kshim_module_init();
		if (!err)
			ngc_inited = 1;
		kshim_is_data_inode = ngc_is_data_inode;
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
	int err, jrec = NGJ_NONE;
	u64 jseq = 0;

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
	/*
	 * Before the core reads anything: a journal left by a crash goes in place first (read-only
	 * mounts see it through the overlay instead).
	 */
	v->jv = kmalloc(sizeof(*v->jv), GFP_KERNEL);
	if (v->jv && !ngj_probe(osdev, size, sector_size, v->jv) && v->jv->lf_pages >= 256) {
		jrec = ngj_recover(v->jv, b, want_rw, &jseq);
		if (!want_rw) {
			kfree(v->jv);
			v->jv = NULL;
		} else {
			/* Test only: a mount that had to recover does not inject the fault again. */
			if (v->jv->replayed || v->jv->torn)
				kshim_jnl_fault = 0;
		}
	} else {
		if (want_rw)
			printk(KERN_WARNING "journal: $LogFile not usable for the journal; metadata goes in place\n");
		kfree(v->jv);
		v->jv = NULL;
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
	/* During the mount, errors keep the volume read-only (the core's mount-time checks use this policy). */
	vol->on_errors = ngc_on_errors("remount-ro");
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
		if (jrec == NGJ_REPAIR)
			*why_ro = "the journal shows metadata written in place without it (needs repair)";
		else if (NVolErrors(vol))
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
	if (!sb_rdonly(v->sb) && v->jv) {
		/* The core's own mount-time writes ($LogFile emptied) go in place before the journal starts. */
		int jerr = kshim_sync(v->sb);
		if (!jerr)
			jerr = blkdev_issue_flush(b);
		if (vol->logfile_ino)
			truncate_inode_pages(vol->logfile_ino->i_mapping, 0);
		if (!jerr)
			jerr = kshim_jnl_activate(b, v->jv->ext, v->jv->next, v->jv->lf_pages, v->jv->serial, jseq + 1);
		if (jerr) {
			printk(KERN_ERR "journal: not active (%d); metadata goes in place\n", jerr);
		} else if (!kshim_watch_add(&vol->free_clusters)) {
			v->watched = &vol->free_clusters;
			v->frees_seen = kshim_watch_count(v->watched);
		}
	}
	/*
	 * Once mounted, an error fails the operation and leaves the volume writable, as on Windows:
	 * with remount-ro, running out of space while growing the MFT made the system volume
	 * read-only.  Errors that mean corruption set NVolErrors, which keeps the dirty flag and
	 * makes the journal header demand a repair.
	 */
	if (!sb_rdonly(v->sb))
		vol->on_errors = ngc_on_errors("continue");
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
	if (v)
		kfree(v->jv);
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
	if (v->bdev->jnl && !NVolErrors(NTFS_SB(sb)))
		kshim_jnl_commit(v->bdev);
	if (v->watched)
		kshim_watch_del(v->watched);
	kfree(v->jv);
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

/* A dentry for @name (UTF-16) in the shape the core's inode operations expect; @inode may be NULL. */
static struct dentry *ngc_mkdentry(ngc_vol *v, const unsigned short *name, unsigned int len, struct inode *inode)
{
	struct dentry *d;
	u8 *u8name;
	int n;
	if (!len || len > NTFS_MAX_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);
	u8name = kmalloc(len * 4 + 1, GFP_NOFS);
	d = kzalloc(sizeof(*d), GFP_NOFS);
	if (!u8name || !d) {
		kfree(u8name);
		kfree(d);
		return ERR_PTR(-ENOMEM);
	}
	n = utf16s_to_utf8s(name, len, UTF16_LITTLE_ENDIAN, u8name, len * 4);
	u8name[n > 0 ? n : 0] = 0;
	d->d_name.name = u8name;
	d->d_name.len = n > 0 ? n : 0;
	d->d_sb = v->sb;
	d->d_parent = d;
	d->d_inode = inode;
	return d;
}

static void ngc_freedentry(struct dentry *d)
{
	if (IS_ERR_OR_NULL(d))
		return;
	kfree(d->d_name.name);
	kfree(d->kshim_ci_name);
	kfree(d);
}

int ngc_lookup(ngc_vol *v, ngc_node *dirn, const unsigned short *name, unsigned int len, ngc_node **out,
		unsigned short *real, unsigned int *real_len)
{
	struct inode *dir = (struct inode *)dirn, *vi;
	struct dentry *d, *r;

	*out = NULL;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	d = ngc_mkdentry(v, name, len, NULL);
	if (IS_ERR(d))
		return PTR_ERR(d);
	r = dir->i_op->lookup(dir, d, 0);
	if (IS_ERR(r)) {
		ngc_freedentry(d);
		return PTR_ERR(r);
	}
	vi = r ? r->d_inode : d->d_inode;
	if (vi && real && real_len) {
		/* The name as stored: the case-folded match, or exactly what was asked. */
		if (d->kshim_ci_name) {
			int n = utf8s_to_utf16s(d->kshim_ci_name, d->kshim_ci_len, UTF16_LITTLE_ENDIAN,
					real, NTFS_MAX_NAME_LEN);
			*real_len = n > 0 ? n : 0;
		} else {
			memcpy(real, name, len * sizeof(*real));
			*real_len = len;
		}
	}
	ngc_freedentry(d);
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
 * A consistency point: every dirty mapping and inode is written back, then (journal active)
 * the transaction is committed and written in place, or (no journal) the device is flushed.
 * Callers hold the volume lock between core operations, so the state written is whole.
 */
static int ngc_commit(struct ngc_vol *v)
{
	int err = kshim_sync(v->sb);
	for (int k = 0; !err && k < 4 && kshim_sb_dirty(v->sb); k++)
		err = kshim_sync(v->sb);
	if (!v->bdev->jnl)
		return err ? err : blkdev_issue_flush(v->bdev);
	if (NVolErrors(NTFS_SB(v->sb)))
		kshim_jnl_mark_errors(v->bdev);
	if (!err && kshim_sb_dirty(v->sb))
		printk(KERN_ERR "journal: metadata still dirty after writeback; committing what was written\n");
	if (!err)
		err = kshim_jnl_commit(v->bdev);
	if (!err)
		err = blkdev_issue_flush(v->bdev);	/* nothing was committed: data writes still reach the medium */
	if (err > 0)
		err = 0;
	if (!err && v->watched)
		v->frees_seen = kshim_watch_count(v->watched);
	return err;
}

/* Overlay pages that make the next operation commit first: half the journal, at most NGC_JNL_COMMIT_PAGES. */
static unsigned long ngc_commit_threshold(struct ngc_vol *v)
{
	return min_t(unsigned long, NGC_JNL_COMMIT_PAGES, kshim_jnl_capacity(v->bdev) / 2);
}

/*
 * File data is written in place at once, so a cluster freed by an uncommitted transaction
 * must not receive data before that transaction commits: a crash would hand the old owner
 * the new data.  Operations that allocate data clusters commit first when anything was freed.
 */
static int ngc_before_alloc(struct ngc_vol *v)
{
	if (v->bdev->jnl && (!v->watched || kshim_watch_count(v->watched) != v->frees_seen))
		return ngc_commit(v);
	return 0;
}

/*
 * Sets VOLUME_IS_DIRTY before the first change after a clean point, and puts it on disk
 * before the change proceeds, so a crash always leaves it behind.  Also commits when the
 * journal overlay has grown large.
 */
int ngc_mark_dirty(ngc_vol *v)
{
	struct ntfs_volume *vol = NTFS_SB(v->sb);
	int err;
	if (sb_rdonly(v->sb))
		return -EROFS;
	if (vol->vol_flags & VOLUME_IS_DIRTY)
		return kshim_jnl_pending(v->bdev) > ngc_commit_threshold(v) ? ngc_commit(v) : 0;
	err = ntfs_set_volume_flags(vol, VOLUME_IS_DIRTY);
	if (!err)
		err = write_inode_now(vol->vol_ino, 1);
	if (!err)
		err = ngc_commit(v);
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
	if (NVolErrors(vol))
		kshim_jnl_mark_errors(v->bdev);
	if (sb_rdonly(v->sb))
		return 0;
	err = ngc_commit(v);
	if (!err && !NVolErrors(vol) && (vol->vol_flags & VOLUME_IS_DIRTY) && !kshim_sb_dirty(v->sb)) {
		err = ntfs_clear_volume_flags(vol, VOLUME_IS_DIRTY);
		if (!err)
			err = ngc_commit(v);
	}
	return err;
}

int ngc_dirty(ngc_vol *v)
{
	struct ntfs_volume *vol = NTFS_SB(v->sb);
	if (NVolErrors(vol))
		kshim_jnl_mark_errors(v->bdev);
	if (sb_rdonly(v->sb))
		return 0;
	return kshim_sb_dirty(v->sb) || (vol->vol_flags & VOLUME_IS_DIRTY) || kshim_jnl_pending(v->bdev);
}

void ngc_jnl_report(ngc_vol *v)
{
	kshim_jnl_report(v->bdev);
}

void ngc_set_journal_fault(unsigned long commit_no)
{
	kshim_jnl_fault = commit_no;
}

static int ngc_dev_write(struct ngc_vol *v, u64 off, const u8 *buf, u64 len)
{
	int err = 0;
	v->bdev->kshim_direct++;
	while (len && !err) {
		size_t n = (size_t)min_t(u64, len, NGC_BOUNCE);
		if (buf)
			memcpy(v->bounce, buf, n);
		else
			memset(v->bounce, 0, n);
		err = kshim_dev_rw(v->bdev, 1, off, v->bounce, n);
		off += n;
		len -= n;
		if (buf)
			buf += n;
	}
	v->bdev->kshim_direct--;
	return err;
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
		if (err) {
			printk(KERN_ERR "ngc: map of ino %llu vcn %lld failed %d (size %lld, alloc %lld, init %lld)\n",
			       (unsigned long long)ni->mft_no, (long long)vcn, err, (long long)ni->data_size,
			       (long long)ni->allocated_size, (long long)ni->initialized_size);
			return err;
		}
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
	if (!err)
		err = ngc_before_alloc(v);
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
	if (!err && (loff_t)newsize > old)
		err = ngc_before_alloc(v);
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

/* ------------------------------------------------------------- namespace (Tier 2) */

/*
 * A full volume makes the core's index code fail to move an attribute in or out of its MFT
 * record with -EPERM ("cannot be resident/non-resident"); for a name operation that means no
 * space, not access denied.
 */
static int ngc_index_errno(int err)
{
	return err == -EPERM ? -ENOSPC : err;
}

int ngc_create(ngc_vol *v, ngc_node *dirn, const unsigned short *name, unsigned int len, int is_dir, ngc_node **out)
{
	struct inode *dir = (struct inode *)dirn;
	struct dentry *d;
	int err;
	*out = NULL;
	if (sb_rdonly(v->sb))
		return -EROFS;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	err = ngc_mark_dirty(v);
	if (err)
		return err;
	d = ngc_mkdentry(v, name, len, NULL);
	if (IS_ERR(d))
		return PTR_ERR(d);
	if (is_dir) {
		struct dentry *r = dir->i_op->mkdir(&nop_mnt_idmap, dir, d, S_IFDIR | 0755);
		err = IS_ERR(r) ? PTR_ERR(r) : 0;
	} else {
		err = dir->i_op->create(&nop_mnt_idmap, dir, d, S_IFREG | 0644);
	}
	if (!err && !d->d_inode)
		err = -EIO;
	if (!err)
		*out = (ngc_node *)d->d_inode;	/* the new inode's reference from new_inode() */
	err = ngc_index_errno(err);
	ngc_freedentry(d);
	return err;
}

int ngc_unlink(ngc_vol *v, ngc_node *dirn, const unsigned short *name, unsigned int len, ngc_node *n)
{
	struct inode *dir = (struct inode *)dirn, *vi = (struct inode *)n;
	struct dentry *d;
	int err;
	if (sb_rdonly(v->sb))
		return -EROFS;
	err = ngc_mark_dirty(v);
	if (err)
		return err;
	d = ngc_mkdentry(v, name, len, vi);
	if (IS_ERR(d))
		return PTR_ERR(d);
	err = S_ISDIR(vi->i_mode) ? dir->i_op->rmdir(dir, d) : dir->i_op->unlink(dir, d);
	ngc_freedentry(d);
	return err;
}

/* Renames (odir, oname) of @n to (ndir, nname); @target is the inode the new name replaces, or NULL. */
int ngc_rename(ngc_vol *v, ngc_node *odirn, const unsigned short *oname, unsigned int olen, ngc_node *n,
		ngc_node *ndirn, const unsigned short *nname, unsigned int nlen, ngc_node *target)
{
	struct inode *odir = (struct inode *)odirn, *ndir = (struct inode *)ndirn;
	struct dentry *od, *nd;
	int err;
	if (sb_rdonly(v->sb))
		return -EROFS;
	err = ngc_mark_dirty(v);
	if (err)
		return err;
	od = ngc_mkdentry(v, oname, olen, (struct inode *)n);
	nd = ngc_mkdentry(v, nname, nlen, (struct inode *)target);
	if (IS_ERR(od) || IS_ERR(nd)) {
		err = IS_ERR(od) ? PTR_ERR(od) : PTR_ERR(nd);
		ngc_freedentry(IS_ERR(od) ? NULL : od);
		ngc_freedentry(IS_ERR(nd) ? NULL : nd);
		return err;
	}
	err = odir->i_op->rename(&nop_mnt_idmap, odir, od, ndir, nd, 0);
	ngc_freedentry(od);
	ngc_freedentry(nd);
	return ngc_index_errno(err);
}

int ngc_link(ngc_vol *v, ngc_node *n, ngc_node *ndirn, const unsigned short *nname, unsigned int nlen)
{
	struct inode *ndir = (struct inode *)ndirn, *vi = (struct inode *)n;
	struct dentry *od, *nd;
	int err;
	if (sb_rdonly(v->sb))
		return -EROFS;
	if (S_ISDIR(vi->i_mode))
		return -EPERM;
	err = ngc_mark_dirty(v);
	if (err)
		return err;
	od = ngc_mkdentry(v, nname, nlen, vi);
	nd = ngc_mkdentry(v, nname, nlen, NULL);
	if (IS_ERR(od) || IS_ERR(nd)) {
		err = IS_ERR(od) ? PTR_ERR(od) : PTR_ERR(nd);
		ngc_freedentry(IS_ERR(od) ? NULL : od);
		ngc_freedentry(IS_ERR(nd) ? NULL : nd);
		return err;
	}
	err = ngc_index_errno(ndir->i_op->link(od, ndir, nd));
	/* The core took a reference for the new dentry (ihold); our dentries hold none. */
	if (!err)
		iput(vi);
	ngc_freedentry(od);
	ngc_freedentry(nd);
	return err;
}

static int ngc_any_entry(void *ctx, const unsigned short *name, unsigned int len, unsigned long long ino, unsigned int t)
{
	(void)ino; (void)t;
	if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
		return 0;
	*(int *)ctx = 1;
	return 1;
}

/* 1 if the directory has no entries besides . and .., 0 if it has, <0 on error. */
int ngc_dir_empty(ngc_vol *v, ngc_node *dirn)
{
	int any = 0, err = ngc_readdir(v, dirn, ngc_any_entry, &any);
	return err < 0 ? err : !any;
}
