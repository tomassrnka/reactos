/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     API definitions for api-ms-win-core-sysinfo-l1
 * COPYRIGHT:   Copyright 2024 Timo Kreuzer (timo.kreuzer@reactos.org)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

WINBASEAPI
VOID
WINAPI
GetSystemTimePreciseAsFileTime(
    _Out_ LPFILETIME lpSystemTimeAsFileTime);

WINBASEAPI
BOOL
WINAPI
GetSystemCpuSetInformation(
    _Out_writes_bytes_to_opt_(BufferLength, *ReturnedLength) PSYSTEM_CPU_SET_INFORMATION Information,
    _In_ ULONG BufferLength,
    _Out_ PULONG ReturnedLength,
    _In_opt_ HANDLE Process,
    _Reserved_ ULONG Flags);

#ifdef __cplusplus
} // extern "C"
#endif
