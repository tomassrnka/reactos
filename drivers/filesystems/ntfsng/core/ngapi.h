/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ngapi.h - the interface between the NT glue (nt/, compiled with NT headers)
 * and the fs/ntfs core plus shim (compiled with Linux-API headers only).
 * Only basic C types cross it.  Errors are negative Linux errno values.
 */
#ifndef NGAPI_H
#define NGAPI_H

typedef struct ngc_vol ngc_vol;      /* a mounted fs/ntfs super_block */
typedef struct ngc_node ngc_node;    /* a referenced fs/ntfs VFS inode */

#define NGC_ENOENT 2
#define NGC_EIO 5
#define NGC_EAGAIN 11
#define NGC_ENOMEM 12
#define NGC_ENOTDIR 20
#define NGC_EISDIR 21
#define NGC_EINVAL 22
#define NGC_EROFS 30
#define NGC_ENAMETOOLONG 36
#define NGC_EOPNOTSUPP 95
#define NGC_EUCLEAN 117
#define NGC_ENOSPC 28
#define NGC_EEXIST 17
#define NGC_ENOTEMPTY 39
#define NGC_EACCES 13
#define NGC_EPERM 1
#define NGC_EFBIG 27
#define NGC_EXDEV 18
#define NGC_ENODATA 61

#define NGC_ATTR_COMPRESSED 1
#define NGC_ATTR_SPARSE 2
#define NGC_ATTR_ENCRYPTED 4
#define NGC_ATTR_NOWRITE 8      /* compressed, encrypted, WOF or sparse: the driver does not write it */

struct ngc_stat {
	unsigned long long mft_ref;      /* MFT number | sequence << 48 */
	unsigned long long size;
	unsigned long long alloc;        /* on-disk allocation (compressed size if compressed) */
	long long crtime, atime, mtime, ctime;  /* NT time, 100 ns since 1601 */
	unsigned int file_attributes;    /* $STANDARD_INFORMATION flags (FILE_ATTRIBUTE_* values) */
	unsigned int nlink;
	unsigned int flags;              /* NGC_ATTR_* of this stream */
	int is_dir;
	int is_link;
};

struct ngc_volinfo {
	unsigned long long total_clusters;
	unsigned long long free_clusters;
	unsigned long long serial;
	unsigned int cluster_size;
	unsigned int sector_size;
	unsigned int label_len;          /* in UTF-16 units */
	unsigned short label[64];
	unsigned char major, minor;
	unsigned char read_only;         /* mounted read-only (policy or request) */
	unsigned char dirty;             /* VOLUME_IS_DIRTY currently set */
	unsigned char damaged;           /* the mount-time consistency check found damage (read-only) */
};

/* Mount-time consistency check: 0 never, 1 after an unclean shutdown or for small MFTs, 2 always. */
extern int ngc_check_policy;

typedef int (*ngc_filldir_t)(void *ctx, const unsigned short *name, unsigned int len,
		unsigned long long mft_no, unsigned int dtype);
typedef int (*ngc_stream_t)(void *ctx, const unsigned short *name, unsigned int len,
		unsigned long long size, unsigned long long alloc);

int ngc_init(void);
/* Mounts; with want_rw the volume goes read-write unless policy refuses (*why_ro says why). */
int ngc_mount(void *osdev, unsigned long long size, unsigned int sector_size, int want_rw,
		ngc_vol **out, const char **why_ro);
void ngc_umount(ngc_vol *v);
void ngc_volinfo(ngc_vol *v, struct ngc_volinfo *vi);
ngc_node *ngc_root(ngc_vol *v);
/* Looks @name up in @dir; with @real (NTFS_MAX_NAME_LEN units) also returns the name as stored on disk. */
int ngc_lookup(ngc_vol *v, ngc_node *dir, const unsigned short *name, unsigned int len, ngc_node **out,
		unsigned short *real, unsigned int *real_len);
int ngc_open_stream(ngc_vol *v, ngc_node *base, const unsigned short *sname, unsigned int len, ngc_node **out);
int ngc_iget(ngc_vol *v, unsigned long long mft_no, ngc_node **out);
void ngc_put(ngc_node *n);
void ngc_stat(ngc_node *n, struct ngc_stat *st);
int ngc_readdir(ngc_vol *v, ngc_node *dir, ngc_filldir_t fn, void *ctx);

