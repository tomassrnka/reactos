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

/* Sets the bits that the command line holds (decimal, or hexadecimal with a 0x prefix) in the process parameters' Flags */
#define SHIM_NS         AddProcessParametersFlags
#include <setup_shim.inl>

#define SHIM_NUM_HOOKS  0
#define SHIM_NOTIFY_FN SHIM_OBJ_NAME(Notify)

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
        ULONG Flags = ParseFlags(SHIM_OBJ_NAME(g_szCommandLine));

        if (Params && Flags)
        {
            SHIM_INFO("Adding process parameter flags 0x%x\n", Flags);
            Params->Flags |= Flags;
        }
    }
    return TRUE;
}

#include <implement_shim.inl>
