// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * kshim_iomap.c - the iomap services the vendored core calls: walking the extents its
 * mapping callbacks report, filling one page-cache folio from them, and writing dirty
 * folios back through them.
 *
 * Written for this shim from the interface contract: the declarations in kshim_iomap.h
 * and what the core's own callbacks expect (fs/ntfs/iomap.c: a map callback may return
 * holding a lock that its unmap callback releases; fs/ntfs/aops.c: the read completion
 * handler zeroes past initialized_size and reports each piece through
 * iomap_finish_folio_read).  Two shim facts simplify it: folios are one page, and
 * submit_bio() finishes the I/O and runs the completion handler before it returns.
 */
#include <kshim.h>

int kshim_dev_rw(struct block_device *b, int write, u64 off, void *buf, size_t len);
/* Set by the adapter: true for inodes whose pages are file data (written in place, not journaled). */
bool (*kshim_is_data_inode)(struct inode *i);

/* ------------------------------------------------------------ extent walk */

/* First byte past the current extent, or past the request if that ends first. */
static loff_t ext_stop(const struct iomap_iter *it)
{
	loff_t ext_end = it->iomap.offset + (loff_t)it->iomap.length;
	loff_t req_end = it->pos + (loff_t)it->len;
	return ext_end < req_end ? ext_end : req_end;
}

/*
 * One step of the walk.  If an extent is outstanding it is closed first: the unmap
 * callback learns how many bytes of it were consumed (and drops whatever the map
 * callback kept, such as a record lock).  The walk ends on any error, when the request
 * is used up, or when the last extent was not consumed at all (unless the callbacks
 * marked it stale), so a user that cannot progress cannot loop forever.
 */
int iomap_iter_next(const struct iomap_iter *it, struct iomap *e, struct iomap *s,
		kshim_map_fn map, kshim_unmap_fn unmap)
{
	int err;

	if (e->length) {
		loff_t used = it->pos - it->kshim_ext_start;
		int stale = (e->flags & IOMAP_F_STALE) != 0;

		err = 0;
		if (unmap) {
			loff_t span = e->offset + (loff_t)e->length - it->kshim_ext_start;
			if (span > used + (loff_t)it->len)
				span = used + (loff_t)it->len;
			ssize_t_ r = unmap(it->inode, it->kshim_ext_start, span, used, it->flags, e);
			if (r < 0)
				err = (int)r;
		}
		if (it->kshim_work < 0)
			return it->kshim_work;
		if (err)
			return err;
		if (it->len == 0 || (used == 0 && !stale))
			return 0;
		memset(e, 0, sizeof(*e));
		memset(s, 0, sizeof(*s));
	}
	err = map(it->inode, it->pos, (loff_t)it->len, it->flags, e, s);
	if (err)
		return err;
	if (e->length == 0 || ext_stop(it) <= it->pos) {
		/* A map callback must cover pos; closing the empty extent keeps its locks balanced. */
		if (unmap)
			unmap(it->inode, it->pos, 0, 0, it->flags, e);
		return -EIO;
	}
	return 1;
}

int iomap_iter(struct iomap_iter *it, const struct iomap_ops *ops)
{
	int r = ops->iomap_next(it, &it->iomap, &it->srcmap);
	it->kshim_work = 0;
	it->kshim_ext_start = it->pos;
	return r;
}

int iomap_iter_advance(struct iomap_iter *it, u64 count)
{
	if (count > it->len)
		return -EIO;
	it->pos += count;
	it->len -= count;
	return 0;
}

/* ------------------------------------------------------------ folio read */

/* Accounting of one folio while iomap_read_folio() fills it (the folio stays locked). */
struct kshim_fill {
	size_t queued;		/* bytes handed to device reads */
	size_t completed;	/* bytes the completion handler reported back */
	int error;
};

void iomap_finish_folio_read(struct folio *f, size_t off, size_t len, int error)
{
	struct kshim_fill *fill = f->kshim_fill;

	(void)off;
	if (!fill) {
		/* A read nobody is accounting for: it covered the whole folio, so finish it here. */
		if (!error)
			folio_mark_uptodate(f);
		folio_unlock(f);
		return;
	}
	fill->completed += len;
	if (error && !fill->error)
		fill->error = error;
}

