/**
 * This file has no copyright assigned and is placed in the Public Domain.
 * This file is part of the mingw-w64 runtime package.
 * No warranty is given; refer to the file DISCLAIMER.PD within this package.
 *
 * Written by Kai Tietz  <kai.tietz@onevision.com>
 */

#ifdef CRTDLL
#undef CRTDLL
#endif

#include <sect_attribs.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <memory.h>
#include <malloc.h>
#ifndef __REACTOS__
#include <corecrt_startup.h>
#else
#include <internal.h>
#endif

/* The TLS directory. Only EXE startup (crtexe.c) and code with thread-local
   data (_tls_index) link this object; DLL startup does not, so a DLL without
   thread-local data has no TLS directory, as on Windows. The callbacks and
   __dyn_tls_init_callback are in tlsdyn.c.  */

#ifdef __GNUC__
/* One input section for everything that pulls this object in, so that
   --gc-sections cannot drop _tls_used while the rest stays linked.  */
#define _TLS_DIR_DATA _CRTALLOC(".data$tls_used")
#define _TLS_DIR_CONST
#else
#define _TLS_DIR_DATA
#define _TLS_DIR_CONST const
#endif

BOOL WINAPI __dyn_tls_init (HANDLE, DWORD, LPVOID);
BOOL WINAPI __dyn_tls_dtor (HANDLE, DWORD, LPVOID);

_TLS_DIR_DATA ULONG _tls_index = 0;

/* TLS raw template data start and end.
   We use here pointer-types for start/end so that tls-data remains
   aligned on pointer-size-width.  This seems to be required for
   pe-loader. */
_CRTALLOC(".tls") char *_tls_start = NULL;
_CRTALLOC(".tls$ZZZ") char *_tls_end = NULL;

_CRTALLOC(".CRT$XLA") PIMAGE_TLS_CALLBACK __xl_a = 0;
_CRTALLOC(".CRT$XLZ") PIMAGE_TLS_CALLBACK __xl_z = 0;

_TLS_DIR_DATA _TLS_DIR_CONST IMAGE_TLS_DIRECTORY _tls_used = {
  (ULONG_PTR) &_tls_start, (ULONG_PTR) &_tls_end,
  (ULONG_PTR) &_tls_index, (ULONG_PTR) (&__xl_a+1),
  (ULONG) 0, (ULONG) 0
};

_CRTALLOC(".CRT$XLC") PIMAGE_TLS_CALLBACK __xl_c = (PIMAGE_TLS_CALLBACK) __dyn_tls_init;
_CRTALLOC(".CRT$XLD") PIMAGE_TLS_CALLBACK __xl_d = (PIMAGE_TLS_CALLBACK) __dyn_tls_dtor;

_TLS_DIR_DATA int mingw_initltsdrot_force = 0;
_TLS_DIR_DATA int mingw_initltsdyn_force = 0;
_TLS_DIR_DATA int mingw_initltssuo_force = 0;
