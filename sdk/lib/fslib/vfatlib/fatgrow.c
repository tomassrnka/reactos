/*
 * PROJECT:     ReactOS VFAT filesystem library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Growing a FAT volume offline, FAT tables included
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * The data area starts after the FATs, so larger FATs move its start up and
 * every cluster number down.  The FATs grow by a whole number of clusters, m:
 * clusters 2 to m+1 (old numbering) end up under the FATs, so whatever they
 * hold moves to free clusters first; then old cluster c becomes cluster c - m
 * at the same place on the disk, in the FAT, in every directory entry, in the
 * FAT32 root cluster field.  On FAT12/16 the root directory moves up with the
 * FATs.  Format reference: the FAT specification (Microsoft, "FAT: General
 * Overview of On-Disk Format", 1.03).
 */

#include "fatgrow.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/*
 * The cluster counts the formatter (fat12.c, fat16.c, fat32.c) writes: the type is the same on every
 * reader (FAT12 below 4085 by the specification, below 4087 for FastFAT) and no cluster number
 * reaches the reserved values 0xFF0, 0xFFF0 and 0x0FFFFFF0.
 */
#define FAT12_MAX_CLUSTERS 4078
#define FAT16_MIN_CLUSTERS 4087
#define FAT16_MAX_CLUSTERS 65518
#define FAT32_MIN_CLUSTERS 65525
#define FAT32_MAX_CLUSTERS 0x0FFFFFEE

struct ref
{
    u32 loc_cluster;    /* directory cluster holding the entry, 0 for the FAT12/16 root region */
    u32 loc_offset;     /* byte offset of the entry in that cluster or region */
    u32 value;          /* its first-cluster field */
};

struct fg
{
    const fatgrow_io *io;
    u32 bps, spc, res, nfats, rootsecs, fatsz, totsec, bits, rootclus, fsinfo, bkboot;
    u32 fds;            /* first data sector */
    u32 clusters;       /* old cluster count */
    u32 *fat;           /* entries in old numbering, sized for the grown volume */
    u32 fat_count;
    u8 *root;           /* FAT12/16 root region */
    struct ref *refs;
    u32 nrefs, cap_refs;
    u8 *boot;
    u8 *visited;        /* directory clusters walked (one bit each) */
};

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static int is_pow2(u32 v) { return v && !(v & (v - 1)); }

static u64 cluster_offset(const struct fg *g, u32 c)
{
    return ((u64)g->fds + (u64)(c - 2) * g->spc) * g->bps;
}

/* Values at or above this are end-of-chain or bad markers, below 2 free or reserved. */
static u32 marker_floor(u32 bits)
{
    return bits == 12 ? 0xFF7 : bits == 16 ? 0xFFF7 : 0x0FFFFFF7;
}

static int is_pointer(const struct fg *g, u32 v)
{
    return v >= 2 && v < marker_floor(g->bits);
}

static u32 get_entry(const u8 *fat, u32 bits, u32 i)
{
    if (bits == 12)
    {
        u32 w = rd16(fat + i * 3 / 2);
        return (i & 1) ? (w >> 4) : (w & 0xFFF);
    }
    if (bits == 16)
        return rd16(fat + i * 2);
    return rd32(fat + i * 4) & 0x0FFFFFFF;
}

static void put_entry(u8 *fat, u32 bits, u32 i, u32 v)
{
    if (bits == 12)
    {
        u8 *p = fat + i * 3 / 2;
        if (i & 1)
        {
            p[0] = (u8)((p[0] & 0x0F) | ((v & 0x0F) << 4));
            p[1] = (u8)(v >> 4);
        }
        else
        {
            p[0] = (u8)v;
            p[1] = (u8)((p[1] & 0xF0) | ((v >> 8) & 0x0F));
        }
    }
    else if (bits == 16)
        wr16(fat + i * 2, v);
    else
        wr32(fat + i * 4, v & 0x0FFFFFFF);
}

