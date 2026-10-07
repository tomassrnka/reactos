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

static int dread(struct ngj_vol *jv, u64 off, void *buf, unsigned int len)
{
	return ngos_dev_read(jv->osdev, off, buf, len) ? -EIO : 0;
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
		if (t == AT_END_T || len < 16 || off + len > size)
			return NULL;
		if (t == type && a[9] == 0)
			return a;
		off += len;
	}
	return NULL;
}

/* Mapping pairs of a non-resident attribute into runs (vcn, lcn, len); lcn -1 for a hole. */
static int decode_runs(const u8 *a, u32 alen, struct ngj_run *runs, int max)
{
	const u8 *p = a + g16(a + 0x20), *end = a + alen;
	s64 vcn = (s64)g64(a + 0x10), lcn = 0;
	int n = 0;
	while (p < end && *p) {
		int lb = *p & 0xf, ob = *p >> 4;
		s64 len = 0, d = 0;
		if (!lb || lb > 8 || ob > 8 || p + 1 + lb + ob > end || n >= max)
			return -EIO;
		for (int i = lb - 1; i >= 0; i--)
			len = len << 8 | p[1 + i];
		if (ob) {
			for (int i = ob - 1; i >= 0; i--)
				d = d << 8 | p[1 + lb + i];
			if (p[lb + ob] & 0x80)
				d -= (s64)1 << (8 * ob);
			lcn += d;
		}
		runs[n].vcn = vcn;
		runs[n].lcn = ob ? lcn : -1;
		runs[n].len = len;
		n++;
		vcn += len;
		p += 1 + lb + ob;
	}
	return n;
}

static int map_vcn(const struct ngj_run *runs, int n, s64 vcn, s64 *lcn)
{
	for (int i = 0; i < n; i++)
		if (vcn >= runs[i].vcn && vcn < runs[i].vcn + runs[i].len && runs[i].lcn >= 0) {
			*lcn = runs[i].lcn + (vcn - runs[i].vcn);
			return 0;
		}
	return -EIO;
}

/* Device offset of each cluster-or-record-sized piece of MFT record @no. */
static int rec_off(struct ngj_vol *jv, u64 no, u32 piece, u64 *dev)
{
	u64 b = no * jv->recsz + piece;
	s64 lcn;
	if (map_vcn(jv->mft, jv->nmft, (s64)(b / jv->cluster), &lcn))
		return -EIO;
	*dev = (u64)lcn * jv->cluster + b % jv->cluster;
	return 0;
}

static int read_rec(struct ngj_vol *jv, u64 no, u8 *r)
{
	u32 step = min_t(u32, jv->recsz, jv->cluster);
	for (u32 p = 0; p < jv->recsz; p += step) {
		u64 dev;
		if (rec_off(jv, no, p, &dev) || dread(jv, dev, r + p, step))
			return -EIO;
	}
	return unfix(r, jv->recsz);
}

int ngj_probe(void *osdev, u64 size, struct ngj_vol *jv)
{
	u8 *bs = kmalloc(4096, GFP_KERNEL), *r = NULL, *a;
	int err = -EINVAL, spc;
	s8 cpr;
	memset(jv, 0, sizeof(*jv));
	jv->osdev = osdev;
	if (!bs)
		return -ENOMEM;
	if (ngos_dev_read(osdev, 0, bs, 512) || memcmp(bs + 3, "NTFS    ", 8))
		goto out;
	jv->bps = g16(bs + 0x0b);
	spc = bs[0x0d];
	if (spc > 0x80)
		spc = 1 << (256 - spc);
	jv->cluster = jv->bps * spc;
	jv->mft_lcn = g64(bs + 0x30);
	jv->mirr_lcn = g64(bs + 0x38);
	cpr = (s8)bs[0x40];
	jv->recsz = cpr > 0 ? cpr * jv->cluster : 1u << -cpr;
	jv->serial = g64(bs + 0x48);
	if (jv->bps < 512 || jv->bps > 4096 || !jv->cluster || jv->cluster > 2u << 20 || jv->recsz < 1024 ||
	    jv->recsz > 4096 || jv->mft_lcn * jv->cluster >= size)
		goto out;
	r = kmalloc(jv->recsz, GFP_KERNEL);
	if (!r) {
		err = -ENOMEM;
		goto out;
	}
	/* $MFT record 0 at mft_lcn: its own $DATA runs locate the others. */
	{
		u32 step = min_t(u32, jv->recsz, jv->cluster);
		for (u32 p = 0; p < jv->recsz; p += step)
			if (dread(jv, jv->mft_lcn * jv->cluster + p, r + p, step))
				goto out;
	}
	err = -EIO;
	if (unfix(r, jv->recsz) || !(a = find_attr(r, jv->recsz, AT_DATA_T)) || !a[8])
		goto out;
	jv->nmft = decode_runs(a, g32(a + 4), jv->mft, NGJ_MAXRUNS);
	if (jv->nmft <= 0)
		goto out;
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
	return kj_page_dev(jv->ext, jv->next, page, &dev) || ngos_dev_write(jv->osdev, dev, buf, KJ_PAGE) ? -EIO : 0;
}