/* Hands the device read being assembled to the core's submit hook (which picks the completion). */
static void read_flush(const struct iomap_iter *it, struct iomap_read_folio_ctx *ctx)
{
	if (ctx->kshim_bio)
		ctx->ops->submit_read(it, ctx);
}

/* Queues the device bytes behind [pos, pos + len) of the current folio. */
int iomap_bio_read_folio_range(const struct iomap_iter *it, struct iomap_read_folio_ctx *ctx, size_t len)
{
	struct folio *f = ctx->cur_folio;
	size_t at = offset_in_folio(f, it->pos);
	sector_t sector = (sector_t)((it->iomap.addr + (u64)(it->pos - it->iomap.offset)) >> SECTOR_SHIFT);
	struct bio *bio = ctx->kshim_bio;

	if (bio && bio_end_sector(bio) == sector && bio_add_folio(bio, f, len, at))
		return 0;
	read_flush(it, ctx);
	bio = bio_alloc(it->iomap.bdev, 4, REQ_OP_READ, GFP_NOFS);
	if (!bio)
		return -ENOMEM;
	bio->bi_iter.bi_sector = sector;
	if (!bio_add_folio(bio, f, len, at)) {
		bio_put(bio);
		return -EIO;
	}
	ctx->kshim_bio = bio;
	return 0;
}

void iomap_bio_submit_read_endio(const struct iomap_iter *it, struct iomap_read_folio_ctx *ctx, bio_end_io_t end_io)
{
	struct bio *bio = ctx->kshim_bio;

	(void)it;
	if (!bio)
		return;
	ctx->kshim_bio = NULL;
	bio->bi_end_io = end_io;
	submit_bio(bio);	/* synchronous here; end_io releases the bio */
}

/*
 * Fills the part of the folio that the current extent covers.  In-memory values are
 * copied, device extents are read in whole blocks up to the block that holds EOF, and
 * everything else in the span (holes, unwritten or new extents, bytes past EOF) is zeroed.
 */
static int fill_extent(struct iomap_iter *it, struct iomap_read_folio_ctx *ctx, struct kshim_fill *fill)
{
	struct folio *f = ctx->cur_folio;
	const struct iomap *e = &it->iomap;
	loff_t isize = i_size_read(it->inode);
	loff_t stop = ext_stop(it);
	u8 *page = folio_address(f);
	size_t at = offset_in_folio(f, it->pos);
	size_t span, data = 0;
	int err;

	if (stop > folio_next_pos(f))
		stop = folio_next_pos(f);
	span = (size_t)(stop - it->pos);

	if (e->type == IOMAP_INLINE) {
		loff_t vend = e->offset + (loff_t)e->length;
		if (!e->inline_data || it->pos < e->offset)
			return -EIO;
		if (vend > isize)
			vend = isize;
		if (vend > it->pos)
			data = min_t(size_t, span, (size_t)(vend - it->pos));
		memcpy(page + at, (u8 *)e->inline_data + (it->pos - e->offset), data);
	} else if (e->type == IOMAP_MAPPED && !(e->flags & IOMAP_F_NEW) && it->pos < isize) {
		loff_t blk_end = round_up(isize, (loff_t)1 << it->inode->i_blkbits);
		data = min_t(size_t, span, (size_t)(blk_end - it->pos));
		fill->queued += data;
		err = ctx->ops->read_folio_range(it, ctx, data);
		if (err) {
			fill->queued -= data;
			return err;
		}
	}
	memset(page + at + data, 0, span - data);
	return iomap_iter_advance(it, span);
}