static int add_ref(struct fg *g, u32 loc_cluster, u32 loc_offset, u32 value)
{
    if (g->nrefs == g->cap_refs)
    {
        u32 cap = g->cap_refs ? g->cap_refs * 2 : 1024;
        struct ref *n = g->io->alloc(g->io->ctx, (u64)cap * sizeof(*n));
        u32 i;
        if (!n)
            return FG_ENOMEM;
        for (i = 0; i < g->nrefs; i++)
            n[i] = g->refs[i];
        if (g->refs)
            g->io->free(g->io->ctx, g->refs);
        g->refs = n;
        g->cap_refs = cap;
    }
    g->refs[g->nrefs].loc_cluster = loc_cluster;
    g->refs[g->nrefs].loc_offset = loc_offset;
    g->refs[g->nrefs].value = value;
    g->nrefs++;
    return FG_OK;
}

/*
 * Records the first-cluster fields of one directory's entries (buffer holds
 * one cluster, or the FAT12/16 root region).  New subdirectories go to the
 * queue.  *end is set at the end-of-directory entry.
 */
static int scan_entries(struct fg *g, const u8 *buf, u32 len, u32 loc_cluster, u32 *queue, u32 *qtail, u32 qcap, int *end)
{
    u32 off;
    int err;

    for (off = 0; off + 32 <= len; off += 32)
    {
        const u8 *e = buf + off;
        u32 cl;
        u8 attr = e[11];

        if (e[0] == 0x00)
        {
            *end = 1;
            return FG_OK;
        }
        if (e[0] == 0xE5 || attr == 0x0F)
            continue;
        cl = rd16(e + 26);
        if (g->bits == 32)
            cl |= (u32)rd16(e + 20) << 16;
        if ((attr & 0x08) && !(attr & 0x10))
            continue;
        if (cl == 0)
            continue;
        if (cl < 2 || cl > g->clusters + 1)
            return FG_EBADFS;
        err = add_ref(g, loc_cluster, off, cl);
        if (err)
            return err;
        if ((attr & 0x10) && e[0] != '.')
        {
            if (g->visited[cl >> 3] & (1 << (cl & 7)))
                return FG_EBADFS;
            g->visited[cl >> 3] |= (u8)(1 << (cl & 7));
            if (*qtail >= qcap)
                return FG_EBADFS;
            queue[(*qtail)++] = cl;
        }
    }
    return FG_OK;
}

/* Walks every directory from the root and records every first-cluster field. */
static int walk_dirs(struct fg *g)
{
    u32 csize = g->spc * g->bps, qhead = 0, qtail = 0, qcap = g->clusters + 1;
    u32 *queue = g->io->alloc(g->io->ctx, (u64)qcap * sizeof(u32));
    u8 *buf = g->io->alloc(g->io->ctx, csize);
    int err = FG_OK, end;

    if (!queue || !buf)
    {
        err = FG_ENOMEM;
        goto out;
    }
    if (g->bits != 32)
    {
        end = 0;
        err = scan_entries(g, g->root, g->rootsecs * g->bps, 0, queue, &qtail, qcap, &end);
    }
    else
    {
        if (g->rootclus < 2 || g->rootclus > g->clusters + 1)
        {
            err = FG_EBADFS;
            goto out;
        }
        g->visited[g->rootclus >> 3] |= (u8)(1 << (g->rootclus & 7));
        queue[qtail++] = g->rootclus;
    }
    while (!err && qhead < qtail)
    {
        u32 c = queue[qhead++], steps = 0;
        end = 0;
        while (!err && !end)
        {
            if (c < 2 || c > g->clusters + 1 || ++steps > g->clusters)
            {
                err = FG_EBADFS;
                break;
            }
            if (g->io->read(g->io->ctx, cluster_offset(g, c), buf, csize))
            {
                err = FG_EIO;
                break;
            }
            err = scan_entries(g, buf, csize, c, queue, &qtail, qcap, &end);
            if (err || end)
                break;
            c = g->fat[c];
            if (!is_pointer(g, c))
                break;
        }
    }
out:
    if (queue)
        g->io->free(g->io->ctx, queue);
    if (buf)
        g->io->free(g->io->ctx, buf);
    return err;
}

