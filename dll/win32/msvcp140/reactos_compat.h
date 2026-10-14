/* Declarations the headers of this tree lack for functions that msvcp90 code of Wine calls */
#ifndef __REACTOS_MSVCP140_COMPAT_H
#define __REACTOS_MSVCP140_COMPAT_H

#include <stdint.h>
#include <windef.h>

BOOLEAN WINAPI CreateSymbolicLinkA(LPCSTR link, LPCSTR target, DWORD flags);
BOOLEAN WINAPI CreateSymbolicLinkW(LPCWSTR link, LPCWSTR target, DWORD flags);

DWORD WINAPI GetCurrentProcessorNumber(void);

#ifndef _DEBUG
void __cdecl _invalid_parameter(const wchar_t *expr, const wchar_t *func, const wchar_t *file,
        unsigned int line, uintptr_t reserved);
#endif

#endif
