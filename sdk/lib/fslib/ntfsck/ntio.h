/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NT native volume I/O of the checker (ntio.c), in plain C types so that the
 *              upstream-facing device operations (ntdevice.c) need no Windows headers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _NTFSCK_NTIO_H
#define _NTFSCK_NTIO_H

struct ntio;

/* How a check takes the volume (set by NtfsckRun before the check) */
enum ntio_lock_mode {
    NTIO_LOCK_NONE = 0,     /* never lock */
    NTIO_LOCK_TRY,          /* lock if no one else has the volume open, else go on unlocked */
    NTIO_LOCK_REQUIRED,     /* fail the open when the volume cannot be locked */
};

struct ntio_policy {
    int lock_mode;          /* enum ntio_lock_mode */
    int dismount_on_close;  /* a write was made: dismount before the unlock */
    int flush_first;        /* have the file system write back its cached changes before the first read */
};

struct ntio_state {
    int opened;             /* times the device was opened during the check */
    int locked;             /* the last open holds the volume lock */
    int lock_status;        /* NTSTATUS of the last lock request, 0 if none was made */
    int open_status;        /* NTSTATUS of the last failed open */
    int flush_status;       /* NTSTATUS of the flush before the check, 0 if none was asked */
    int writes_refused;     /* writes attempted while the volume was open read-only */
    unsigned long long bytes_read;
    long long size;         /* of the last opened volume */
    int sector_size;
};

extern struct ntio_policy NtioPolicy;
extern struct ntio_state NtioState;

/* All return 0 (or a byte count) on success and -1 with errno set on failure. */
int ntio_open(const char *name, int writable, struct ntio **io);
int ntio_close(struct ntio *io);
long long ntio_pread(struct ntio *io, void *buf, long long count, long long offset);
long long ntio_pwrite(struct ntio *io, const void *buf, long long count, long long offset);
int ntio_sync(struct ntio *io);
long long ntio_size(struct ntio *io);
int ntio_sector_size(struct ntio *io);
/* Closes every device a check left open; returns how many there were. */
int ntio_close_all(void);

#endif /* _NTFSCK_NTIO_H */
