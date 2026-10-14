/*
 * PROJECT:     ReactOS mbed TLS
 * LICENSE:     Apache-2.0 (https://spdx.org/licenses/Apache-2.0.html)
 * PURPOSE:     Entropy source and C runtime glue of the mbed TLS DLL
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <stdarg.h>
#include <stdio.h>
#include <windef.h>
#include <winbase.h>

#include <mbedtls/build_info.h>

#include <mbedtls/entropy.h>
#include <mbedtls/threading.h>

#if defined(MBEDTLS_ENTROPY_HARDWARE_ALT)

/* advapi32 RtlGenRandom; the stock Windows entropy source asks bcrypt.dll, which is built on this DLL's neighbours */
BOOLEAN NTAPI SystemFunction036(PVOID Buffer, ULONG Length);

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    *olen = 0;

    while (len)
    {
        ULONG chunk = len > 0x10000 ? 0x10000 : (ULONG)len;

        if (!SystemFunction036(output, chunk))
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

        output += chunk;
        len -= chunk;
        *olen += chunk;
    }

    return 0;
}

#endif

/* The 5.2 msvcrt has no vsnprintf_s, which the Windows snprintf glue of platform.c calls; copies and terminates, -1 on truncation */
int vsnprintf_s(char *buffer, size_t size, size_t count, const char *format, va_list args)
{
    int ret;

    if (!buffer || !size)
        return -1;

    if (count >= size)
        count = size - 1;

    ret = _vsnprintf(buffer, count, format, args);
    if (ret < 0 || (size_t)ret > count)
    {
        buffer[count] = '\0';
        return -1;
    }

    buffer[ret] = '\0';
    return ret;
}

/* Mutexes of the library: a CRITICAL_SECTION behind the pointer of the mutex type */
static void ros_mutex_init(mbedtls_threading_mutex_t *mutex)
{
    CRITICAL_SECTION *cs = HeapAlloc(GetProcessHeap(), 0, sizeof(*cs));

    /* InitializeCriticalSection raises an exception when its debug data cannot be allocated; this returns an error */
    if (cs && !InitializeCriticalSectionAndSpinCount(cs, 0))
    {
        HeapFree(GetProcessHeap(), 0, cs);
        cs = NULL;
    }
    mutex->cs = cs;
}

static void ros_mutex_free(mbedtls_threading_mutex_t *mutex)
{
    if (mutex->cs)
    {
        DeleteCriticalSection(mutex->cs);
        HeapFree(GetProcessHeap(), 0, mutex->cs);
        mutex->cs = NULL;
    }
}

static int ros_mutex_lock(mbedtls_threading_mutex_t *mutex)
{
    if (!mutex->cs)
        return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;

    EnterCriticalSection(mutex->cs);
    return 0;
}

static int ros_mutex_unlock(mbedtls_threading_mutex_t *mutex)
{
    if (!mutex->cs)
        return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;

    LeaveCriticalSection(mutex->cs);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(instance);
            mbedtls_threading_set_alt(ros_mutex_init, ros_mutex_free, ros_mutex_lock, ros_mutex_unlock);
            break;

        case DLL_PROCESS_DETACH:
            if (!reserved)
                mbedtls_threading_free_alt();
            break;
    }

    return TRUE;
}
