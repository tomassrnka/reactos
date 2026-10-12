/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     libntfs device operations on an NT volume (the I/O itself is in ntio.c)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "config.h"
#include "types.h"
#include "device.h"
#include "logging.h"
#include "ntio.h"

struct ntdev {
    struct ntio *io;
    s64 pos;
};

#define DEV(dev) ((struct ntdev *)(dev)->d_private)

static int ntdev_open(struct ntfs_device *dev, int flags)
{
    struct ntdev *d;
    int writable = (flags & O_ACCMODE) != O_RDONLY;

    if (NDevOpen(dev)) {
        errno = EBUSY;
        return -1;
    }
    d = calloc(1, sizeof(*d));
    if (!d)
        return -1;
    if (ntio_open(dev->d_name, writable, &d->io)) {
        free(d);
        return -1;
    }
    dev->d_private = d;
    if (!writable)
        NDevSetReadOnly(dev);
    else
        NDevClearReadOnly(dev);
    NDevSetBlock(dev);
    NDevSetOpen(dev);
    return 0;
}

static int ntdev_close(struct ntfs_device *dev)
{
    struct ntdev *d = DEV(dev);
    int ret;

    if (!NDevOpen(dev) || !d) {
        errno = EBADF;
        return -1;
    }
    ret = ntio_close(d->io);
    free(d);
    dev->d_private = NULL;
    NDevClearOpen(dev);
    return ret;
}

static s64 ntdev_seek(struct ntfs_device *dev, s64 offset, int whence)
{
    struct ntdev *d = DEV(dev);
    s64 pos;

    switch (whence) {
    case SEEK_SET:
        pos = offset;
        break;
    case SEEK_CUR:
        pos = d->pos + offset;
        break;
    case SEEK_END:
        pos = ntio_size(d->io);
        if (pos < 0)
            return -1;
        pos += offset;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (pos < 0) {
        errno = EINVAL;
        return -1;
    }
    d->pos = pos;
    return pos;
}

static s64 ntdev_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset)
{
    return ntio_pread(DEV(dev)->io, buf, count, offset);
}

static s64 ntdev_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset)
{
    if (NDevReadOnly(dev)) {
        NtioState.writes_refused++;
        errno = EROFS;
        return -1;
    }
    NDevSetDirty(dev);
    return ntio_pwrite(DEV(dev)->io, buf, count, offset);
}

static s64 ntdev_read(struct ntfs_device *dev, void *buf, s64 count)
{
    struct ntdev *d = DEV(dev);
    s64 r = ntio_pread(d->io, buf, count, d->pos);

    if (r > 0)
        d->pos += r;
    return r;
}

static s64 ntdev_write(struct ntfs_device *dev, const void *buf, s64 count)
{
    struct ntdev *d = DEV(dev);
    s64 r = ntdev_pwrite(dev, buf, count, d->pos);

    if (r > 0)
        d->pos += r;
    return r;
}

static int ntdev_sync(struct ntfs_device *dev)
{
    if (NDevReadOnly(dev))
        return 0;
    if (ntio_sync(DEV(dev)->io))
        return -1;
    NDevClearDirty(dev);
    return 0;
}

static int ntdev_stat(struct ntfs_device *dev, struct stat *buf)
{
    s64 size = ntio_size(DEV(dev)->io);

    memset(buf, 0, sizeof(*buf));
    buf->st_mode = S_IFBLK;
    if (size > 0)
        buf->st_size = (long)(size > 0x7fffffff ? 0x7fffffff : size);
    return 0;
}

static int ntdev_ioctl(struct ntfs_device *dev, unsigned long request, void *argp)
{
    struct ntdev *d = DEV(dev);

    switch (request) {
    case BLKGETSIZE64: {
        s64 size = ntio_size(d->io);
        if (size < 0)
            return -1;
        *(u64 *)argp = (u64)size;
        return 0;
    }
    case BLKSSZGET: {
        int ss = ntio_sector_size(d->io);
        if (ss <= 0)
            return -1;
        *(int *)argp = ss;
        return 0;
    }
    default:
        errno = EOPNOTSUPP;
        return -1;
    }
}

struct ntfs_device_operations ntfs_device_reactos_io_ops = {
    .open   = ntdev_open,
    .close  = ntdev_close,
    .seek   = ntdev_seek,
    .read   = ntdev_read,
    .write  = ntdev_write,
    .pread  = ntdev_pread,
    .pwrite = ntdev_pwrite,
    .sync   = ntdev_sync,
    .stat   = ntdev_stat,
    .ioctl  = ntdev_ioctl,
};
