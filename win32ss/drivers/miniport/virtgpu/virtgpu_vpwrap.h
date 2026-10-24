/*
 * PROJECT:     ReactOS virtio-gpu 2D miniport (nitro / GFX-02)
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Scalar-only bridge so the ntddk/virtio core TU can reach the
 *              VideoPort services that only the miniport TU may include.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * Implemented in virtgpump.c (miniport.h/video.h world), called from
 * virtgpu_core.c (ntddk/virtio world). Only scalar types cross this boundary.
 */

#ifndef _VIRTGPU_VPWRAP_H_
#define _VIRTGPU_VPWRAP_H_

/* MMIO register access (addr is a mapped virtual address) */
unsigned char  Vpw_Read8 (void *addr);
unsigned short Vpw_Read16(void *addr);
unsigned long  Vpw_Read32(void *addr);
void Vpw_Write8 (void *addr, unsigned char  v);
void Vpw_Write16(void *addr, unsigned short v);
void Vpw_Write32(void *addr, unsigned long  v);

/* PCI config space. Returns 0 on success. */
int  Vpw_PciReadConfig (void *ext, int where, void *buf, int len);
int  Vpw_PciWriteConfig(void *ext, int where, void *buf, int len);

/* Map a device physical range (io != 0 => I/O space). Returns mapped VA or 0. */
void *Vpw_MapDevice(void *ext, unsigned long long phys, unsigned long len, int io);

/* Non-paged pool */
void *Vpw_AllocPool(void *ext, unsigned long len);
void  Vpw_FreePool (void *ext, void *p);

/* Busy stall, microseconds */
void Vpw_Stall(unsigned long usecs);

/* Debug logging (level: 0=error,1=info) */
void Vpw_Log(int level, const char *fmt, unsigned long long a, unsigned long long b);

/* Decode BAR [bar]: physical base, length, io-space flag. Returns 0 on success. */
int Vpw_GetBar(void *ext, int bar, unsigned long long *phys, unsigned long *len, int *io);

#endif /* _VIRTGPU_VPWRAP_H_ */
