/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * kshim_jnl.h - metadata block journal of the shim.  While active, every device write that is
 * not marked direct (file data) is held in an in-memory overlay; reads see the overlay.  A
 * commit writes the overlay to the journal area (inside $LogFile), flushes (which also makes the
 * file data written since the last commit durable), writes a checksummed header that commits the
 * transaction, flushes, writes it in place, flushes and marks the header applied.  A crash leaves
 * either the last applied state or a committed transaction that the next mount writes in place
 * again.
 */
#ifndef KSHIM_JNL_H
#define KSHIM_JNL_H

#define KJ_PAGE 4096
#define KJ_MAGIC "NTFSNGJ1"
#define KJ_VERSION 2
#define KJ_HDR_PAGE 3			/* $LogFile page of the header: never a power of two */
#define KJ_FIRST_SLOT_PAGE 5

enum { KJ_ST_ACTIVE = 1, KJ_ST_COMMITTED = 2, KJ_ST_UNJOURNALED = 3, KJ_ST_ERRORS = 4 };

struct kj_hdr {
	char magic[8];
	u32 version;
	u32 state;
	u64 seq;
	u32 npages;		/* data pages of the committed transaction */
	u32 ndesc;		/* descriptor pages before them */
	u32 payload_crc;	/* CRC-32 over the descriptor and data pages */
	u32 serial_lo, serial_hi;
	u32 page_size;
	u64 hwm;		/* highest slot count ever used */
	/*
	 * Update sequence number of $MFT record 3 ($Volume) on disk before and after the in-place pass
	 * of this transaction (equal for ACTIVE).  Every driver writes that record when it marks the
	 * volume dirty, and every write changes the number: a different number means another driver
	 * changed the volume since this header was written, and the header must not be acted on.
	 */
	u16 vol_usn_old, vol_usn_new;
	u32 flags;
	u32 hdr_crc;		/* CRC-32 of this structure up to here */
};

struct kj_desc {
	u64 blk;		/* device offset / KJ_PAGE */
	u32 crc;		/* CRC-32 of the data page */
	u8 mask;		/* valid 512-byte sectors */
	u8 pad[3];
};
#define KJ_DESC_PER_PAGE (KJ_PAGE / sizeof(struct kj_desc))

/* One run of the journal file ($LogFile) in KJ_PAGE units. */
struct kj_ext { u64 page; u64 npages; u64 dev; };

struct kshim_jnl;

u32 kj_crc32(u32 crc, const void *p, size_t n);
u64 kj_slot_page(u64 slot);
int kj_rec_usn(const u8 *rec, size_t avail, u16 *usn);
int kj_page_dev(const struct kj_ext *ext, int next, u64 page, u64 *dev);

int kshim_jnl_activate(struct block_device *b, const struct kj_ext *ext, int next, u64 lf_pages,
		u64 serial, u64 seq, u64 vol_rec_off);
int kshim_jnl_retire(struct block_device *b);
void kshim_jnl_deactivate(struct block_device *b);
int kshim_jnl_commit(struct block_device *b);	/* 1 committed (device flushed), 0 nothing to do, <0 error */
void kshim_jnl_mark_errors(struct block_device *b);
unsigned long kshim_jnl_capacity(struct block_device *b);
int kshim_jnl_ro_page(struct block_device *b, u64 blk, u8 mask, const u8 *data);
unsigned long kshim_jnl_pending(struct block_device *b);
void kshim_jnl_report(struct block_device *b);
/* Test only: nonzero stops the machine half way through writing that commit in place. */
extern unsigned long kshim_jnl_fault;

/* Used by kshim_dev_rw. */
int kshim_jnl_capture(struct block_device *b, u64 off, const u8 *buf, size_t len);
void kshim_jnl_degrade(struct block_device *b);
void kshim_jnl_patch(struct block_device *b, int to_overlay, u64 off, u8 *buf, size_t len);

/* Positive increments of a watched counter (the core's free-cluster count) are counted. */
int kshim_watch_add(atomic64_t *v);
void kshim_watch_del(atomic64_t *v);
unsigned long kshim_watch_count(atomic64_t *v);
void kshim_watch_hit(atomic64_t *v);

#endif
