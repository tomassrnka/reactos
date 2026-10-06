/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ngos.h - the narrow bridge between the Linux-API shim (compiled without any
 * NT header) and the NT kernel.  Implemented in nt/ngos_nt.c.  Only basic C
 * types cross this boundary.
 */
#ifndef NGOS_H
#define NGOS_H

#include <stddef.h>
#include <stdint.h>

/* Opaque storage for a KEVENT (16 bytes on x86, 24 on x64). */
struct ngos_ev { long long w[4]; };

void *ngos_alloc(size_t n);
void ngos_free(void *p);
void ngos_print(const char *s);
void ngos_bugcheck(const char *what, const char *file, int line) __attribute__((noreturn));

void ngos_ev_init(struct ngos_ev *ev, int synchronization, int signaled);
void ngos_ev_set(struct ngos_ev *ev);
void ngos_ev_clear(struct ngos_ev *ev);
void ngos_ev_wait(struct ngos_ev *ev);
int ngos_ev_trywait(struct ngos_ev *ev);
int ngos_ev_state(struct ngos_ev *ev);

unsigned char ngos_spin_lock(uintptr_t *lock);
void ngos_spin_unlock(uintptr_t *lock, unsigned char irql);

void ngos_yield(void);
void ngos_sleep_ms(unsigned int ms);
unsigned long ngos_jiffies(void);
void ngos_time(long long *sec, long *nsec);

int ngos_dev_read(void *dev, unsigned long long off, void *buf, unsigned int len);

#endif
