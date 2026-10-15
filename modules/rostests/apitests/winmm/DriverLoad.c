/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     winmm loads its multimedia drivers on first use, not when it is loaded
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <mmsystem.h>
#include <winreg.h>

/* The test program loads winmm with a delay, and the test loads it first, so winmm's DllMain has run before the
   checks. Windows has not loaded any of the registered wave, MIDI, mixer or auxiliary drivers at that point: a
   driver DLL loaded from DllMain would be loaded under the loader lock, which can deadlock a process whose other
   threads need that lock. */

static ULONG CheckKey(PCSTR KeyName)
{
    CHAR Name[256], Data[MAX_PATH];
    DWORD NameLength, DataLength, Type, Index;
    ULONG Count = 0;
    HKEY Key;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, KeyName, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return 0;

    for (Index = 0; ; Index++)
    {
        NameLength = sizeof(Name);
        DataLength = sizeof(Data) - 1;
        if (RegEnumValueA(Key, Index, Name, &NameLength, NULL, &Type, (LPBYTE)Data, &DataLength) != ERROR_SUCCESS)
            break;
        if (Type != REG_SZ || DataLength == 0)
            continue;
        Data[DataLength] = ANSI_NULL;
        if (_strnicmp(Name, "wave", 4) && _strnicmp(Name, "midi", 4) &&
            _strnicmp(Name, "mixer", 5) && _strnicmp(Name, "aux", 3))
        {
            continue;
        }

        Count++;
        ok(GetModuleHandleA(Data) == NULL, "%s (%s) is loaded before the first use of winmm\n", Data, Name);
    }

    RegCloseKey(Key);
    return Count;
}

START_TEST(DriverLoad)
{
    ULONG Count;

    ok(LoadLibraryA("winmm.dll") != NULL, "LoadLibrary failed: %lu\n", GetLastError());

    Count = CheckKey("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32");
    Count += CheckKey("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers");
    if (Count == 0)
        skip("No multimedia drivers are registered\n");
    trace("%lu registered drivers checked\n", Count);

    /* The first use loads them; it must still work */
    trace("waveOutGetNumDevs %u, midiOutGetNumDevs %u, mixerGetNumDevs %u\n",
          waveOutGetNumDevs(), midiOutGetNumDevs(), mixerGetNumDevs());
}
