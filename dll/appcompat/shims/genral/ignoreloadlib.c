/*
 * PROJECT:     ReactOS 'General' Shim library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     IgnoreLoadLibrary shim (the Windows 8 and later version, command line driven)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <shimlib.h>

/*
 * The command line is a ';' separated list of 'name' or 'name:value'. A LoadLibrary call for a module with that
 * exact file name (the directory is not looked at, and no extension is added) does not load anything and returns
 * the (decimal) value, 0 when there is none. All other modules are loaded as usual, with the error dialogs off.
 */
typedef HMODULE(WINAPI* LOADLIBRARYAPROC)(LPCSTR lpLibFileName);
typedef HMODULE(WINAPI* LOADLIBRARYEXAPROC)(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
typedef HMODULE(WINAPI* LOADLIBRARYWPROC)(LPCWSTR lpLibFileName);
typedef HMODULE(WINAPI* LOADLIBRARYEXWPROC)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

#define SHIM_NS         IgnoreLoadLibrary
#include <setup_shim.inl>

#define IGNORE_ERRORMODE (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)

static WCHAR IgnoreToUpper(WCHAR Ch)
{
    return (Ch >= L'a' && Ch <= L'z') ? (WCHAR)(Ch - (L'a' - L'A')) : Ch;
}

/* Compare the file name part of a (possibly counted, possibly wide) module name with a name from the command line */
static BOOL IgnoreMatch(PCSTR Name, PCSTR NameEnd, PCSTR Entry, SIZE_T EntryLen)
{
    PCSTR Base = Name, Cur;
    SIZE_T n;

    for (Cur = Name; Cur < NameEnd; ++Cur)
    {
        if (*Cur == '\\' || *Cur == '/')
            Base = Cur + 1;
    }
    if ((SIZE_T)(NameEnd - Base) != EntryLen)
        return FALSE;
    for (n = 0; n < EntryLen; ++n)
    {
        if (IgnoreToUpper((UCHAR)Base[n]) != IgnoreToUpper((UCHAR)Entry[n]))
            return FALSE;
    }
    return TRUE;
}

/* Look the module up in the command line, return TRUE when it is one that has to be ignored */
static BOOL IgnoreLookup(PCSTR Name, PCSTR NameEnd, HMODULE* Result)
{
    PCSTR Cur = SHIM_OBJ_NAME(g_szCommandLine);

    if (!Cur || !Name)
        return FALSE;

    while (*Cur)
    {
        PCSTR EntryEnd = Cur, Colon = NULL;
        SIZE_T NameLen;

        while (*EntryEnd && *EntryEnd != ';')
        {
            if (*EntryEnd == ':' && !Colon)
                Colon = EntryEnd;
            ++EntryEnd;
        }
        NameLen = (Colon ? Colon : EntryEnd) - Cur;
        if (NameLen && IgnoreMatch(Name, NameEnd, Cur, NameLen))
        {
            ULONG_PTR Value = 0;
            if (Colon)
            {
                PCSTR Digit;
                for (Digit = Colon + 1; Digit < EntryEnd && *Digit >= '0' && *Digit <= '9'; ++Digit)
                    Value = Value * 10 + (*Digit - '0');
            }
            *Result = (HMODULE)Value;
            return TRUE;
        }
        Cur = EntryEnd;
        if (*Cur == ';')
            ++Cur;
    }
    return FALSE;
}

/* The wide names are looked at as the ANSI names the command line holds: only the characters that fit are compared */
static BOOL IgnoreLookupW(PCWSTR Name, HMODULE* Result)
{
    char Buffer[MAX_PATH];
    SIZE_T Len;

    if (!Name)
        return FALSE;
    for (Len = 0; Name[Len]; ++Len)
    {
        if (Len + 1 >= sizeof(Buffer) || Name[Len] > 0x7f)
            return FALSE;
        Buffer[Len] = (char)Name[Len];
    }
    Buffer[Len] = '\0';
    return IgnoreLookup(Buffer, Buffer + Len, Result);
}

static BOOL IgnoreLookupA(PCSTR Name, HMODULE* Result)
{
    SIZE_T Len;

    if (!Name)
        return FALSE;
    for (Len = 0; Name[Len]; ++Len)
        ;
    return IgnoreLookup(Name, Name + Len, Result);
}

HMODULE WINAPI SHIM_OBJ_NAME(APIHook_LoadLibraryA)(LPCSTR lpLibFileName)
{
    HMODULE Module;
    DWORD dwOldErrorMode;

    if (IgnoreLookupA(lpLibFileName, &Module))
        return Module;

    dwOldErrorMode = SetErrorMode(IGNORE_ERRORMODE);
    Module = CALL_SHIM(0, LOADLIBRARYAPROC)(lpLibFileName);
    SetErrorMode(dwOldErrorMode);

    return Module;
}

HMODULE WINAPI SHIM_OBJ_NAME(APIHook_LoadLibraryExA)(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    HMODULE Module;
    DWORD dwOldErrorMode;

    if (IgnoreLookupA(lpLibFileName, &Module))
        return Module;

    dwOldErrorMode = SetErrorMode(IGNORE_ERRORMODE);
    Module = CALL_SHIM(1, LOADLIBRARYEXAPROC)(lpLibFileName, hFile, dwFlags);
    SetErrorMode(dwOldErrorMode);

    return Module;
}

HMODULE WINAPI SHIM_OBJ_NAME(APIHook_LoadLibraryW)(LPCWSTR lpLibFileName)
{
    HMODULE Module;
    DWORD dwOldErrorMode;

    if (IgnoreLookupW(lpLibFileName, &Module))
        return Module;

    dwOldErrorMode = SetErrorMode(IGNORE_ERRORMODE);
    Module = CALL_SHIM(2, LOADLIBRARYWPROC)(lpLibFileName);
    SetErrorMode(dwOldErrorMode);

    return Module;
}

HMODULE WINAPI SHIM_OBJ_NAME(APIHook_LoadLibraryExW)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    HMODULE Module;
    DWORD dwOldErrorMode;

    if (IgnoreLookupW(lpLibFileName, &Module))
        return Module;

    dwOldErrorMode = SetErrorMode(IGNORE_ERRORMODE);
    Module = CALL_SHIM(3, LOADLIBRARYEXWPROC)(lpLibFileName, hFile, dwFlags);
    SetErrorMode(dwOldErrorMode);

    return Module;
}


#define SHIM_NUM_HOOKS  4
#define SHIM_SETUP_HOOKS \
    SHIM_HOOK(0, "KERNEL32.DLL", "LoadLibraryA", SHIM_OBJ_NAME(APIHook_LoadLibraryA)) \
    SHIM_HOOK(1, "KERNEL32.DLL", "LoadLibraryExA", SHIM_OBJ_NAME(APIHook_LoadLibraryExA)) \
    SHIM_HOOK(2, "KERNEL32.DLL", "LoadLibraryW", SHIM_OBJ_NAME(APIHook_LoadLibraryW)) \
    SHIM_HOOK(3, "KERNEL32.DLL", "LoadLibraryExW", SHIM_OBJ_NAME(APIHook_LoadLibraryExW))

#include <implement_shim.inl>
