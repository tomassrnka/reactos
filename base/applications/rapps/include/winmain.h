#pragma once

#include "settings.h"

extern LPCWSTR szWindowClass;

extern HWND hMainWnd;
extern HINSTANCE hInst;

// TRUE when RAPPS is driven as a silent/headless CLI operation (install/setup/download
// with /S, e.g. launched by featmgr). No modal dialog may be shown in this mode: a blocked
// MessageBox would hang the parent that waits on the process. Set in ParseCmdAndExecute
// before any database work so the auto DB-update path can suppress its UI too.
extern BOOL g_bSilentMode;

// integrity.cpp
BOOL VerifyInteg(LPCWSTR lpSHA1Hash, LPCWSTR lpFileName);
BOOL VerifyInteg256(LPCWSTR lpSHA256Hash, LPCWSTR lpFileName);