/* Clears VOLUME_IS_DIRTY in $Volume (record 3) and its $MFTMirr copy. */
static int clear_dirty(struct ngj_vol *jv, int *was)
{
	u8 *r = kmalloc(jv->recsz, GFP_KERNEL), *a;
	u32 step = min_t(u32, jv->recsz, jv->cluster);
	int err = -EIO;
	*was = 0;
	if (!r)
		return -ENOMEM;
	if (read_rec(jv, 3, r) || !(a = find_attr(r, jv->recsz, AT_VOLINFO_T)) || a[8] || g32(a + 0x10) < 12)
		goto out;
	a += g16(a + 0x14);
	if (!(g16(a + 10) & VOL_DIRTY)) {
		err = 0;
		goto out;
	}
	*was = 1;
	a[10] &= ~VOL_DIRTY;
	refix(r, jv->recsz);
	for (u32 p = 0; p < jv->recsz; p += step) {
		u64 dev;
		if (rec_off(jv, 3, p, &dev) || ngos_dev_write(jv->osdev, dev, r + p, step))
			goto out;
	}
	if (ngos_dev_write(jv->osdev, jv->mirr_lcn * jv->cluster + 3 * jv->recsz, r, jv->recsz))
		goto out;
	err = ngos_dev_flush(jv->osdev) ? -EIO : 0;
out:
	kfree(r);
	return err;
}

static int replay_sectors(struct ngj_vol *jv, const struct kj_desc *d, u8 *pg)
{
	for (int s = 0; s < 8; s++)
		if (d->mask & (1u << s) && ngos_dev_write(jv->osdev, d->blk * KJ_PAGE + s * 512, pg + s * 512, 512))
			return -EIO;
	return 0;
}

/*
 * Returns NGJ_NONE (no journal of ours: the caller's usual dirty-volume policy applies),
 * NGJ_CLEAN (consistent after an optional replay; dirty flag cleared when @write) or
 * NGJ_REPAIR (metadata went in place without the journal: needs a repair).
 */
int ngj_recover(struct ngj_vol *jv, int write, u64 *seq)
{
	u8 *pg = kmalloc(KJ_PAGE, GFP_KERNEL), *dp = kmalloc(KJ_PAGE, GFP_KERNEL);
	struct kj_hdr h;
	int res = NGJ_NONE, was = 0;
	*seq = 0;
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
	if (h.state == KJ_ST_UNJOURNALED) {
		res = NGJ_REPAIR;
		printk(KERN_ERR "journal: metadata was written in place without the journal (seq %llu): needs repair\n",
		       (unsigned long long)h.seq);
		goto out;
	}
	if (h.state != KJ_ST_ACTIVE && h.state != KJ_ST_COMMITTED)
		goto out;
	res = NGJ_CLEAN;
	if (h.state == KJ_ST_COMMITTED) {
		u32 crc = 0, total = h.ndesc + h.npages, i;
		int ok = h.ndesc == (h.npages + KJ_DESC_PER_PAGE - 1) / KJ_DESC_PER_PAGE &&
			 kj_slot_page(total) <= jv->lf_pages;
		for (i = 0; ok && i < total; i++) {
			if (lf_read(jv, kj_slot_page(i), pg)) {
				ok = 0;
				break;
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
				if (!write)
					continue;
				if (d->mask == 0xff ? ngos_dev_write(jv->osdev, d->blk * KJ_PAGE, pg, KJ_PAGE) :
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
	       write ? "" : " (read-only: not written)", was ? "cleared" : "was clear");
	if (jv->replayed)
		printk(KERN_WARNING "journal: replayed %u pages\n", jv->replayed);
out:
	kfree(pg);
	kfree(dp);
	return res;
}
