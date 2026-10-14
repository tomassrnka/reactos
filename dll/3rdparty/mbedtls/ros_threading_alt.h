/*
 * PROJECT:     ReactOS mbed TLS
 * LICENSE:     Apache-2.0 (https://spdx.org/licenses/Apache-2.0.html)
 * PURPOSE:     Mutex type for MBEDTLS_THREADING_ALT (installed as mbedtls/threading_alt.h by import.sh)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef MBEDTLS_THREADING_ALT_H
#define MBEDTLS_THREADING_ALT_H

/* A pointer to a CRITICAL_SECTION made by ros_platform.c; no Windows header is needed here */
typedef struct mbedtls_threading_mutex_t
{
    void *cs;
} mbedtls_threading_mutex_t;

#endif /* MBEDTLS_THREADING_ALT_H */
