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
