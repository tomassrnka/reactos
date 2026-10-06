// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ngcore.c - the fs/ntfs side of ngapi.h.  Drives the unmodified core the
 * way Linux's VFS does (fs_context mount, inode_operations->lookup,
 * file_operations->iterate_shared, address_space->read_folio through the
 * shim page cache) and hands the NT glue plain C values.  No NTFS format
 * logic lives here.
 */
#include <kshim.h>
#include "ntfs/ntfs.h"
#include "ntfs/attrib.h"
#include "ntfs/dir.h"
#include "ntfs/inode.h"
#include "ntfs/mft.h"
#include "ngapi.h"

extern initcall_t kshim_module_init;
extern struct file_system_type *kshim_fs_type;
extern struct block_device *kshim_mount_bdev;
struct block_device *kshim_bdev_open(void *osdev, u64 size, unsigned int sector_size);
void kshim_bdev_close(struct block_device *b);
void kshim_mapping_shrink(struct address_space *m);
extern unsigned long kshim_pc_pages, kshim_inodes_live, kshim_counter_reads;
void kshim_dump_allocs(void);

struct ngc_vol {
	struct super_block *sb;
	struct block_device *bdev;
};

static DEFINE_MUTEX(ngc_mount_lock);
static int ngc_inited;

#define NT_EPOCH_DELTA 116444736000000000LL
static long long ts_to_nt(struct timespec64 t)
{
	return t.tv_sec * 10000000LL + t.tv_nsec / 100 + NT_EPOCH_DELTA;
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

int ngc_mount(void *osdev, unsigned long long size, unsigned int sector_size, ngc_vol **out)
{
	struct fs_context *fc;
	struct block_device *b;
	struct ngc_vol *v;
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
	/* NT name lookups are case-insensitive; the core defaults to case-sensitive. */
	NVolClearCaseSensitive((struct ntfs_volume *)fc->s_fs_info);
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
