// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ngjrec.c - mount-time side of the metadata journal: finds $LogFile on the raw volume (boot
 * sector, $MFT record 0 and record 2, decoded from the documented on-disk format, before the core
 * mounts), writes a committed transaction in place again and clears the dirty flag of a volume
 * whose journal shows it consistent.
 */
#include <kshim.h>
#include <kshim_jnl.h>
#include "ngjrec.h"

#define REC_FILE 0x454c4946u		/* "FILE" */
#define AT_DATA_T 0x80
#define AT_VOLINFO_T 0x70
#define AT_END_T 0xffffffffu
#define VOL_DIRTY 0x0001

static u16 g16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 g32(const u8 *p) { return g16(p) | (u32)g16(p + 2) << 16; }
static u64 g64(const u8 *p) { return g32(p) | (u64)g32(p + 4) << 32; }

/* Every raw access stays inside the volume and on device-sector boundaries (malformed volumes too). */
static int inside(struct ngj_vol *jv, u64 off, u64 len)
{
	return off < jv->size && len <= jv->size - off && !((off | len) & (jv->devsec - 1));
}

static int dread(struct ngj_vol *jv, u64 off, void *buf, unsigned int len)
{
	return inside(jv, off, len) && !ngos_dev_read(jv->osdev, off, buf, len) ? 0 : -EIO;
}

static int dwrite(struct ngj_vol *jv, u64 off, void *buf, unsigned int len)
{
	return inside(jv, off, len) && !ngos_dev_write(jv->osdev, off, buf, len) ? 0 : -EIO;
}

/* Multi-sector protection: check and undo the update sequence (stride 512). */
static int unfix(u8 *r, u32 size)
{
	u16 uo = g16(r + 4), uc = g16(r + 6);
	if (g32(r) != REC_FILE || uc != size / 512 + 1 || uo + 2u * uc > size || (uo & 1))
		return -EIO;
	for (u16 i = 1; i < uc; i++) {
		u8 *e = r + i * 512 - 2;
		if (e[0] != r[uo] || e[1] != r[uo + 1])
			return -EIO;
		e[0] = r[uo + 2 * i];
		e[1] = r[uo + 2 * i + 1];
	}
	return 0;
}

static void refix(u8 *r, u32 size)
{
	u16 uo = g16(r + 4), uc = g16(r + 6), usn = g16(r + uo) + 1;
	if (usn == 0 || usn == 0xffff)
		usn = 1;
	r[uo] = (u8)usn;
	r[uo + 1] = (u8)(usn >> 8);
	for (u16 i = 1; i < uc; i++) {
		u8 *e = r + i * 512 - 2;
		r[uo + 2 * i] = e[0];
		r[uo + 2 * i + 1] = e[1];
		e[0] = (u8)usn;
		e[1] = (u8)(usn >> 8);
	}
}

/* The first attribute of @type (unnamed for $DATA) in an unfixed record, or NULL. */
static u8 *find_attr(u8 *r, u32 size, u32 type)
{
	u32 off = g16(r + 0x14);
	while (off + 16 <= size) {
		u8 *a = r + off;
		u32 t = g32(a), len = g32(a + 4);
		/* off + 16 <= size here, so the subtraction cannot wrap (off + len can, in 32 bits). */
		if (t == AT_END_T || len < 16 || len > size - off)
			return NULL;
		if (t == type && a[9] == 0)
			return len >= (a[8] ? 0x40u : 0x18u) ? a : NULL;
		off += len;
	}
	return NULL;
}

/* Mapping pairs of a non-resident attribute into runs (vcn, lcn, len); lcn -1 for a hole. */
static int decode_runs(const u8 *a, u32 alen, struct ngj_run *runs, int max)
{
	const u8 *p, *end = a + alen;
	s64 vcn = (s64)g64(a + 0x10), lcn = 0;
	int n = 0;
	if (g16(a + 0x20) >= alen)
		return -EIO;
	p = a + g16(a + 0x20);
	while (p < end && *p) {
		int lb = *p & 0xf, ob = *p >> 4;
		u64 ulen = 0, ud = 0;
		s64 len, d;
		if (!lb || lb > 8 || ob > 8 || p + 1 + lb + ob > end || n >= max)
			return -EIO;
		for (int i = lb - 1; i >= 0; i--)
			ulen = ulen << 8 | p[1 + i];
		len = (s64)ulen;
		if (ob) {
			for (int i = ob - 1; i >= 0; i--)
				ud = ud << 8 | p[1 + lb + i];
			/* Sign extension in unsigned arithmetic: an 8-byte offset is already full width. */
			if (ob < 8 && (p[lb + ob] & 0x80))
				ud |= ~(u64)0 << (8 * ob);
			d = (s64)ud;
			if (d > ((s64)1 << 41) || d < -((s64)1 << 41))
				return -EIO;	/* the result would be out of range anyway; no overflow in the sum */
			lcn += d;
		}
		if (len <= 0 || len > (s64)1 << 40 || vcn > (s64)1 << 40 || lcn < 0 || lcn > (s64)1 << 40)
			return -EIO;
		runs[n].vcn = vcn;
		runs[n].lcn = ob ? lcn : -1;
		runs[n].len = len;
		n++;
		vcn += len;
		p += 1 + lb + ob;
	}
	return n;
}

