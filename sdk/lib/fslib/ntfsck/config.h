/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Build configuration of the imported ntfsprogs-plus sources: replaces the
 *              config.h that autoconf generates upstream.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * Every upstream source includes this file first. It includes the C headers the sources
 * need, then redirects the C library calls that ntdll.dll does not export to the native
 * implementations in reactos/compat.c, so that the checker links into native images
 * (untfs.dll, autochk.exe) that import nothing but ntdll.dll.
 */
#ifndef _NTFSPROGS_REACTOS_CONFIG_H
#define _NTFSPROGS_REACTOS_CONFIG_H

#define PACKAGE "ntfsprogs-plus"
#define PACKAGE_NAME "ntfsprogs-plus"
#define PACKAGE_VERSION "1.0.0"
#define VERSION "1.0.0"
#define PACKAGE_STRING "ntfsprogs-plus 1.0.0"

#define WORDS_LITTLEENDIAN 1
#define STDC_HEADERS 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_ERRNO_H 1
#define HAVE_STDARG_H 1
#define HAVE_LIMITS_H 1
#define HAVE_CTYPE_H 1
#define HAVE_TIME_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDDEF_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_FCNTL_H 1
#define HAVE_WCHAR_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRCASECMP 1
#define HAVE_SNPRINTF 1
#define HAVE_VPRINTF 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_REALPATH 1
#define HAVE_FFS 1
#define HAVE_DAEMON 1
#define HAVE_STRSEP 1

/*
 * The device layer is ntdevice.c, on NT native I/O. device_io.h makes the default
 * operations the Unix ones when there is no <windows.h>; they are renamed to ours.
 */
#define ntfs_device_unix_io_ops ntfs_device_reactos_io_ops

/* Request codes of the device layer's ioctl, the values the upstream Windows build uses. */
#define BLKGETSIZE64 0x80041272
#define BLKSSZGET 0x1268

/* The checker is a library here: main() and its option parser are not built. */
#define NTFSCK_LIBRARY 1

/* Upstream DEBUG turns on debug logging and self-checks that exit(); builds here are release builds of it. */
#undef DEBUG

/* The build defines _CRT_DECLARE_NONSTDC_NAMES: the POSIX names of the CRT headers (dev_t, struct stat). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/types.h>
/* Before <wchar.h>, which defines _STAT_DEFINED without struct stat */
#include <sys/stat.h>
#include <wchar.h>

#ifndef PRIu64
#define PRIu64 "I64u"
#endif
#ifndef PRId64
#define PRId64 "I64d"
#endif
#ifndef PRIx64
#define PRIx64 "I64x"
#endif
#ifndef PRIi64
#define PRIi64 "I64i"
#endif

#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif

/* POSIX types and constants the upstream code uses and the Windows headers lack. */
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned int mode_t;
#endif
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#ifdef _WIN64
typedef long long ssize_t;
#else
typedef int ssize_t;
#endif
#endif

/* 64-bit file offsets: off_t is 32 bits on Windows. */
#define off_t long long

#ifndef S_IFBLK
#define S_IFBLK 0x3000
#endif
#ifndef S_ISBLK
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_IFLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0xC000
#endif
#ifndef S_IFIFO
#define S_IFIFO 0x1000
#endif
#ifndef S_ISVTX
#define S_ISVTX 01000
#endif
#ifndef S_ISGID
#define S_ISGID 02000
#endif
#ifndef S_ISUID
#define S_ISUID 04000
#endif
#ifndef S_IRWXU
#define S_IRWXU 0700
#endif
#ifndef S_IRWXG
#define S_IRWXG 0070
#endif
#ifndef S_IRWXO
#define S_IRWXO 0007
#endif

/* Device numbers of special files (kept in extended attributes) */
#ifndef major
#define major(dev) ((unsigned int)(((dev) >> 8) & 0xfff))
#define minor(dev) ((unsigned int)(((dev) & 0xff) | (((dev) >> 12) & 0xfff00)))
#define makedev(ma, mi) ((dev_t)((((ma) & 0xfff) << 8) | ((mi) & 0xff) | (((mi) & 0xfff00) << 12)))
#endif

#ifndef O_EXCL
#define O_EXCL 0x0400
#endif

/* errno values some sources use that the Windows CRT headers do not define. */
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 130
#endif
#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif
#ifndef EOVERFLOW
#define EOVERFLOW 132
#endif
#ifndef ENODATA
#define ENODATA 120
#endif
#ifndef ENOTBLK
#define ENOTBLK 15
#endif
#ifndef ETXTBSY
#define ETXTBSY 139
#endif
#ifndef ELOOP
#define ELOOP 114
#endif
#ifndef EMLINK
#define EMLINK 31
#endif
#ifndef ENOMSG
#define ENOMSG 122
#endif
#ifndef EIDRM
#define EIDRM 111
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY 41
#endif
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#ifndef ESTALE
#define ESTALE 116
#endif
#ifndef EDQUOT
#define EDQUOT 122
#endif
#ifndef EMEDIUMTYPE
#define EMEDIUMTYPE 124
#endif
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

#include "compat_ros.h"

#endif /* _NTFSPROGS_REACTOS_CONFIG_H */
