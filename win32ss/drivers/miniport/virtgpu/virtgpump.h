/*
 * PROJECT:     ReactOS virtio-gpu 2D miniport (nitro / GFX-02)
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Core API shared between the miniport TU and the virtio/GPU core TU.
 *              Scalar-only: safe to include from both the miniport.h/video.h world
 *              and the ntddk/virtio world.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#ifndef _VIRTGPUMP_H_
#define _VIRTGPUMP_H_

#define VIRTGPU_TAG 'GPUV'

/*
 * virtio-gpu core (virtgpu_core.c). All pointers are opaque; 'ext' is the
 * miniport HwDeviceExtension, 'ctx' is the core context from VgpuCreate.
 * Integer returns: 0 == success, non-zero == failure.
 */
int  VgpuCreate(void *ext, void **pCtx);
void VgpuDestroy(void *ctx);

unsigned long VgpuGetModeCount(void *ctx);
int  VgpuGetMode(void *ctx, unsigned long index,
                 unsigned long *width, unsigned long *height);
unsigned long VgpuGetDefaultModeIndex(void *ctx);

int  VgpuSetModeByIndex(void *ctx, unsigned long index);

/* Physical base + byte length of the current scanout backing (for MapMemory). */
int  VgpuGetFramebuffer(void *ctx, unsigned long long *phys, unsigned long *len);

/* Accumulate a dirty rectangle to be pushed by the flush pump (optional path). */
void VgpuFlushRect(void *ctx, unsigned long x, unsigned long y,
                   unsigned long w, unsigned long h);

#endif /* _VIRTGPUMP_H_ */