/*
 * The first $MFT records are contiguous at mft_lcn and mirrored at mirr_lcn, so records 2 and 3
 * are read there; $MFT record 0 is not used (a half-applied transaction may have changed it).
 * A record that fails its update sequence check is taken from $MFTMirr.
 */
static int read_rec(struct ngj_vol *jv, u64 no, u8 *r)
{
	if (!dread(jv, jv->mft_lcn * jv->cluster + no * jv->recsz, r, jv->recsz) && !unfix(r, jv->recsz))
		return 0;
	if (!dread(jv, jv->mirr_lcn * jv->cluster + no * jv->recsz, r, jv->recsz) && !unfix(r, jv->recsz))
		return 0;
	return -EIO;
}

static int pow2(u64 x) { return x && !(x & (x - 1)); }

int ngj_probe(void *osdev, u64 size, unsigned int devsec, struct ngj_vol *jv)
{
	u8 *bs = kmalloc(4096, GFP_KERNEL), *r = NULL, *a;
	int err = -EINVAL, spc;
	s8 cpr;
	memset(jv, 0, sizeof(*jv));
	jv->osdev = osdev;
	jv->size = size;
	jv->devsec = devsec;
	if (!bs)
		return -ENOMEM;
	if (!pow2(devsec) || devsec < 512 || devsec > 4096 || dread(jv, 0, bs, devsec) || memcmp(bs + 3, "NTFS    ", 8))
		goto out;
	jv->bps = g16(bs + 0x0b);
	spc = bs[0x0d];
	if (spc > 0x80)
		spc = spc >= 0xec ? 1 << (256 - spc) : 0;
	jv->cluster = jv->bps * spc;
	jv->mft_lcn = g64(bs + 0x30);
	jv->mirr_lcn = g64(bs + 0x38);
	cpr = (s8)bs[0x40];
	if (cpr > 0 && cpr <= 4)
		jv->recsz = cpr * jv->cluster;
	else if (cpr >= -12 && cpr <= -10)
		jv->recsz = 1u << -cpr;
	jv->serial = g64(bs + 0x48);
	if (!pow2(jv->bps) || jv->bps < devsec || jv->bps > 4096 || !pow2(jv->cluster) || jv->cluster > 2u << 20 ||
	    jv->recsz < 1024 || jv->recsz > 4096 || jv->recsz % devsec ||
	    jv->mft_lcn >= size / jv->cluster || jv->mirr_lcn >= size / jv->cluster)
		goto out;
	r = kmalloc(jv->recsz, GFP_KERNEL);
	if (!r) {
		err = -ENOMEM;
		goto out;
	}
	err = -EIO;
	if (read_rec(jv, 2, r) || !(a = find_attr(r, jv->recsz, AT_DATA_T)) || !a[8])
		goto out;
	{
		struct ngj_run lr[NGJ_MAXRUNS];
		int n = decode_runs(a, g32(a + 4), lr, NGJ_MAXRUNS);
		u64 lfsize = g64(a + 0x30);
		if (n <= 0 || (jv->cluster > KJ_PAGE && jv->cluster % KJ_PAGE) || (jv->cluster < KJ_PAGE && KJ_PAGE % jv->cluster))
			goto out;
		jv->lf_pages = lfsize / KJ_PAGE;
		/* Runs in pages: a page must not straddle two runs (clusters below 4 KiB need contiguous runs). */
		for (int i = 0; i < n && jv->next < NGJ_MAXRUNS; i++) {
			u64 vb = (u64)lr[i].vcn * jv->cluster, len = (u64)lr[i].len * jv->cluster;
			if (lr[i].lcn < 0 || vb % KJ_PAGE || len % KJ_PAGE)
				goto out;
			jv->ext[jv->next].page = vb / KJ_PAGE;
			jv->ext[jv->next].npages = len / KJ_PAGE;
			jv->ext[jv->next].dev = (u64)lr[i].lcn * jv->cluster;
			jv->next++;
		}
		/* Only pages that are mapped from page 0 on, and at most 256 MiB of them. */
		{
			u64 covered = 0;
			for (int i = 0; i < jv->next && jv->ext[i].page == covered; i++)
				covered += jv->ext[i].npages;
			if (jv->lf_pages > covered)
				jv->lf_pages = covered;
			if (jv->lf_pages > 65536)
				jv->lf_pages = 65536;
		}
	}
	err = 0;
out:
	kfree(r);
	kfree(bs);
	return err;
}

