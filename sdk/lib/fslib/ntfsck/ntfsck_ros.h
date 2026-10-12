/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Internal header of the NT side of the port (compat.c, ntio.c, ntfschk.c).
 *              The imported sources never include it: their types clash with the NT headers.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _NTFSCK_ROS_H
#define _NTFSCK_ROS_H

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <winbase.h>
#include <winioctl.h>
#define NTOS_MODE_USER
#include <ndk/iofuncs.h>
#include <ndk/kefuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/psfuncs.h>
#include <ntstatus.h>

#ifndef FSCTL_ALLOW_EXTENDED_DASD_IO
#define FSCTL_ALLOW_EXTENDED_DASD_IO CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 32, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif

#include "config.h"

extern HANDLE NtfsckHeap;

/* compat.c: one complete line of the checker's stdout (0) or stderr (1) */
void NtfsckLine(int Stream, const char *Text);
void NtfsckFlushStreams(void);
void NtfsckResetStreams(void);

#endif /* _NTFSCK_ROS_H */