/* Rewrites the first-cluster field at every recorded place: directory clusters on disk, the root region in memory. */
static int patch_refs(struct fg *g, u8 *buf)
{
    u32 csize = g->spc * g->bps, i, cur = 0;
    int err = FG_OK;

    for (i = 0; i < g->nrefs && !err; i++)
    {
        struct ref *r = &g->refs[i];
        u8 *e;
        if (r->loc_cluster == 0)
        {
            e = g->root + r->loc_offset;
        }
        else
        {
            if (r->loc_cluster != cur)
            {
                if (cur && g->io->write(g->io->ctx, cluster_offset(g, cur), buf, csize))
                {
                    err = FG_EIO;
                    break;
                }
                cur = r->loc_cluster;
                if (g->io->read(g->io->ctx, cluster_offset(g, cur), buf, csize))
                {
                    err = FG_EIO;
                    break;
                }
            }
            e = buf + r->loc_offset;
        }
        wr16(e + 26, r->value & 0xFFFF);
        if (g->bits == 32)
            wr16(e + 20, r->value >> 16);
    }
    if (!err && cur && g->io->write(g->io->ctx, cluster_offset(g, cur), buf, csize))
        err = FG_EIO;
    return err;
}

/* Directory clusters in ascending order, so that patch_refs reads and writes each once. */
static void sort_refs(struct ref *r, u32 n)
{
    u32 gap, i, j;
    for (gap = n / 2; gap > 0; gap /= 2)
    {
        for (i = gap; i < n; i++)
        {
            struct ref t = r[i];
            for (j = i; j >= gap && r[j - gap].loc_cluster > t.loc_cluster; j -= gap)
                r[j] = r[j - gap];
            r[j] = t;
        }
    }
}

/* buf: fatsz sectors, zero-filled. */
static int write_fats(struct fg *g, u32 fatsz, u32 count, const u32 *entries, u8 *buf)
{
    u64 bytes = (u64)fatsz * g->bps;
    u32 i, k;
    int err = FG_OK;

    for (i = 0; i < count; i++)
        put_entry(buf, g->bits, i, entries[i]);
    if (g->bits == 32)
    {
        wr32(buf, entries[0]);
        wr32(buf + 4, entries[1]);
    }
    for (k = 0; k < g->nfats && !err; k++)
    {
        u64 off = ((u64)g->res + (u64)k * fatsz) * g->bps, done;
        for (done = 0; done < bytes && !err; done += 1024 * 1024)
        {
            u32 n = (u32)((bytes - done) < 1024 * 1024 ? (bytes - done) : 1024 * 1024);
            if (g->io->write(g->io->ctx, off + done, buf + done, n))
                err = FG_EIO;
        }
    }
    return err;
}

/*
 * Reads FAT 1 into g->fat: the entries of the clusters (pointers must stay inside the volume), and
 * the spare entries the FAT holds past them, which must be free: the grown volume takes them over.
 */
static int read_fat(struct fg *g, u32 count)
{
    u64 bytes = (u64)g->fatsz * g->bps, done;
    u64 held = bytes * 8 / g->bits;
    u8 *buf = g->io->alloc(g->io->ctx, bytes);
    u32 i;
    int err = FG_OK;

    if (!buf)
        return FG_ENOMEM;
    for (done = 0; done < bytes && !err; done += 1024 * 1024)
    {
        u32 n = (u32)((bytes - done) < 1024 * 1024 ? (bytes - done) : 1024 * 1024);
        if (g->io->read(g->io->ctx, (u64)g->res * g->bps + done, buf + done, n))
            err = FG_EIO;
    }
    for (i = 0; !err && i < g->clusters + 2; i++)
    {
        g->fat[i] = get_entry(buf, g->bits, i);
        if (i >= 2 && is_pointer(g, g->fat[i]) && g->fat[i] > g->clusters + 1)
            err = FG_EBADFS;
    }
    for (; !err && i < held && i < count; i++)
    {
        if (get_entry(buf, g->bits, i))
            err = FG_EBADFS;
    }
    if (!err && g->bits == 32)
    {
        /* Entries 0 and 1 whole, reserved bits included (media byte, clean-shutdown and error bits). */
        g->fat[0] = rd32(buf);
        g->fat[1] = rd32(buf + 4);
    }
    (void)count;
    g->io->free(g->io->ctx, buf);
    return err;
}

