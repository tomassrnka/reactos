/*
 * PROJECT:     ReactOS virtio-gpu 2D miniport (nitro / GFX-02)
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     virtio-gpu 2D control protocol (VIRTIO 1.x spec, section "GPU Device")
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#ifndef _VIRTIO_GPU_H_
#define _VIRTIO_GPU_H_

/* virtio device id */
#define VIRTIO_ID_GPU                           16

/* feature bits */
#define VIRTIO_GPU_F_VIRGL                      0
#define VIRTIO_GPU_F_EDID                       1

#define VIRTIO_GPU_MAX_SCANOUTS                 16

/* control queue 2D command types */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107
#define VIRTIO_GPU_CMD_GET_EDID                 0x010a

/* cursor queue command types */
#define VIRTIO_GPU_CMD_UPDATE_CURSOR            0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR              0x0301

/* success responses */
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_OK_EDID                 0x1104
/* error responses */
#define VIRTIO_GPU_RESP_ERR_UNSPEC              0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202

#define VIRTIO_GPU_FLAG_FENCE                   (1 << 0)

/* 2D resource formats (byte order in memory: B,G,R,X) */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM        1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM        2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM        3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM        4

#include <pshpack1.h>

typedef struct _VIRTIO_GPU_CTRL_HDR {
    ULONG     type;
    ULONG     flags;
    ULONGLONG fence_id;
    ULONG     ctx_id;
    ULONG     padding;
} VIRTIO_GPU_CTRL_HDR;

typedef struct _VIRTIO_GPU_RECT {
    ULONG x;
    ULONG y;
    ULONG width;
    ULONG height;
} VIRTIO_GPU_RECT;

typedef struct _VIRTIO_GPU_DISPLAY_ONE {
    VIRTIO_GPU_RECT r;
    ULONG enabled;
    ULONG flags;
} VIRTIO_GPU_DISPLAY_ONE;

typedef struct _VIRTIO_GPU_RESP_DISPLAY_INFO {
    VIRTIO_GPU_CTRL_HDR    hdr;
    VIRTIO_GPU_DISPLAY_ONE pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} VIRTIO_GPU_RESP_DISPLAY_INFO;

typedef struct _VIRTIO_GPU_RESOURCE_CREATE_2D {
    VIRTIO_GPU_CTRL_HDR hdr;
    ULONG resource_id;
    ULONG format;
    ULONG width;
    ULONG height;
} VIRTIO_GPU_RESOURCE_CREATE_2D;

typedef struct _VIRTIO_GPU_RESOURCE_UNREF {
    VIRTIO_GPU_CTRL_HDR hdr;
    ULONG resource_id;
    ULONG padding;
} VIRTIO_GPU_RESOURCE_UNREF;

typedef struct _VIRTIO_GPU_MEM_ENTRY {
    ULONGLONG addr;
    ULONG length;
    ULONG padding;
} VIRTIO_GPU_MEM_ENTRY;

typedef struct _VIRTIO_GPU_RESOURCE_ATTACH_BACKING {
    VIRTIO_GPU_CTRL_HDR hdr;
    ULONG resource_id;
    ULONG nr_entries;
    /* followed by nr_entries * VIRTIO_GPU_MEM_ENTRY */
} VIRTIO_GPU_RESOURCE_ATTACH_BACKING;

typedef struct _VIRTIO_GPU_RESOURCE_DETACH_BACKING {
    VIRTIO_GPU_CTRL_HDR hdr;
    ULONG resource_id;
    ULONG padding;
} VIRTIO_GPU_RESOURCE_DETACH_BACKING;

typedef struct _VIRTIO_GPU_SET_SCANOUT {
    VIRTIO_GPU_CTRL_HDR hdr;
    VIRTIO_GPU_RECT r;
    ULONG scanout_id;
    ULONG resource_id;
} VIRTIO_GPU_SET_SCANOUT;

typedef struct _VIRTIO_GPU_TRANSFER_TO_HOST_2D {
    VIRTIO_GPU_CTRL_HDR hdr;
    VIRTIO_GPU_RECT r;
    ULONGLONG offset;
    ULONG resource_id;
    ULONG padding;
} VIRTIO_GPU_TRANSFER_TO_HOST_2D;

typedef struct _VIRTIO_GPU_RESOURCE_FLUSH {
    VIRTIO_GPU_CTRL_HDR hdr;
    VIRTIO_GPU_RECT r;
    ULONG resource_id;
    ULONG padding;
} VIRTIO_GPU_RESOURCE_FLUSH;

/* device config space (BAR device cfg) */
typedef struct _VIRTIO_GPU_CONFIG {
    ULONG events_read;
    ULONG events_clear;
    ULONG num_scanouts;
    ULONG num_capsets;
} VIRTIO_GPU_CONFIG;

#include <poppack.h>

#endif /* _VIRTIO_GPU_H_ */
