/*
 * PROJECT:     ReactOS 'General' Shim library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     IgnoreChromeSandbox shim
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <shimlib.h>

/*
 * Chrome (and the Chromium based programs) start their child processes inside a restricted-token sandbox that
 * needs the Windows 8 and later job / token / mitigation support. This shim starts the children of these programs
 * with the '--no-sandbox' switch added to their command line when it is not there yet.
 *
 * FIXME: the exact set of command lines the Windows shim touches is not established here; this one only changes a
 * command line that starts a program named chrome.exe.
 */
typedef BOOL(WINAPI* CREATEPROCESSWPROC)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL,
                                         DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

#define SHIM_NS         IgnoreChromeSandbox
#include <setup_shim.inl>

#define NO_SANDBOX_SWITCH   L" --no-sandbox"

static WCHAR SandboxUpper(WCHAR Ch)
{
    return (Ch >= L'a' && Ch <= L'z') ? (WCHAR)(Ch - (L'a' - L'A')) : Ch;
}

/* Case insensitive search of a (short, ASCII) needle in the first Count characters of Text */
static BOOL SandboxFind(PCWSTR Text, SIZE_T Count, PCWSTR Needle)
{
    SIZE_T n, m;

    for (n = 0; n < Count; ++n)
    {
        for (m = 0; Needle[m] && n + m < Count; ++m)
        {
            if (SandboxUpper(Text[n + m]) != SandboxUpper(Needle[m]))
                break;
        }
        if (!Needle[m])
            return TRUE;
    }
    return FALSE;
}

/* TRUE when the program that is started is named chrome.exe: the application name, else the first token */
static BOOL SandboxIsChrome(PCWSTR ApplicationName, PCWSTR CommandLine)
{
    PCWSTR Start, End, Cur;

    if (ApplicationName && *ApplicationName)
    {
        Start = ApplicationName;
        for (End = Start; *End; ++End)
            ;
    }
    else if (CommandLine && *CommandLine)
    {
        Start = CommandLine;
        if (*Start == L'"')
        {
            ++Start;
            for (End = Start; *End && *End != L'"'; ++End)
                ;
        }
        else
        {
            for (End = Start; *End && *End != L' ' && *End != L'\t'; ++End)
                ;
        }
    }
    else
    {
        return FALSE;
    }

    for (Cur = Start; Cur < End; ++Cur)
    {
        if (*Cur == L'\\' || *Cur == L'/')
            Start = Cur + 1;
    }
    return (End - Start) == 10 && SandboxFind(Start, 10, L"CHROME.EXE");
}

BOOL WINAPI SHIM_OBJ_NAME(APIHook_CreateProcessW)(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles,
    DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation)
{
    if (lpCommandLine && SandboxIsChrome(lpApplicationName, lpCommandLine))
    {
        SIZE_T Len = 0, Extra = sizeof(NO_SANDBOX_SWITCH) / sizeof(WCHAR);
        PWSTR NewLine;

        while (lpCommandLine[Len])
            ++Len;

        if (!SandboxFind(lpCommandLine, Len, L"--no-sandbox"))
        {
            NewLine = ShimLib_ShimMalloc((Len + Extra) * sizeof(WCHAR));
            if (NewLine)
            {
                BOOL Result;
                SIZE_T n;

                for (n = 0; n < Len; ++n)
                    NewLine[n] = lpCommandLine[n];
                for (n = 0; n < Extra; ++n)
                    NewLine[Len + n] = NO_SANDBOX_SWITCH[n];

                SHIM_INFO("Adding --no-sandbox\n");
                Result = CALL_SHIM(0, CREATEPROCESSWPROC)(lpApplicationName, NewLine, lpProcessAttributes,
                    lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory,
                    lpStartupInfo, lpProcessInformation);
                ShimLib_ShimFree(NewLine);
                return Result;
            }
        }
    }

    return CALL_SHIM(0, CREATEPROCESSWPROC)(lpApplicationName, lpCommandLine, lpProcessAttributes,
        lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory,
        lpStartupInfo, lpProcessInformation);
}

#define SHIM_NUM_HOOKS  1
#define SHIM_SETUP_HOOKS \
    SHIM_HOOK(0, "KERNEL32.DLL", "CreateProcessW", SHIM_OBJ_NAME(APIHook_CreateProcessW))

#include <implement_shim.inl>