/* One directory entry with what a directory listing shows (ngc_readdir_full). */
struct ngc_dirent {
	const unsigned short *name;
	unsigned int len;                /* in UTF-16 units */
	int is_dot;                      /* "." or ".." (st not filled) */
	struct ngc_stat st;
	unsigned short short_name[12];   /* the 8.3 name of this link in this directory, if any */
	unsigned int short_len;
	unsigned int reparse_tag;
};
typedef int (*ngc_dirent_t)(void *ctx, const struct ngc_dirent *e);
int ngc_readdir_full(ngc_vol *v, ngc_node *dir, ngc_dirent_t fn, void *ctx);
int ngc_streams(ngc_node *n, ngc_stream_t fn, void *ctx);
/* Reads [off, off+len) through the private page cache; bytes past EOF are zero.  Returns bytes copied or <0. */
long ngc_read(ngc_node *n, unsigned long long off, unsigned int len, void *buf, int drop_cache);
long ngc_read_direct(ngc_vol *v, ngc_node *n, unsigned long long off, unsigned int len, void *buf);
void ngc_stats(unsigned long *pages, unsigned long *inodes, unsigned long *reads);
/* Drops the clean, unreferenced shim page-cache pages of a node. */
void ngc_trim(ngc_node *n);
/* Write side (all under the caller's volume lock). */
int ngc_is_rw(ngc_vol *v);
int ngc_mark_dirty(ngc_vol *v);
int ngc_sync(ngc_vol *v);
int ngc_commit_now(ngc_vol *v);
int ngc_changed(ngc_vol *v);
void ngc_icache_trim(ngc_vol *v);
int ngc_dirty(ngc_vol *v);
void ngc_jnl_report(ngc_vol *v);
void ngc_set_journal_fault(unsigned long commit_no);
typedef int (*ngc_run_t)(void *ctx, unsigned long long vcn, long long lcn, unsigned long long len);
int ngc_runs(ngc_vol *v, ngc_node *n, ngc_run_t fn, void *ctx);
int ngc_raw_write(ngc_vol *v, unsigned long long off, const void *buf, unsigned int len);
int ngc_links(ngc_node *n);
int ngc_parent_name(ngc_node *n, unsigned long long *parent, unsigned short *out, unsigned int *len);
int ngc_get_security(ngc_vol *v, ngc_node *n, void **out, unsigned int *len);
int ngc_set_security(ngc_vol *v, ngc_node *n, const void *sd, unsigned int len);
void ngc_free(void *p);
int ngc_create_stream(ngc_vol *v, ngc_node *base, const unsigned short *sname, unsigned int len, ngc_node **out);
int ngc_delete_stream(ngc_vol *v, ngc_node *n);
int ngc_is_reparse(ngc_node *n);
int ngc_get_reparse(ngc_node *n, void **out, unsigned int *len);
int ngc_set_reparse(ngc_vol *v, ngc_node *n, const void *data, unsigned int len);
int ngc_delete_reparse(ngc_vol *v, ngc_node *n, unsigned int tag);
int ngc_short_name(ngc_node *n, unsigned long long parent_mref, unsigned short *out, unsigned int *len);
int ngc_add_short_name(ngc_vol *v, ngc_node *dir, ngc_node *n, const unsigned short *lname, unsigned int llen,
		const unsigned short *sname, unsigned int slen);
long ngc_write(ngc_vol *v, ngc_node *n, unsigned long long off, unsigned int len, const void *buf);
int ngc_set_size(ngc_vol *v, ngc_node *n, unsigned long long newsize);
int ngc_set_info(ngc_vol *v, ngc_node *n, const long long times[4], unsigned int attrs, unsigned int attrs_mask);
int ngc_create(ngc_vol *v, ngc_node *dir, const unsigned short *name, unsigned int len, int is_dir, ngc_node **out);
int ngc_unlink(ngc_vol *v, ngc_node *dir, const unsigned short *name, unsigned int len, ngc_node *n);
int ngc_rename(ngc_vol *v, ngc_node *odir, const unsigned short *oname, unsigned int olen, ngc_node *n,
		ngc_node *ndir, const unsigned short *nname, unsigned int nlen, ngc_node *target);
int ngc_link(ngc_vol *v, ngc_node *n, ngc_node *ndir, const unsigned short *nname, unsigned int nlen);
int ngc_dir_empty(ngc_vol *v, ngc_node *dir);
void ngc_write_stats(unsigned long *writes, unsigned long long *bytes, unsigned long *syncs, unsigned long *dirties);
/* Prints shim memory accounting (live allocations by call site, cache pages, inodes). */
void ngc_debug_dump(void);

#endif
