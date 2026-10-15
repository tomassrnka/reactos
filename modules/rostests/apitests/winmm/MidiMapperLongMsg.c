/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     The MIDI mapper refuses a long message whose header is not prepared
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <mmsystem.h>

/* Windows answers MIDIERR_UNPREPARED and leaves the header alone. A mapper that forwards the data instead
   hands whatever the buffer holds to the device, and the winmm winetest passes it 70000 bytes of unwritten
   heap: with a driver that serializes its notes this runs for minutes. */

static const BYTE GmReset[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };

START_TEST(MidiMapperLongMsg)
{
    HMIDIOUT Midi;
    MIDIHDR Header;
    MMRESULT Result;
    ULONG Tries;

    if (midiOutGetNumDevs() == 0)
    {
        skip("No MIDI output device\n");
        return;
    }

    Result = midiOutOpen(&Midi, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
    if (Result != MMSYSERR_NOERROR)
    {
        skip("The MIDI mapper does not open: %u\n", Result);
        return;
    }

    /* A control change (all notes off, silent) makes the mapper open its output port */
    Result = midiOutShortMsg(Midi, 0x00007BB0);
    ok(Result == MMSYSERR_NOERROR, "midiOutShortMsg returned %u\n", Result);

    memset(&Header, 0, sizeof(Header));
    Header.lpData = (LPSTR)GmReset;
    Header.dwBufferLength = sizeof(GmReset);
    Result = midiOutLongMsg(Midi, &Header, sizeof(Header));
    ok(Result == MIDIERR_UNPREPARED, "midiOutLongMsg of an unprepared header returned %u\n", Result);
    ok(Header.dwFlags == 0, "dwFlags is 0x%lx after the refused message\n", Header.dwFlags);

    Result = midiOutPrepareHeader(Midi, &Header, sizeof(Header));
    ok(Result == MMSYSERR_NOERROR, "midiOutPrepareHeader returned %u\n", Result);
    if (Result == MMSYSERR_NOERROR)
    {
        Result = midiOutLongMsg(Midi, &Header, sizeof(Header));
        ok(Result == MMSYSERR_NOERROR, "midiOutLongMsg of a prepared header returned %u\n", Result);

        /* A driver may still hold the header; give it time before the header is released */
        for (Tries = 0; Tries < 100 && (Header.dwFlags & MHDR_INQUEUE); Tries++)
            Sleep(20);

        Result = midiOutUnprepareHeader(Midi, &Header, sizeof(Header));
        ok(Result == MMSYSERR_NOERROR, "midiOutUnprepareHeader returned %u\n", Result);
    }

    Result = midiOutClose(Midi);
    ok(Result == MMSYSERR_NOERROR, "midiOutClose returned %u\n", Result);
}