static int lf_read(struct ngj_vol *jv, u64 page, void *buf)
{
	u64 dev;
	return kj_page_dev(jv->ext, jv->next, page, &dev) || dread(jv, dev, buf, KJ_PAGE) ? -EIO : 0;
}

static int lf_write(struct ngj_vol *jv, u64 page, void *buf)
{
	u64 dev;
	return kj_page_dev(jv->ext, jv->next, page, &dev) || dwrite(jv, dev, buf, KJ_PAGE) ? -EIO : 0;
}

/* Clears VOLUME_IS_DIRTY in $Volume (record 3) and its $MFTMirr copy. */
static int clear_dirty(struct ngj_vol *jv, int *was)
{
	u8 *r = kmalloc(jv->recsz, GFP_KERNEL), *a;
	int err = -EIO;
	*was = 0;
	if (!r)
		return -ENOMEM;
	if (read_rec(jv, 3, r) || !(a = find_attr(r, jv->recsz, AT_VOLINFO_T)) || a[8] || g32(a + 0x10) < 12 ||
	    g16(a + 0x14) + 12u > g32(a + 4))
		goto out;
	a += g16(a + 0x14);
	if (!(g16(a + 10) & VOL_DIRTY)) {
		err = 0;
		goto out;
	}
	*was = 1;
	a[10] &= ~VOL_DIRTY;
	refix(r, jv->recsz);
	if (dwrite(jv, jv->mft_lcn * jv->cluster + 3 * jv->recsz, r, jv->recsz) ||
	    dwrite(jv, jv->mirr_lcn * jv->cluster + 3 * jv->recsz, r, jv->recsz))
		goto out;
	err = ngos_dev_flush(jv->osdev) ? -EIO : 0;
out:
	kfree(r);
	return err;
}

/* The update sequence number of $MFT record 3 ($Volume) as it is on disk (no fixup check: one sector is enough). */
static int vol_usn(struct ngj_vol *jv, u8 *buf, u16 *usn)
{
	u64 off = jv->mft_lcn * jv->cluster + 3 * jv->recsz, base = off & ~(u64)(jv->devsec - 1);
	if (dread(jv, base, buf, jv->devsec))
		return -EIO;
	return kj_rec_usn(buf + (off - base), jv->devsec - (size_t)(off - base), usn);
}

static int replay_sectors(struct ngj_vol *jv, const struct kj_desc *d, u8 *pg)
{
	if (d->blk >= jv->size / KJ_PAGE)
		return -EIO;
	for (int s = 0; s < 8; s++)
		if (d->mask & (1u << s) && dwrite(jv, d->blk * KJ_PAGE + s * 512, pg + s * 512, 512))
			return -EIO;
	return 0;
}

/*
 * Returns NGJ_NONE (no journal of ours: the caller's usual dirty-volume policy applies),
 * NGJ_CLEAN (consistent after an optional replay; dirty flag cleared when @write) or
 * NGJ_REPAIR (metadata went in place without the journal: needs a repair).
 */
