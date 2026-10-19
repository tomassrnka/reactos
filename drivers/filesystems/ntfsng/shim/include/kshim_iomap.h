/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * kshim_iomap.h - the iomap interface as the vendored fs/ntfs core uses it.
 *
 * Only the names the core refers to are declared: the extent description it fills in
 * its mapping callbacks, the callback tables it hands to the shim, and the entry points
 * it calls.  Numeric values and the iterator's private members are this shim's own; the
 * implementation is shim/kshim_iomap.c.
 */
#ifndef KSHIM_IOMAP_H
#define KSHIM_IOMAP_H

/* What an extent of a file is backed by (struct iomap.type). */
#define IOMAP_HOLE 0		/* nothing: reads as zeros */
#define IOMAP_MAPPED 1		/* device bytes at iomap.addr */
#define IOMAP_UNWRITTEN 2	/* allocated but not yet initialised: reads as zeros */
#define IOMAP_DELALLOC 3	/* reserved, no device location yet: reads as zeros */
#define IOMAP_INLINE 4		/* bytes in memory at iomap.inline_data (resident value) */

/* Extent modifiers (struct iomap.flags). */
#define IOMAP_F_NEW 0x01	/* freshly allocated: contents undefined, read as zeros */
#define IOMAP_F_MERGED 0x02	/* informational only */
#define IOMAP_F_STALE 0x04	/* mapping went stale: map again even without progress */

/* What the caller is doing (iomap_iter.flags, passed to the mapping callback). */
#define IOMAP_WRITE 0x01
#define IOMAP_ZERO 0x02
#define IOMAP_REPORT 0x04
#define IOMAP_DIRECT 0x08

#define IOMAP_NULL_ADDR (~0ULL)

/* One extent: file bytes [offset, offset + length) and where they live. */
struct iomap {
	u64 addr;			/* device byte address of @offset (MAPPED, UNWRITTEN) */
	loff_t offset;
	u64 length;
	u16 type;
	u16 flags;
	struct block_device *bdev;
	void *inline_data;		/* address of the byte at @offset (INLINE) */
	void *private;			/* owned by the mapping callbacks */
};

/*
 * A walk over the extents of the byte range [pos, pos + len) of @inode.  The user loops
 * on iomap_iter() while it returns > 0, consumes bytes of the current extent with
 * iomap_iter_advance() and stores its own result (0 or -errno) in kshim_work.
 */
struct iomap_iter {
	struct inode *inode;
	loff_t pos;
	u64 len;
	unsigned flags;
	struct iomap iomap;
	struct iomap srcmap;		/* not used by this core; kept zeroed */
	void *private;
	loff_t kshim_ext_start;		/* pos when the current extent was handed out */
	int kshim_work;
};

typedef int (*kshim_map_fn)(struct inode *inode, loff_t pos, loff_t length, unsigned flags,
		struct iomap *iomap, struct iomap *srcmap);
typedef int (*kshim_unmap_fn)(struct inode *inode, loff_t pos, loff_t length, ssize_t_ done,
		unsigned flags, struct iomap *iomap);

struct iomap_ops {
	int (*iomap_next)(const struct iomap_iter *iter, struct iomap *iomap, struct iomap *srcmap);
};

int iomap_iter(struct iomap_iter *iter, const struct iomap_ops *ops);
int iomap_iter_advance(struct iomap_iter *iter, u64 count);
int iomap_iter_next(const struct iomap_iter *iter, struct iomap *iomap, struct iomap *srcmap,
		kshim_map_fn map, kshim_unmap_fn unmap);

/* The core builds its iomap_next callbacks from a map and an optional unmap function. */
#define DEFINE_IOMAP_ITER_NEXT_END(fn, map_fn, unmap_fn) \
	int fn(const struct iomap_iter *it, struct iomap *e, struct iomap *s) \
	{ return iomap_iter_next(it, e, s, map_fn, unmap_fn); }
