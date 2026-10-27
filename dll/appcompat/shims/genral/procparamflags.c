/*
 * PROJECT:     ReactOS 'General' Shim library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     AddProcessParametersFlags shim
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <ntndk.h>
#include <shimlib.h>

/* Sets the bits that the command line holds (decimal, or hexadecimal with a 0x prefix) in the process parameters' Flags.
 * Only the bits in ALLOWED_FLAGS are applied: the other bits (NORMALIZED, the loader's DLL path bits, NX, ...) describe
 * how the parameter block is laid out or how the loader treats it, and setting them from a guessed command line would
 * corrupt the process. */
#define SHIM_NS         AddProcessParametersFlags
#include <setup_shim.inl>

#define SHIM_NUM_HOOKS  0
#define SHIM_NOTIFY_FN SHIM_OBJ_NAME(Notify)

#define ALLOWED_FLAGS   (RTL_USER_PROCESS_PARAMETERS_PROFILE_USER | \
                         RTL_USER_PROCESS_PARAMETERS_PROFILE_KERNEL | \
                         RTL_USER_PROCESS_PARAMETERS_PROFILE_SERVER | \
                         RTL_USER_PROCESS_PARAMETERS_CASE_SENSITIVE | \
                         RTL_USER_PROCESS_PARAMETERS_DISABLE_HEAP_CHECKS)

static ULONG ParseFlags(PCSTR Text)
{
    ULONG Value = 0, Base = 10, Digit;

    if (!Text)
        return 0;
    while (*Text == ' ' || *Text == '\t')
        ++Text;
    if (Text[0] == '0' && (Text[1] == 'x' || Text[1] == 'X'))
    {
        Base = 16;
        Text += 2;
    }
    for (;; ++Text)
    {
        if (*Text >= '0' && *Text <= '9')
            Digit = *Text - '0';
        else if (Base == 16 && *Text >= 'a' && *Text <= 'f')
            Digit = *Text - 'a' + 10;
        else if (Base == 16 && *Text >= 'A' && *Text <= 'F')
            Digit = *Text - 'A' + 10;
        else
            break;
        Value = Value * Base + Digit;
    }
    return Value;
}

BOOL WINAPI SHIM_OBJ_NAME(Notify)(DWORD fdwReason, PVOID ptr)
{
    if (fdwReason == SHIM_REASON_INIT)
    {
        PRTL_USER_PROCESS_PARAMETERS Params = NtCurrentPeb()->ProcessParameters;
        ULONG Requested = ParseFlags(SHIM_OBJ_NAME(g_szCommandLine));
        ULONG Flags = Requested & ALLOWED_FLAGS;

        if (Requested != Flags)
            SHIM_WARN("Ignoring process parameter flags 0x%x (not allowed)\n", Requested & ~ALLOWED_FLAGS);
        if (Params && Flags)
        {
            SHIM_INFO("Adding process parameter flags 0x%x\n", Flags);
            Params->Flags |= Flags;
        }
    }
    return TRUE;
}

#include <implement_shim.inl>