int ngj_recover(struct ngj_vol *jv, struct block_device *b, int write, u64 *seq)
{
	u8 *pg = kmalloc(KJ_PAGE, GFP_KERNEL), *dp = kmalloc(KJ_PAGE, GFP_KERNEL);
	struct kj_hdr h;
	int res = NGJ_NONE, was = 0;
	*seq = 0;
	jv->bdev = b;
	if (!pg || !dp)
		goto out;
	/* Restart pages in use (Windows, or anything else that wrote a log): not ours. */
	for (u64 p = 0; p < 2; p++)
		if (lf_read(jv, p, pg) || g32(pg) != 0xffffffffu)
			goto out;
	if (lf_read(jv, KJ_HDR_PAGE, pg))
		goto out;
	memcpy(&h, pg, sizeof(h));
	if (memcmp(h.magic, KJ_MAGIC, 8) || h.version != KJ_VERSION ||
	    h.hdr_crc != kj_crc32(0, &h, offsetof(struct kj_hdr, hdr_crc)) ||
	    ((u64)h.serial_hi << 32 | h.serial_lo) != jv->serial || h.page_size != KJ_PAGE)
		goto out;
	*seq = h.seq;
	if (h.state == KJ_ST_UNJOURNALED || h.state == KJ_ST_ERRORS) {
		res = NGJ_REPAIR;
		printk(KERN_ERR "journal: %s (seq %llu): needs repair\n", h.state == KJ_ST_ERRORS ?
		       "the core reported errors in the last session" : "metadata was written in place without the journal",
		       (unsigned long long)h.seq);
		goto out;
	}
	if (h.state != KJ_ST_ACTIVE && h.state != KJ_ST_COMMITTED)
		goto out;
	/*
	 * Ours only while $Volume still carries the number this header recorded (either side of the
	 * transaction's in-place pass).  Otherwise another driver wrote the volume after this header
	 * (the Linux core, for one, leaves an empty-looking $LogFile alone): neither replay the old
	 * transaction over its changes nor clear the dirty flag it may have set.
	 */
	{
		u16 usn;
		if (vol_usn(jv, pg, &usn) ||
		    (usn != h.vol_usn_new && (h.state != KJ_ST_COMMITTED || usn != h.vol_usn_old))) {
			printk(KERN_WARNING "journal: header seq %llu state %u is older than the volume ($Volume usn %u, header %u/%u): ignored\n",
			       (unsigned long long)h.seq, h.state, usn, h.vol_usn_old, h.vol_usn_new);
			goto out;
		}
	}
	res = NGJ_CLEAN;
	if (h.state == KJ_ST_COMMITTED) {
		u32 crc = 0, total = h.ndesc + h.npages, i;
		int ok = h.npages && h.npages < (1u << 24) &&
			 h.ndesc == (h.npages + KJ_DESC_PER_PAGE - 1) / KJ_DESC_PER_PAGE &&
			 kj_slot_page(total - 1) < jv->lf_pages;
		for (i = 0; ok && i < total; i++) {
			if (lf_read(jv, kj_slot_page(i), pg)) {
				/* Unreadable, not torn: the transaction may be half in place, keep the evidence. */
				res = NGJ_REPAIR;
				goto out;
			}
			crc = kj_crc32(crc, pg, KJ_PAGE);
		}
		if (ok && crc == h.payload_crc) {
			for (i = 0; i < h.npages; i++) {
				const struct kj_desc *d = (const struct kj_desc *)dp + i % KJ_DESC_PER_PAGE;
				if ((i % KJ_DESC_PER_PAGE == 0 && lf_read(jv, kj_slot_page(i / KJ_DESC_PER_PAGE), dp)) ||
				    lf_read(jv, kj_slot_page(h.ndesc + i), pg) || kj_crc32(0, pg, KJ_PAGE) != d->crc) {
					res = NGJ_REPAIR;
					goto out;
				}
				if (!write) {
					if (kshim_jnl_ro_page(jv->bdev, d->blk, d->mask, pg)) {
						res = NGJ_REPAIR;
						goto out;
					}
					continue;
				}
				if (d->mask == 0xff ? (d->blk >= jv->size / KJ_PAGE || dwrite(jv, d->blk * KJ_PAGE, pg, KJ_PAGE)) :
				    replay_sectors(jv, d, pg)) {
					res = NGJ_REPAIR;
					goto out;
				}
			}
			if (write && ngos_dev_flush(jv->osdev)) {
				res = NGJ_REPAIR;
				goto out;
			}
			jv->replayed = h.npages;
		} else {
			jv->torn = 1;
		}
	}
	if (h.flags & KJ_FL_ERRORS) {
		/* The transaction is in place (or shown), but the core found errors in that session. */
		res = NGJ_REPAIR;
		printk(KERN_ERR "journal: seq %llu: the core reported errors in the last session: needs repair\n",
		       (unsigned long long)h.seq);
		goto out;
	}
	if (write) {
		if (clear_dirty(jv, &was)) {
			res = NGJ_REPAIR;
			goto out;
		}
		jv->cleared_dirty = was;
		memset(pg, 0xff, KJ_PAGE);
		if (lf_write(jv, KJ_HDR_PAGE, pg) || ngos_dev_flush(jv->osdev))
			res = NGJ_REPAIR;
	}
	printk(KERN_WARNING "journal: found seq %llu state %u: %s%s, dirty flag %s\n",
	       (unsigned long long)h.seq, h.state,
	       jv->replayed ? "replayed a committed transaction" : jv->torn ? "transaction not committed (ignored)" : "nothing to replay",
	       write ? "" : " (read-only mount: shown through the overlay, not written)", was ? "cleared" : write ? "was clear" : "not touched");
	if (jv->replayed)
		printk(KERN_WARNING "journal: replayed %u pages\n", jv->replayed);
out:
	kfree(pg);
	kfree(dp);
	return res;
}
