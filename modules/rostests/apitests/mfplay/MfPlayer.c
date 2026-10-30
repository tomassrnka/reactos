/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for mfplay!MFPCreateMediaPlayer
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64). LibreOffice (mergedlo.dll) links against mfplay.dll,
 * which ReactOS did not have, so it could not start.
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#define COBJMACROS
#include <ole2.h>
#include <mfapi.h>
#include <mfplay.h>

typedef HRESULT (WINAPI *PFN_MFPCreateMediaPlayer)(PCWSTR, BOOL, MFP_CREATION_OPTIONS,
                                                   IMFPMediaPlayerCallback *, HWND, IMFPMediaPlayer **);

START_TEST(MfPlayer)
{
    HMODULE Mfplay = LoadLibraryW(L"mfplay.dll");
    PFN_MFPCreateMediaPlayer pCreate;
    IMFPMediaPlayer *Player;
    MFP_MEDIAPLAYER_STATE State;
    HRESULT hr;

    ok(Mfplay != NULL, "mfplay.dll does not load, error %lu\n", GetLastError());
    if (!Mfplay)
        return;

    pCreate = (PFN_MFPCreateMediaPlayer)GetProcAddress(Mfplay, "MFPCreateMediaPlayer");
    ok(pCreate != NULL, "MFPCreateMediaPlayer is not exported\n");
    ok(GetProcAddress(Mfplay, "MFPCreateMediaPlayerEx") != NULL, "MFPCreateMediaPlayerEx is not exported\n");
    ok(GetProcAddress(Mfplay, "DllCanUnloadNow") != NULL, "DllCanUnloadNow is not exported\n");
    ok(GetProcAddress(Mfplay, "DllGetClassObject") != NULL, "DllGetClassObject is not exported\n");
    ok(GetProcAddress(Mfplay, "DllRegisterServer") != NULL, "DllRegisterServer is not exported\n");
    ok(GetProcAddress(Mfplay, "DllUnregisterServer") != NULL, "DllUnregisterServer is not exported\n");
    if (!pCreate)
        return;

    /* A player without a media item starts empty */
    Player = (IMFPMediaPlayer *)(ULONG_PTR)1;
    hr = pCreate(NULL, FALSE, 0, NULL, NULL, &Player);
    ok(hr == S_OK, "MFPCreateMediaPlayer returned %08lx\n", hr);
    if (hr == S_OK)
    {
        State = (MFP_MEDIAPLAYER_STATE)99;
        hr = IMFPMediaPlayer_GetState(Player, &State);
        ok(hr == S_OK, "GetState returned %08lx\n", hr);
        ok(State == MFP_MEDIAPLAYER_STATE_EMPTY, "State is %d\n", State);
        IMFPMediaPlayer_Release(Player);
    }

    hr = pCreate(NULL, FALSE, 0, NULL, NULL, NULL);
    ok(hr == E_POINTER, "MFPCreateMediaPlayer without an answer returned %08lx\n", hr);

    /* Starting playback needs something to play */
    Player = (IMFPMediaPlayer *)(ULONG_PTR)1;
    hr = pCreate(NULL, TRUE, 0, NULL, NULL, &Player);
    ok(hr == E_INVALIDARG, "MFPCreateMediaPlayer(start, no URL) returned %08lx\n", hr);

    Player = (IMFPMediaPlayer *)(ULONG_PTR)1;
    hr = pCreate(L"nonexistent.mp4", FALSE, 0, NULL, NULL, &Player);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "MFPCreateMediaPlayer(missing file) returned %08lx\n", hr);
    ok(Player == NULL, "Player is %p\n", Player);
}
