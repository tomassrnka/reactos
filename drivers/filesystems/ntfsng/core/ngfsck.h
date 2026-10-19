/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ngfsck.h - the fast mount-time consistency check (ngfsck.c). */
#ifndef NGFSCK_H
#define NGFSCK_H

struct ngc_fsck_res {
	unsigned long long records, inuse, dirs, ientries, clusters, leaked_clusters;
	unsigned int fatal, leaks, dirs_skipped, clusters_skipped, ms;
	char why[128];		/* the first fatal finding */
};

/* 0 when the check ran (r->fatal says whether the volume is damaged); a negative errno if it could not run. */
int ngc_fsck(struct ntfs_volume *vol, struct block_device *b, struct ngc_fsck_res *r);

#endif
