/* SPDX-License-Identifier: GPL-2.0-or-later */
/* kshim_iomap.h - iomap types; definitions follow include/linux/iomap.h (Linux v7.3-rc6). */
#ifndef KSHIM_IOMAP_H
#define KSHIM_IOMAP_H
#define IOMAP_HOLE 0
#define IOMAP_DELALLOC 1
#define IOMAP_MAPPED 2
#define IOMAP_UNWRITTEN 3
#define IOMAP_INLINE 4
#define IOMAP_F_NEW (1U << 0)
#define IOMAP_F_DIRTY (1U << 1)
#define IOMAP_F_SHARED (1U << 2)
#define IOMAP_F_MERGED (1U << 3)
#define IOMAP_F_BUFFER_HEAD 0
#define IOMAP_F_XATTR (1U << 5)
#define IOMAP_F_BOUNDARY (1U << 6)
#define IOMAP_F_ANON_WRITE (1U << 7)
#define IOMAP_F_ZERO_TAIL (1U << 10)
#define IOMAP_F_PRIVATE (1U << 12)
#define IOMAP_F_SIZE_CHANGED (1U << 14)
#define IOMAP_F_STALE (1U << 15)
#define IOMAP_NULL_ADDR -1ULL
struct iomap {
	u64 addr; loff_t offset; u64 length; u16 type; u16 flags;
	struct block_device *bdev; struct dax_device *dax_dev;
	void *inline_data; void *private; u64 validity_cookie;
};
static inline void *iomap_inline_data(const struct iomap *iomap, loff_t pos)
{ return (char *)iomap->inline_data + pos - iomap->offset; }
struct iomap_iter;
struct iomap_write_ops {
	struct folio *(*get_folio)(struct iomap_iter *iter, loff_t pos, unsigned len);
	void (*put_folio)(struct inode *inode, loff_t pos, unsigned copied, struct folio *folio);
	bool (*iomap_valid)(struct inode *inode, const struct iomap *iomap);
	int (*read_folio_range)(const struct iomap_iter *iter, struct folio *folio, loff_t pos, size_t len);
};
#define IOMAP_WRITE (1 << 0)
#define IOMAP_ZERO (1 << 1)
#define IOMAP_REPORT (1 << 2)
#define IOMAP_FAULT (1 << 3)
#define IOMAP_DIRECT (1 << 4)
#define IOMAP_NOWAIT (1 << 5)
#define IOMAP_OVERWRITE_ONLY (1 << 6)
#define IOMAP_UNSHARE (1 << 7)
#define IOMAP_DAX 0
#define IOMAP_ATOMIC (1 << 9)
#define IOMAP_DONTCACHE (1 << 10)
typedef int (*iomap_iter_begin_fn)(struct inode *inode, loff_t pos, loff_t length, unsigned flags, struct iomap *iomap, struct iomap *srcmap);
typedef int (*iomap_iter_end_fn)(struct inode *inode, loff_t pos, loff_t length, ssize_t_ written, unsigned flags, struct iomap *iomap);
typedef int (*iomap_iter_next_fn)(const struct iomap_iter *iter, struct iomap *iomap, struct iomap *srcmap);
struct iomap_ops { iomap_iter_begin_fn iomap_begin; iomap_iter_end_fn iomap_end; iomap_iter_next_fn iomap_next; };
struct iomap_iter {
	struct inode *inode; loff_t pos; u64 len; loff_t iter_start_pos; int status;
	unsigned flags; struct iomap iomap; struct iomap srcmap; struct folio_batch *fbatch; void *private;
};
int iomap_iter(struct iomap_iter *iter, const struct iomap_ops *ops);
int iomap_iter_advance(struct iomap_iter *iter, u64 count);
static inline u64 iomap_length_trim(const struct iomap_iter *iter, loff_t pos, u64 len)
{
	u64 end = iter->iomap.offset + iter->iomap.length;
	if (iter->srcmap.type != IOMAP_HOLE)
		end = min(end, iter->srcmap.offset + iter->srcmap.length);
	return min(len, end - pos);
}
static inline u64 iomap_length(const struct iomap_iter *iter) { return iomap_length_trim(iter, iter->pos, iter->len); }
int iomap_iter_continue(const struct iomap_iter *iter, struct iomap *iomap, struct iomap *srcmap, int ret);
static __always_inline int iomap_iter_next(const struct iomap_iter *iter,
		struct iomap *iomap, struct iomap *srcmap,
		iomap_iter_begin_fn begin, iomap_iter_end_fn end)
{
	int ret = 0;
	if (iomap->length) {
		if (end) {
			ssize_t_ advanced = iter->pos - iter->iter_start_pos;
			loff_t len = iomap_length_trim(iter, iter->iter_start_pos, iter->len + advanced);
			ret = end(iter->inode, iter->iter_start_pos, len, advanced, iter->flags, iomap);
		}
		ret = iomap_iter_continue(iter, iomap, srcmap, ret);
		if (ret <= 0)
			return ret;
	}
	ret = begin(iter->inode, iter->pos, iter->len, iter->flags, iomap, srcmap);
	return ret < 0 ? ret : 1;
}
#define DEFINE_IOMAP_ITER_NEXT_END(name, begin_fn, end_fn) \
int name(const struct iomap_iter *iter, struct iomap *iomap, struct iomap *srcmap) \
{ return iomap_iter_next(iter, iomap, srcmap, begin_fn, end_fn); }
#define DEFINE_IOMAP_ITER_NEXT(name, begin_fn) DEFINE_IOMAP_ITER_NEXT_END(name, begin_fn, NULL)