#define DEFINE_IOMAP_ITER_NEXT(fn, map_fn) DEFINE_IOMAP_ITER_NEXT_END(fn, map_fn, NULL)

/* Page-cache reads. */
struct iomap_read_folio_ctx;
struct iomap_read_ops {
	int (*read_folio_range)(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx, size_t len);
	void (*submit_read)(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx);
};
struct iomap_read_folio_ctx {
	const struct iomap_read_ops *ops;
	struct folio *cur_folio;
	struct readahead_control *rac;
	struct bio *kshim_bio;		/* device read being assembled, not yet submitted */
};
void iomap_read_folio(const struct iomap_ops *ops, struct iomap_read_folio_ctx *ctx, void *private);
void iomap_readahead(const struct iomap_ops *ops, struct iomap_read_folio_ctx *ctx, void *private);
void iomap_finish_folio_read(struct folio *folio, size_t off, size_t len, int error);
int iomap_bio_read_folio_range(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx, size_t len);
void iomap_bio_submit_read_endio(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx, bio_end_io_t end_io);

/* Buffered and direct writes, zeroing, seeking: referenced by the core, not used by the NT port. */
struct iomap_write_ops {
	void (*put_folio)(struct inode *inode, loff_t pos, unsigned copied, struct folio *folio);
	bool (*iomap_valid)(struct inode *inode, const struct iomap *iomap);
};
struct iomap_dio_ops {
	int (*end_io)(struct kiocb *iocb, ssize_t_ size, int error, unsigned flags);
};
bool iomap_is_partially_uptodate(struct folio *folio, size_t from, size_t count);
bool iomap_release_folio(struct folio *folio, gfp_t gfp_flags);
void iomap_invalidate_folio(struct folio *folio, size_t offset, size_t len);
bool iomap_dirty_folio(struct address_space *mapping, struct folio *folio);
ssize_t_ iomap_file_buffered_write(struct kiocb *iocb, struct iov_iter *from, const struct iomap_ops *ops,
		const struct iomap_write_ops *write_ops, void *private);
int iomap_zero_range(struct inode *inode, loff_t pos, loff_t len, bool *did_zero, const struct iomap_ops *ops,
		const struct iomap_write_ops *write_ops, void *private);
vm_fault_t iomap_page_mkwrite(struct vm_fault *vmf, const struct iomap_ops *ops, void *private);
int iomap_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo, u64 start, u64 len,
		const struct iomap_ops *ops);
loff_t iomap_seek_hole(struct inode *inode, loff_t offset, const struct iomap_ops *ops);
loff_t iomap_seek_data(struct inode *inode, loff_t offset, const struct iomap_ops *ops);
ssize_t_ iomap_dio_rw(struct kiocb *iocb, struct iov_iter *iter, const struct iomap_ops *ops,
		const struct iomap_dio_ops *dops, unsigned int dio_flags, void *private, size_t done_before);
int iomap_swapfile_activate(struct swap_info_struct *sis, struct file *swap_file, sector_t *pagespan,
		const struct iomap_ops *ops);

/* Writeback of dirty page-cache folios. */
struct iomap_writepage_ctx;
struct iomap_writeback_ops {
	ssize_t_ (*writeback_range)(struct iomap_writepage_ctx *wpc, struct folio *folio, u64 pos,
			unsigned int len, u64 end_pos);
	int (*writeback_submit)(struct iomap_writepage_ctx *wpc, int error);
};
struct iomap_writepage_ctx {
	struct iomap iomap;		/* the extent the core mapped last */
	struct inode *inode;
	struct writeback_control *wbc;
	const struct iomap_writeback_ops *ops;
};
int iomap_writepages(struct iomap_writepage_ctx *wpc);
ssize_t_ iomap_add_to_ioend(struct iomap_writepage_ctx *wpc, struct folio *folio, loff_t pos, loff_t end_pos,
		unsigned int dirty_len);
int iomap_ioend_writeback_submit(struct iomap_writepage_ctx *wpc, int error);

#endif
