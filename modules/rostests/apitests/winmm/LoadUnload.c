/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     winmm can be loaded and unloaded repeatedly, with and without having used it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <mmsystem.h>

/* This test does not import winmm (the test program loads it with a delay), so FreeLibrary can unload it and run
   its DllMain detach. The detach must neither run driver code nor hang, with or without drivers loaded. */

typedef UINT (WINAPI *PFN_NUMDEVS)(void);
typedef DWORD (WINAPI *PFN_TIME)(void);

static UINT UseWinmm(HMODULE Module, UINT *Counts)
{
    PFN_NUMDEVS WaveOut = (PFN_NUMDEVS)GetProcAddress(Module, "waveOutGetNumDevs");
    PFN_NUMDEVS MidiOut = (PFN_NUMDEVS)GetProcAddress(Module, "midiOutGetNumDevs");
    PFN_NUMDEVS Mixer = (PFN_NUMDEVS)GetProcAddress(Module, "mixerGetNumDevs");
    PFN_NUMDEVS Aux = (PFN_NUMDEVS)GetProcAddress(Module, "auxGetNumDevs");

    if (!WaveOut || !MidiOut || !Mixer || !Aux)
        return FALSE;
    Counts[0] = WaveOut();
    Counts[1] = MidiOut();
    Counts[2] = Mixer();
    Counts[3] = Aux();
    return TRUE;
}

START_TEST(LoadUnload)
{
    HMODULE Module;
    PFN_TIME TimeGetTime;
    UINT Counts[4], First[4] = { 0 }, Round;
    ULONG Unloaded = 0;
    MMRESULT Result;
    HWAVEOUT WaveOut;
    HMIDIOUT MidiOut;
    HMIXER Mixer;
    WAVEOUTCAPSA WaveCaps;
    WAVEFORMATEX Format = { WAVE_FORMAT_PCM, 1, 8000, 8000, 1, 8, 0 };

    ok(GetModuleHandleA("winmm.dll") == NULL, "winmm is loaded before the test loads it\n");

    /* Never used: the module must go away at the last FreeLibrary, and no driver may have been loaded */
    for (Round = 0; Round < 5; Round++)
    {
        Module = LoadLibraryA("winmm.dll");
        ok(Module != NULL, "LoadLibrary failed: %lu\n", GetLastError());
        if (!Module)
            return;
        TimeGetTime = (PFN_TIME)GetProcAddress(Module, "timeGetTime");
        ok(TimeGetTime != NULL, "no timeGetTime\n");
        if (TimeGetTime)
            TimeGetTime();
        ok(GetModuleHandleA("beepmidi.dll") == NULL, "a driver is loaded by timeGetTime\n");
        ok(FreeLibrary(Module), "FreeLibrary failed: %lu\n", GetLastError());
        ok(GetModuleHandleA("winmm.dll") == NULL, "winmm stays loaded after FreeLibrary (round %u)\n", Round);
    }

    /* Used: the drivers are loaded. Whether the module leaves memory afterwards depends on the drivers; the
       device counts must stay the same and nothing may crash or hang. */
    for (Round = 0; Round < 10; Round++)
    {
        Module = LoadLibraryA("winmm.dll");
        ok(Module != NULL, "LoadLibrary failed: %lu\n", GetLastError());
        if (!Module)
            return;
        ok(UseWinmm(Module, Counts), "winmm lacks an entry point\n");
        if (Round == 0)
            memcpy(First, Counts, sizeof(First));
        else
            ok(!memcmp(First, Counts, sizeof(First)), "device counts changed in round %u\n", Round);
        ok(FreeLibrary(Module), "FreeLibrary failed: %lu\n", GetLastError());
        if (GetModuleHandleA("winmm.dll") == NULL)
            Unloaded++;
    }
    trace("wave %u, midi %u, mixer %u, aux %u devices; winmm left memory %lu times in 10 rounds\n",
          First[0], First[1], First[2], First[3], Unloaded);

    /* An unknown device number is a bad device id, whatever drivers loaded (auxGetDevCaps is left out: ReactOS answers
       MMSYSERR_INVALHANDLE where Windows answers MMSYSERR_BADDEVICEID) */
    Module = LoadLibraryA("winmm.dll");
    ok(Module != NULL, "LoadLibrary failed: %lu\n", GetLastError());
    if (!Module)
        return;
    Result = waveOutGetDevCapsA(1000, &WaveCaps, sizeof(WaveCaps));
    ok(Result == MMSYSERR_BADDEVICEID, "waveOutGetDevCaps(1000) returned %u\n", Result);
    Result = waveOutOpen(&WaveOut, 1000, &Format, 0, 0, CALLBACK_NULL);
    ok(Result == MMSYSERR_BADDEVICEID, "waveOutOpen(1000) returned %u\n", Result);
    Result = midiOutOpen(&MidiOut, 1000, 0, 0, CALLBACK_NULL);
    ok(Result == MMSYSERR_BADDEVICEID, "midiOutOpen(1000) returned %u\n", Result);
    Result = mixerOpen(&Mixer, 1000, 0, 0, 0);
    ok(Result == MMSYSERR_BADDEVICEID, "mixerOpen(1000) returned %u\n", Result);
    FreeLibrary(Module);
}