static int write_boot(struct fg *g, u64 new_sectors, u32 fatsz)
{
    u8 *b = g->boot;
    int err;

    if (g->bits != 32 && new_sectors <= 0xFFFF)
    {
        wr16(b + 0x13, (u32)new_sectors);
        wr32(b + 0x20, 0);
    }
    else
    {
        wr16(b + 0x13, 0);
        wr32(b + 0x20, (u32)new_sectors);
    }
    if (g->bits == 32)
        wr32(b + 0x24, fatsz);
    else
        wr16(b + 0x16, fatsz);
    err = g->io->write(g->io->ctx, 0, b, g->bps) ? FG_EIO : FG_OK;
    if (!err && g->bits == 32 && g->bkboot && g->bkboot < g->res &&
        g->io->write(g->io->ctx, (u64)g->bkboot * g->bps, b, g->bps))
        err = FG_EIO;
    return err;
}

static int write_fsinfo(struct fg *g, u32 free_clusters)
{
    u8 *s;
    int err = FG_OK;

    if (g->bits != 32 || !g->fsinfo || g->fsinfo >= g->res)
        return FG_OK;
    s = g->io->alloc(g->io->ctx, g->bps);
    if (!s)
        return FG_ENOMEM;
    if (g->io->read(g->io->ctx, (u64)g->fsinfo * g->bps, s, g->bps))
        err = FG_EIO;
    else if (rd32(s) == 0x41615252 && rd32(s + 484) == 0x61417272)
    {
        wr32(s + 488, free_clusters);
        wr32(s + 492, 0xFFFFFFFF);
        if (g->io->write(g->io->ctx, (u64)g->fsinfo * g->bps, s, g->bps))
            err = FG_EIO;
    }
    g->io->free(g->io->ctx, s);
    return err;
}

static int type_ok(u32 bits, u32 clusters)
{
    if (bits == 12)
        return clusters <= FAT12_MAX_CLUSTERS;
    if (bits == 16)
        return clusters >= FAT16_MIN_CLUSTERS && clusters <= FAT16_MAX_CLUSTERS;
    return clusters >= FAT32_MIN_CLUSTERS && clusters <= FAT32_MAX_CLUSTERS;
}