struct iomap_read_folio_ctx;
struct iomap_read_ops {
	int (*read_folio_range)(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx, size_t len);
	void (*submit_read)(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx);
};
struct iomap_read_folio_ctx {
	const struct iomap_read_ops *ops; struct folio *cur_folio; struct readahead_control *rac;
	void *read_ctx; loff_t read_ctx_file_offset; struct fsverity_info *vi;
};
void iomap_read_folio(const struct iomap_ops *ops, struct iomap_read_folio_ctx *ctx, void *private);
void iomap_readahead(const struct iomap_ops *ops, struct iomap_read_folio_ctx *ctx, void *private);
void iomap_finish_folio_read(struct folio *folio, size_t off, size_t len, int error);
int iomap_bio_read_folio_range(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx, size_t plen);
void iomap_bio_submit_read_endio(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx, bio_end_io_t end_io);
bool iomap_is_partially_uptodate(struct folio *, size_t from, size_t count);
bool iomap_release_folio(struct folio *folio, gfp_t gfp_flags);
void iomap_invalidate_folio(struct folio *folio, size_t offset, size_t len);
bool iomap_dirty_folio(struct address_space *mapping, struct folio *folio);
ssize_t_ iomap_file_buffered_write(struct kiocb *iocb, struct iov_iter *from, const struct iomap_ops *ops, const struct iomap_write_ops *write_ops, void *private);
int iomap_zero_range(struct inode *inode, loff_t pos, loff_t len, bool *did_zero, const struct iomap_ops *ops, const struct iomap_write_ops *write_ops, void *private);
int iomap_truncate_page(struct inode *inode, loff_t pos, bool *did_zero, const struct iomap_ops *ops, const struct iomap_write_ops *write_ops, void *private);
vm_fault_t iomap_page_mkwrite(struct vm_fault *vmf, const struct iomap_ops *ops, void *private);
int iomap_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo, u64 start, u64 len, const struct iomap_ops *ops);
loff_t iomap_seek_hole(struct inode *inode, loff_t offset, const struct iomap_ops *ops);
loff_t iomap_seek_data(struct inode *inode, loff_t offset, const struct iomap_ops *ops);
struct iomap_writepage_ctx;
struct iomap_writeback_ops {
	ssize_t_ (*writeback_range)(struct iomap_writepage_ctx *wpc, struct folio *folio, u64 pos, unsigned int len, u64 end_pos);
	int (*writeback_submit)(struct iomap_writepage_ctx *wpc, int error);
};
struct iomap_writepage_ctx {
	struct iomap iomap; struct inode *inode; struct writeback_control *wbc;
	const struct iomap_writeback_ops *ops; u32 nr_folios; void *wb_ctx;
};
ssize_t_ iomap_add_to_ioend(struct iomap_writepage_ctx *wpc, struct folio *folio, loff_t pos, loff_t end_pos, unsigned int dirty_len);
int iomap_ioend_writeback_submit(struct iomap_writepage_ctx *wpc, int error);
int iomap_writepages(struct iomap_writepage_ctx *wpc);
struct iomap_dio_ops {
	int (*end_io)(struct kiocb *iocb, ssize_t_ size, int error, unsigned flags);
	void (*submit_io)(const struct iomap_iter *iter, struct bio *bio, loff_t file_offset);
	struct bio_set *bio_set;
};
#define IOMAP_DIO_UNWRITTEN (1 << 0)
#define IOMAP_DIO_COW (1 << 1)
#define IOMAP_DIO_FORCE_WAIT (1 << 0)
#define IOMAP_DIO_OVERWRITE_ONLY (1 << 1)
#define IOMAP_DIO_PARTIAL (1 << 2)
ssize_t_ iomap_dio_rw(struct kiocb *iocb, struct iov_iter *iter, const struct iomap_ops *ops, const struct iomap_dio_ops *dops, unsigned int dio_flags, void *private, size_t done_before);
int iomap_swapfile_activate(struct swap_info_struct *sis, struct file *swap_file, sector_t *pagespan, const struct iomap_ops *ops);
#endif
