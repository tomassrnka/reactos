// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * kshim_jnl.c - the shim's metadata block journal (see kshim_jnl.h).  Callers hold the NT glue's
 * volume lock, so the overlay needs no lock of its own.
 */
#include <kshim.h>
#include <kshim_jnl.h>

#define KJ_BUCKETS 1024
#define KJ_SECT 512

struct kj_ent {
	struct kj_ent *next;
	u64 blk;
	u8 mask;
	u8 *data;
};

struct kshim_jnl {
	struct kj_ent *hash[KJ_BUCKETS];
	unsigned long npages;
	struct kj_ext *ext;
	int next;
	u64 lf_pages;
	u64 capacity;			/* descriptor + data slots */
	u64 seq, hwm;
	u64 serial;
	int degraded;			/* the overlay failed: writes go in place under an UNJOURNALED header */
	int errors;			/* the core reported errors: the header keeps saying so */
	int readonly;			/* only a replay overlay for a read-only mount: no journal writes */
	u8 *hdrpage;
	unsigned long commits, committed_pages, max_tx, fallbacks, dropped, replay_pages;
};

static u32 kj_crc_table[256];
unsigned long kshim_jnl_fault;

u32 kj_crc32(u32 crc, const void *p, size_t n)
{
	const u8 *s = p;
	if (!kj_crc_table[1]) {
		for (u32 i = 0; i < 256; i++) {
			u32 c = i;
			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			kj_crc_table[i] = c;
		}
	}
	crc = ~crc;
	while (n--)
		crc = kj_crc_table[(crc ^ *s++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

/* Journal slots skip every $LogFile page at a power-of-two offset: restart-page scans read those. */
u64 kj_slot_page(u64 slot)
{
	u64 p = KJ_FIRST_SLOT_PAGE + slot;
	for (u64 q = 8; q <= p; q <<= 1)
		p++;
	return p;
}

int kj_page_dev(const struct kj_ext *ext, int next, u64 page, u64 *dev)
{
	for (int i = 0; i < next; i++)
		if (page >= ext[i].page && page < ext[i].page + ext[i].npages) {
			*dev = ext[i].dev + (page - ext[i].page) * KJ_PAGE;
			return 0;
		}
	return -EIO;
}

static int kj_in_area(struct kshim_jnl *j, u64 off, size_t len)
{
	for (int i = 0; i < j->next; i++) {
		u64 s = j->ext[i].dev, e = s + j->ext[i].npages * KJ_PAGE;
		if (off < e && off + len > s)
			return 1;
	}
	return 0;
}

static struct kj_ent *kj_find(struct kshim_jnl *j, u64 blk)
{
	for (struct kj_ent *e = j->hash[blk % KJ_BUCKETS]; e; e = e->next)
		if (e->blk == blk)
			return e;
	return NULL;
}

static void kj_drop_all(struct kshim_jnl *j)
{
	for (int i = 0; i < KJ_BUCKETS; i++) {
		struct kj_ent *e = j->hash[i], *n;
		for (; e; e = n) {
			n = e->next;
			kfree(e->data);
			kfree(e);
		}
		j->hash[i] = NULL;
	}
	j->npages = 0;
}

static int kj_hdr_write(struct block_device *b, struct kshim_jnl *j, u32 state, u32 npages, u32 ndesc,
		u32 pcrc, int flush)
{
	struct kj_hdr *h = (struct kj_hdr *)j->hdrpage;
	u64 dev;
	if (j->readonly)
		return -EROFS;
	if (j->errors && state == KJ_ST_ACTIVE)
		state = KJ_ST_ERRORS;
	memset(j->hdrpage, 0, KJ_PAGE);
	memcpy(h->magic, KJ_MAGIC, 8);
	h->version = KJ_VERSION;
	h->state = state;
	h->seq = j->seq;
	h->npages = npages;
	h->ndesc = ndesc;
	h->payload_crc = pcrc;
	h->serial_lo = (u32)j->serial;
	h->serial_hi = (u32)(j->serial >> 32);
	h->page_size = KJ_PAGE;
	h->hwm = j->hwm;
	h->hdr_crc = kj_crc32(0, h, offsetof(struct kj_hdr, hdr_crc));
	if (kj_page_dev(j->ext, j->next, KJ_HDR_PAGE, &dev) || ngos_dev_write(b->osdev, dev, j->hdrpage, KJ_PAGE))
		return -EIO;
	if (flush && ngos_dev_flush(b->osdev))
		return -EIO;
	return 0;
}

int kshim_jnl_activate(struct block_device *b, const struct kj_ext *ext, int next, u64 lf_pages,
		u64 serial, u64 seq)
{
	struct kshim_jnl *j;
	int err;
	if (b->jnl)
		return 0;
	if (lf_pages < 64)
		return -ENOSPC;
	j = kzalloc(sizeof(*j), GFP_KERNEL);
	if (!j)
		return -ENOMEM;
	j->ext = kmalloc_array(next, sizeof(*ext), GFP_KERNEL);
	j->hdrpage = kmalloc(KJ_PAGE, GFP_KERNEL);
	if (!j->ext || !j->hdrpage) {
		kfree(j->ext);
		kfree(j->hdrpage);
		kfree(j);
		return -ENOMEM;
	}
	memcpy(j->ext, ext, next * sizeof(*ext));
	j->next = next;
	j->lf_pages = lf_pages;
	j->serial = serial;
	j->seq = seq;
	while (kj_slot_page(j->capacity) < lf_pages)
		j->capacity++;
	err = kj_hdr_write(b, j, KJ_ST_ACTIVE, 0, 0, 0, 1);
	if (err) {
		kfree(j->ext);
		kfree(j->hdrpage);
		kfree(j);
		return err;
	}
	b->jnl = j;
	printk(KERN_WARNING "journal: active, %llu slots in $LogFile (%llu pages), seq %llu\n",
	       (unsigned long long)j->capacity, (unsigned long long)lf_pages, (unsigned long long)seq);
	return 0;
}

void kshim_jnl_deactivate(struct block_device *b)
{
	struct kshim_jnl *j = b->jnl;
	if (!j)
		return;
	kj_drop_all(j);
	b->jnl = NULL;
	kfree(j->ext);
	kfree(j->hdrpage);
	kfree(j);
}

unsigned long kshim_jnl_pending(struct block_device *b)
{
	return b->jnl ? b->jnl->npages : 0;
}

/* Holds a metadata write in the overlay; -ENOMEM means the caller writes in place. */
int kshim_jnl_capture(struct block_device *b, u64 off, const u8 *buf, size_t len)
{
	struct kshim_jnl *j = b->jnl;
	if (j->degraded)
		return -ENOMEM;
	if (kj_in_area(j, off, len)) {
		if (j->dropped++ < 4)
			printk(KERN_ERR "journal: dropped a write into the journal area at %llu len %zu\n",
			       (unsigned long long)off, len);
		return 0;
	}
	while (len) {
		u64 blk = off / KJ_PAGE;
		size_t in = off % KJ_PAGE, n = min_t(size_t, len, KJ_PAGE - in);
		struct kj_ent *e = kj_find(j, blk);
		if (!e) {
			e = kzalloc(sizeof(*e), GFP_NOFS);
			if (e)
				e->data = kmalloc(KJ_PAGE, GFP_NOFS);
			if (!e || !e->data) {
				kfree(e);
				return -ENOMEM;
			}
			e->blk = blk;
			e->next = j->hash[blk % KJ_BUCKETS];
			j->hash[blk % KJ_BUCKETS] = e;
			j->npages++;
		}
		/* A partial sector not held yet starts from the device copy; bits change only after that. */
		for (size_t s = in / KJ_SECT * KJ_SECT; s < in + n; s += KJ_SECT)
			if (!(e->mask & (1u << (s / KJ_SECT))) && (s < in || s + KJ_SECT > in + n) &&
			    ngos_dev_read(b->osdev, blk * KJ_PAGE + s, e->data + s, KJ_SECT))
				return -EIO;
		memcpy(e->data + in, buf, n);
		for (size_t s = in / KJ_SECT * KJ_SECT; s < in + n; s += KJ_SECT)
			e->mask |= 1u << (s / KJ_SECT);
		off += n;
		buf += n;
		len -= n;
	}
	return 0;
}

/* to_overlay 0: copy held sectors over a buffer read from the device; 1: update held sectors from a direct write. */
void kshim_jnl_patch(struct block_device *b, int to_overlay, u64 off, u8 *buf, size_t len)
{
	struct kshim_jnl *j = b->jnl;
	if (!j || !j->npages)
		return;
	while (len) {
		u64 blk = off / KJ_PAGE;
		size_t in = off % KJ_PAGE, n = min_t(size_t, len, KJ_PAGE - in);
		struct kj_ent *e = kj_find(j, blk);
		if (e) {
			for (size_t s = in / KJ_SECT * KJ_SECT; s < in + n; s += KJ_SECT) {
				size_t a, z;
				if (!(e->mask & (1u << (s / KJ_SECT))))
					continue;
				a = max_t(size_t, s, in);
				z = min_t(size_t, s + KJ_SECT, in + n);
				if (to_overlay)
					memcpy(e->data + a, buf + (a - in), z - a);
				else
					memcpy(buf + (a - in), e->data + a, z - a);
			}
		}
		off += n;
		buf += n;
		len -= n;
	}
}

static int cmp_ent(const void *a, const void *b)
{
	u64 x = (*(struct kj_ent *const *)a)->blk, y = (*(struct kj_ent *const *)b)->blk;
	return x < y ? -1 : x > y;
}

static int kj_apply(struct block_device *b, struct kj_ent **v, unsigned long n)
{
	for (unsigned long i = 0; i < n; i++) {
		struct kj_ent *e = v[i];
		u64 base = e->blk * KJ_PAGE;
		if (e->mask == 0xff) {
			if (ngos_dev_write(b->osdev, base, e->data, KJ_PAGE))
				return -EIO;
			continue;
		}
		for (int s = 0; s < 8; s++) {
			int z = s;
			if (!(e->mask & (1u << s)))
				continue;
			while (z + 1 < 8 && (e->mask & (1u << (z + 1))))
				z++;
			if (ngos_dev_write(b->osdev, base + s * KJ_SECT, e->data + s * KJ_SECT, (z - s + 1) * KJ_SECT))
				return -EIO;
			s = z;
		}
	}
	return 0;
}

static int kj_apply_buckets(struct block_device *b, struct kshim_jnl *j)
{
	for (int i = 0; i < KJ_BUCKETS; i++)
		for (struct kj_ent *e = j->hash[i]; e; e = e->next)
			if (kj_apply(b, &e, 1))
				return -EIO;
	return 0;
}

/* Merge-sort replacement for the shim's insertion sort: a transaction can hold thousands of pages. */
static void kj_sort(struct kj_ent **v, struct kj_ent **tmp, unsigned long n)
{
	if (n < 16) {
		sort(v, n, sizeof(*v), cmp_ent, NULL);
		return;
	}
	unsigned long h = n / 2, i = 0, k = 0, m = h;
	kj_sort(v, tmp, h);
	kj_sort(v + h, tmp, n - h);
	while (i < h && m < n)
		tmp[k++] = v[i]->blk <= v[m]->blk ? v[i++] : v[m++];
	while (i < h)
		tmp[k++] = v[i++];
	while (m < n)
		tmp[k++] = v[m++];
	memcpy(v, tmp, n * sizeof(*v));
}

int kshim_jnl_commit(struct block_device *b)
{
	struct kshim_jnl *j = b->jnl;
	struct kj_ent **v = NULL, **tmp = NULL;
	struct kj_desc *d = NULL;
	unsigned long n, k = 0, ndesc;
	u32 crc = 0;
	int err = 0;
	if (!j || (!j->npages && !j->degraded))
		return 0;	/* nothing written: the caller flushes the device itself */
	n = j->npages;
	ndesc = (n + KJ_DESC_PER_PAGE - 1) / KJ_DESC_PER_PAGE;
	if (j->readonly)
		return 0;
	if (n) {
		v = kmalloc_array(n, sizeof(*v), GFP_NOFS);
		tmp = kmalloc_array(n, sizeof(*v), GFP_NOFS);
		if (!v || !tmp) {
			/* No memory to build a transaction: in place, under an UNJOURNALED header. */
			j->fallbacks++;
			err = kj_hdr_write(b, j, KJ_ST_UNJOURNALED, 0, 0, 0, 1);
			if (!err)
				err = kj_apply_buckets(b, j);
			if (!err && ngos_dev_flush(b->osdev))
				err = -EIO;
			if (!err) {
				j->degraded = 0;
				err = kj_hdr_write(b, j, KJ_ST_ACTIVE, 0, 0, 0, 1);
			}
			goto done;
		}
		for (int i = 0; i < KJ_BUCKETS; i++)
			for (struct kj_ent *e = j->hash[i]; e; e = e->next)
				v[k++] = e;
		kj_sort(v, tmp, n);
	}
	if (j->degraded || ndesc + n > j->capacity) {
		/* Does not fit (or the overlay failed): in place under an UNJOURNALED header. */
		j->fallbacks++;
		printk(KERN_WARNING "journal: %lu pages do not fit (%llu slots) or degraded=%d: writing in place\n",
		       n, (unsigned long long)j->capacity, j->degraded);
		err = kj_hdr_write(b, j, KJ_ST_UNJOURNALED, 0, 0, 0, 1);
		if (!err)
			err = kj_apply(b, v, n);
		if (!err && ngos_dev_flush(b->osdev))
			err = -EIO;
		if (!err) {
			j->degraded = 0;
			err = kj_hdr_write(b, j, KJ_ST_ACTIVE, 0, 0, 0, 1);
		}
		goto done;
	}
	d = kmalloc(KJ_PAGE, GFP_NOFS);
	if (!d) {
		err = -ENOMEM;
		goto out;
	}
	for (unsigned long p = 0; p < ndesc && !err; p++) {
		u64 dev;
		memset(d, 0, KJ_PAGE);
		for (unsigned long q = 0; q < KJ_DESC_PER_PAGE && p * KJ_DESC_PER_PAGE + q < n; q++) {
			struct kj_ent *e = v[p * KJ_DESC_PER_PAGE + q];
			d[q].blk = e->blk;
			d[q].mask = e->mask;
			d[q].crc = kj_crc32(0, e->data, KJ_PAGE);
		}
		crc = kj_crc32(crc, d, KJ_PAGE);
		if (kj_page_dev(j->ext, j->next, kj_slot_page(p), &dev) || ngos_dev_write(b->osdev, dev, d, KJ_PAGE))
			err = -EIO;
	}
	for (unsigned long i = 0; i < n && !err; i++) {
		u64 dev;
		crc = kj_crc32(crc, v[i]->data, KJ_PAGE);
		if (kj_page_dev(j->ext, j->next, kj_slot_page(ndesc + i), &dev) ||
		    ngos_dev_write(b->osdev, dev, v[i]->data, KJ_PAGE))
			err = -EIO;
	}
	if (err)
		goto out;
	j->seq++;
	if (ndesc + n > j->hwm)
		j->hwm = ndesc + n;
	err = kj_hdr_write(b, j, KJ_ST_COMMITTED, (u32)n, (u32)ndesc, crc, 1);
	if (err)
		goto out;
	if (kshim_jnl_fault && j->commits + 1 >= kshim_jnl_fault && n >= 2) {
		kj_apply(b, v, n / 2);
		ngos_dev_flush(b->osdev);
		printk(KERN_ERR "journal: fault injected in commit %lu seq %llu: %lu of %lu pages in place\n",
		       j->commits + 1, (unsigned long long)j->seq, n / 2, n);
		ngos_bugcheck("journal fault injection", __FILE__, __LINE__);
	}
	err = kj_apply(b, v, n);
	if (!err && ngos_dev_flush(b->osdev))
		err = -EIO;
	if (err) {
		/*
		 * Part of a committed transaction may be in place.  The next commit would overwrite its
		 * slots, so from here on the header says the metadata needs repair until an in-place
		 * pass of the whole overlay succeeds.
		 */
		j->degraded = 1;
		kj_hdr_write(b, j, KJ_ST_UNJOURNALED, 0, 0, 0, 1);
		goto out;
	}
	err = kj_hdr_write(b, j, KJ_ST_ACTIVE, 0, 0, 0, 0);
done:
	if (!err) {
		j->commits++;
		j->committed_pages += n;
		if (n > j->max_tx)
			j->max_tx = n;
		kj_drop_all(j);
	}
out:
	if (err)
		printk(KERN_ERR "journal: commit of %lu pages failed %d\n", n, err);
	kfree(d);
	kfree(v);
	kfree(tmp);
	return err ? err : 1;
}

void kshim_jnl_report(struct block_device *b)
{
	struct kshim_jnl *j = b->jnl;
	if (!j)
		return;
	printk(KERN_WARNING "journal: %lu commits, %lu pages, largest %lu, %lu in-place fallbacks, %lu dropped, seq %llu, pending %lu\n",
	       j->commits, j->committed_pages, j->max_tx, j->fallbacks, j->dropped, (unsigned long long)j->seq,
	       j->npages);
}

/* ------------------------------------------------------------ watched counters */
#define KSHIM_WATCHES 32
static struct { atomic64_t *v; unsigned long n; } kshim_watch[KSHIM_WATCHES];

int kshim_watch_add(atomic64_t *v)
{
	for (int i = 0; i < KSHIM_WATCHES; i++)
		if (!kshim_watch[i].v) {
			kshim_watch[i].v = v;
			kshim_watch[i].n = 0;
			return 0;
		}
	return -ENOSPC;
}

void kshim_watch_del(atomic64_t *v)
{
	for (int i = 0; i < KSHIM_WATCHES; i++)
		if (kshim_watch[i].v == v)
			kshim_watch[i].v = NULL;
}

unsigned long kshim_watch_count(atomic64_t *v)
{
	for (int i = 0; i < KSHIM_WATCHES; i++)
		if (kshim_watch[i].v == v)
			return kshim_watch[i].n;
	return 0;
}

void kshim_watch_hit(atomic64_t *v)
{
	for (int i = 0; i < KSHIM_WATCHES; i++)
		if (kshim_watch[i].v == v)
			kshim_watch[i].n++;
}

/* The core reported errors: the header says so (needs repair) from now on, so no mount clears the dirty flag. */
void kshim_jnl_mark_errors(struct block_device *b)
{
	struct kshim_jnl *j = b->jnl;
	if (!j || j->errors || j->readonly)
		return;
	j->errors = 1;
	printk(KERN_ERR "journal: the core reported errors: the volume stays marked for repair\n");
	kj_hdr_write(b, j, KJ_ST_ERRORS, 0, 0, 0, 1);
}

unsigned long kshim_jnl_capacity(struct block_device *b)
{
	return b->jnl ? (unsigned long)b->jnl->capacity : 0;
}

/* Read-only mount: a committed transaction is shown through the overlay instead of being written. */
int kshim_jnl_ro_page(struct block_device *b, u64 blk, u8 mask, const u8 *data)
{
	struct kshim_jnl *j = b->jnl;
	struct kj_ent *e;
	if (!j) {
		j = kzalloc(sizeof(*j), GFP_KERNEL);
		if (!j)
			return -ENOMEM;
		j->readonly = 1;
		b->jnl = j;
	}
	e = kj_find(j, blk);
	if (!e) {
		e = kzalloc(sizeof(*e), GFP_KERNEL);
		if (e)
			e->data = kmalloc(KJ_PAGE, GFP_KERNEL);
		if (!e || !e->data) {
			kfree(e);
			return -ENOMEM;
		}
		e->blk = blk;
		e->next = j->hash[blk % KJ_BUCKETS];
		j->hash[blk % KJ_BUCKETS] = e;
		j->npages++;
	}
	for (int s = 0; s < 8; s++)
		if (mask & (1u << s)) {
			memcpy(e->data + s * KJ_SECT, data + s * KJ_SECT, KJ_SECT);
			e->mask |= 1u << s;
		}
	return 0;
}

/* The overlay could not take a write: everything goes in place until the next commit. */
void kshim_jnl_degrade(struct block_device *b)
{
	struct kshim_jnl *j = b->jnl;
	if (!j || j->degraded)
		return;
	j->degraded = 1;
	printk(KERN_ERR "journal: out of memory for the overlay: writing in place until the next commit\n");
	kj_hdr_write(b, j, KJ_ST_UNJOURNALED, 0, 0, 0, 1);
}