void iomap_read_folio(const struct iomap_ops *ops, struct iomap_read_folio_ctx *ctx, void *private)
{
	struct folio *f = ctx->cur_folio;
	struct kshim_fill fill = { 0, 0, 0 };
	struct iomap_iter it = {
		.inode = f->mapping->host,
		.pos = folio_pos(f),
		.len = folio_size(f),
		.private = private,
	};
	int r;

	f->kshim_fill = &fill;
	while ((r = iomap_iter(&it, ops)) > 0)
		it.kshim_work = fill_extent(&it, ctx, &fill);
	read_flush(&it, ctx);
	f->kshim_fill = NULL;

	if (r == 0 && it.len == 0 && !fill.error && fill.completed == fill.queued) {
		folio_mark_uptodate(f);
	} else {
		printk(KERN_ERR "kshim: read of folio %lu failed (walk %d, io %d, %lu of %lu bytes back)\n",
				(unsigned long)f->index, r, fill.error,
				(unsigned long)fill.completed, (unsigned long)fill.queued);
		folio_clear_uptodate(f);
	}
	folio_unlock(f);
}

/* ------------------------------------------------------------ writeback */

bool iomap_dirty_folio(struct address_space *m, struct folio *f)
{
	(void)m;
	return folio_mark_dirty(f);
}

/*
 * Writes every dirty folio of the inode's mapping: the bytes below EOF are offered to the
 * core's writeback_range callback piece by piece (it maps an extent and passes the piece
 * to iomap_add_to_ioend).  A folio that fails stays dirty for a later attempt.
 */
int iomap_writepages(struct iomap_writepage_ctx *wpc)
{
	struct address_space *m = wpc->inode->i_mapping;
	struct folio *f = NULL;
	int err = 0;

	while ((f = writeback_iter(m, wpc->wbc, f, &err)) != NULL) {
		u64 isize = (u64)i_size_read(wpc->inode);
		u64 pos = (u64)folio_pos(f);
		u64 stop = min_t(u64, isize, (u64)folio_next_pos(f));
		int ferr = 0;

		folio_start_writeback(f);
		while (pos < stop) {
			ssize_t_ n = wpc->ops->writeback_range(wpc, f, pos, (unsigned int)(stop - pos), stop);
			if (n <= 0) {
				ferr = n < 0 ? (int)n : -EIO;
				break;
			}
			pos += (u64)n;
		}
		if (ferr) {
			folio_mark_dirty(f);
			err = ferr;
		}
		folio_end_writeback(f);
		folio_unlock(f);
	}
	if (wpc->ops->writeback_submit)
		err = wpc->ops->writeback_submit(wpc, err);
	return err;
}

/*
 * Writes the piece [pos, pos + len) of the folio that lies inside wpc->iomap, at once.
 * Returns the bytes consumed.  The write is widened to whole sectors when that stays
 * inside both the folio and the extent: bytes past EOF in the last sector are don't-care.
 */
ssize_t_ iomap_add_to_ioend(struct iomap_writepage_ctx *wpc, struct folio *f, loff_t pos, loff_t end_pos,
		unsigned int len)
{
	const struct iomap *e = &wpc->iomap;
	loff_t ext_end = e->offset + (loff_t)e->length;
	size_t at = offset_in_folio(f, pos);
	size_t n, io;
	int err, direct;

	(void)end_pos;
	if (pos < e->offset || pos >= ext_end)
		return -EIO;
	n = min_t(u64, len, (u64)(ext_end - pos));
	if (e->type != IOMAP_MAPPED && e->type != IOMAP_UNWRITTEN) {
		printk(KERN_WARNING "kshim: writeback of ino %lu at %lld over an extent of type %u skipped\n",
				wpc->inode->i_ino, (long long)pos, e->type);
		return (ssize_t_)n;
	}
	io = round_up(n, (size_t)SECTOR_SIZE);
	if (at + io > folio_size(f) || pos + (loff_t)io > ext_end)
		io = n;
	direct = kshim_is_data_inode && kshim_is_data_inode(wpc->inode);
	e->bdev->kshim_direct += direct;
	err = kshim_dev_rw(e->bdev, 1, e->addr + (u64)(pos - e->offset), (u8 *)folio_address(f) + at, io);
	e->bdev->kshim_direct -= direct;
	return err ? err : (ssize_t_)n;
}

int iomap_ioend_writeback_submit(struct iomap_writepage_ctx *wpc, int error)
{
	(void)wpc;
	return error;
}
