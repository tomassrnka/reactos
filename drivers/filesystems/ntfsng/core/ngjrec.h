/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NGJREC_H
#define NGJREC_H

#define NGJ_MAXRUNS 32
enum { NGJ_NONE, NGJ_CLEAN, NGJ_REPAIR };

struct ngj_run { s64 vcn, lcn, len; };

struct ngj_vol {
	void *osdev;
	u32 bps, cluster, recsz;
	u64 mft_lcn, mirr_lcn, serial;
	struct ngj_run mft[NGJ_MAXRUNS];
	int nmft;
	struct kj_ext ext[NGJ_MAXRUNS];
	int next;
	u64 lf_pages;
	u32 replayed;
	int torn, cleared_dirty;
};

int ngj_probe(void *osdev, u64 size, struct ngj_vol *jv);
int ngj_recover(struct ngj_vol *jv, int write, u64 *seq);

#endif