int fatgrow(const fatgrow_io *io, unsigned long long new_sectors, fatgrow_result *result)
{
    struct fg G, *g = &G;
    u32 fatsz2, d, m, newc, i, used, moved = 0, freec = 0, y;
    u32 *remap = 0, *newfat = 0;
    u8 *patchbuf = 0, *fatbuf = 0;
    u8 head[512];
    int err;

    for (i = 0; i < sizeof(G); i++)
        ((u8 *)&G)[i] = 0;
    g->io = io;
    if (result)
    {
        for (i = 0; i < sizeof(*result); i++)
            ((u8 *)result)[i] = 0;
    }

    if (io->read(io->ctx, 0, head, sizeof(head)))
        return FG_EIO;
    if (head[510] != 0x55 || head[511] != 0xAA)
        return FG_EBADFS;
    g->bps = rd16(head + 0x0B);
    g->spc = head[0x0D];
    g->res = rd16(head + 0x0E);
    g->nfats = head[0x10];
    g->rootsecs = g->bps ? (rd16(head + 0x11) * 32 + g->bps - 1) / g->bps : 0;
    g->totsec = rd16(head + 0x13) ? rd16(head + 0x13) : rd32(head + 0x20);
    g->fatsz = rd16(head + 0x16) ? rd16(head + 0x16) : rd32(head + 0x24);
    if (!is_pow2(g->bps) || g->bps < 512 || g->bps > 4096 || !is_pow2(g->spc) || !g->res ||
        !g->nfats || g->nfats > 4 || !g->fatsz || !g->totsec)
        return FG_EBADFS;
    g->fds = g->res + g->nfats * g->fatsz + g->rootsecs;
    if (g->totsec <= g->fds)
        return FG_EBADFS;
    g->clusters = (g->totsec - g->fds) / g->spc;
    if (rd16(head + 0x16) == 0)
    {
        g->bits = 32;
        g->rootclus = rd32(head + 0x2C);
        g->fsinfo = rd16(head + 0x30);
        g->bkboot = rd16(head + 0x32);
        if (g->rootsecs)
            return FG_EBADFS;
    }
    else
        g->bits = g->clusters < 4085 ? 12 : 16;
    if (!type_ok(g->bits, g->clusters) || (u64)g->fatsz * g->bps * 8 / g->bits < (u64)g->clusters + 2)
        return FG_EBADFS;
    if (new_sectors <= g->totsec || new_sectors > 0xFFFFFFFFULL)
        return FG_EINVAL;

    /* The smallest FATs, grown by whole clusters, that describe every cluster of the new size. */
    for (d = 0; ; d++)
    {
        u64 fds2 = g->fds + (u64)g->nfats * d;
        fatsz2 = g->fatsz + d;
        if (((u64)g->nfats * d) % g->spc)
            continue;
        if (new_sectors <= fds2)
            return FG_EINVAL;
        newc = (u32)((new_sectors - fds2) / g->spc);
        if ((u64)fatsz2 * g->bps * 8 / g->bits >= (u64)newc + 2)
            break;
    }
    if (!type_ok(g->bits, newc))
        return FG_ETYPE;
    m = g->nfats * d / g->spc;
    if (result)
    {
        result->old_fat_sectors = g->fatsz;
        result->new_fat_sectors = fatsz2;
        result->old_clusters = g->clusters;
        result->new_clusters = newc;
    }

    g->boot = io->alloc(io->ctx, g->bps);
    g->fat_count = newc + m + 2;
    g->fat = io->alloc(io->ctx, (u64)g->fat_count * sizeof(u32));
    g->visited = io->alloc(io->ctx, ((u64)g->clusters + 2) / 8 + 1);
    if (!g->boot || !g->fat || !g->visited)
    {
        err = FG_ENOMEM;
        goto out;
    }
    if (io->read(io->ctx, 0, g->boot, g->bps))
    {
        err = FG_EIO;
        goto out;
    }
    err = read_fat(g, g->fat_count);
    if (err)
        goto out;

    if (d == 0)
    {
        /* The FATs have room: only the size changes. */
        for (i = 2; i < newc + 2; i++)
        {
            if (!g->fat[i])
                freec++;
        }
        err = write_boot(g, new_sectors, g->fatsz);
        if (!err)
            err = write_fsinfo(g, freec);
        goto out;
    }

    if (g->bits != 32)
    {
        g->root = io->alloc(io->ctx, (u64)g->rootsecs * g->bps);
        if (!g->root)
        {
            err = FG_ENOMEM;
            goto out;
        }
        if (io->read(io->ctx, ((u64)g->res + (u64)g->nfats * g->fatsz) * g->bps, g->root, g->rootsecs * g->bps))
        {
            err = FG_EIO;
            goto out;
        }
    }
    err = walk_dirs(g);
    if (err)
        goto out;

    /*
     * Every buffer before the first write: from the first directory entry rewritten until the
     * boot sector is written the volume is in neither numbering, and must not stop half way.
     */
    remap = io->alloc(io->ctx, ((u64)m + 2) * sizeof(u32));
    newfat = io->alloc(io->ctx, ((u64)newc + 2) * sizeof(u32));
    patchbuf = io->alloc(io->ctx, (u64)g->spc * g->bps);
    fatbuf = io->alloc(io->ctx, (u64)fatsz2 * g->bps);
    if (!remap || !newfat || !patchbuf || !fatbuf)
    {
        err = FG_ENOMEM;
        goto out;
    }

    /* Out of the way of the FATs: clusters 2 .. m+1 move to free clusters, highest first. */
    for (used = 0, i = 2; i < m + 2 && i < g->fat_count; i++)
    {
        if (g->fat[i])
            used++;
    }
    y = g->fat_count - 1;
    if (used)
    {
        u32 csize = g->spc * g->bps;
        u8 *buf = patchbuf;     /* copies into free clusters change nothing yet */
        for (i = 2; i < m + 2 && !err; i++)
        {
            if (!g->fat[i])
                continue;
            if (g->fat[i] >= marker_floor(g->bits) && g->fat[i] < marker_floor(g->bits) + 1)
            {
                err = FG_EBADFS;    /* a bad cluster cannot move */
                break;
            }
            while (y >= m + 2 && g->fat[y])
                y--;
            if (y < m + 2)
            {
                err = FG_ENOSPC;
                break;
            }
            if (io->read(io->ctx, cluster_offset(g, i), buf, csize) ||
                io->write(io->ctx, cluster_offset(g, y), buf, csize))
            {
                err = FG_EIO;
                break;
            }
            g->fat[y] = g->fat[i];
            g->fat[i] = 0;
            remap[i] = y;
            moved++;
            y--;
        }
        if (err)
            goto out;
        for (i = 2; i < g->fat_count; i++)
        {
            if (is_pointer(g, g->fat[i]) && g->fat[i] < m + 2)
                g->fat[i] = remap[g->fat[i]];
        }
        for (i = 0; i < g->nrefs; i++)
        {
            if (g->refs[i].value < m + 2)
                g->refs[i].value = remap[g->refs[i].value];
            if (g->refs[i].loc_cluster && g->refs[i].loc_cluster < m + 2)
                g->refs[i].loc_cluster = remap[g->refs[i].loc_cluster];
        }
        if (g->bits == 32 && g->rootclus < m + 2)
            g->rootclus = remap[g->rootclus];
    }
    if (result)
        result->moved_clusters = moved;

    /* New numbering: old cluster c is cluster c - m. */
    newfat[0] = g->fat[0];
    newfat[1] = g->fat[1];
    for (i = 2; i < newc + 2; i++)
    {
        u32 v = g->fat[i + m];
        if (is_pointer(g, v))
        {
            if (v < m + 2)
            {
                err = FG_EBADFS;
                goto out;
            }
            v -= m;
        }
        newfat[i] = v;
        if (!v)
            freec++;
    }
    for (i = 0; i < g->nrefs; i++)
        g->refs[i].value -= m;
    sort_refs(g->refs, g->nrefs);

    /* Directory entries first (their clusters do not move again), then the root region, the FATs, the boot sector. */
    err = patch_refs(g, patchbuf);
    if (err)
        goto out;
    if (g->bits != 32 &&
        io->write(io->ctx, ((u64)g->res + (u64)g->nfats * fatsz2) * g->bps, g->root, g->rootsecs * g->bps))
    {
        err = FG_EIO;
        goto out;
    }
    err = write_fats(g, fatsz2, newc + 2, newfat, fatbuf);
    if (err)
        goto out;
    if (g->bits == 32)
        wr32(g->boot + 0x2C, g->rootclus - m);
    err = write_boot(g, new_sectors, fatsz2);
    if (!err)
        err = write_fsinfo(g, freec);

out:
    if (fatbuf)
        io->free(io->ctx, fatbuf);
    if (patchbuf)
        io->free(io->ctx, patchbuf);
    if (newfat)
        io->free(io->ctx, newfat);
    if (remap)
        io->free(io->ctx, remap);
    if (g->refs)
        io->free(io->ctx, g->refs);
    if (g->root)
        io->free(io->ctx, g->root);
    if (g->visited)
        io->free(io->ctx, g->visited);
    if (g->fat)
        io->free(io->ctx, g->fat);
    if (g->boot)
        io->free(io->ctx, g->boot);
    return err;
}
