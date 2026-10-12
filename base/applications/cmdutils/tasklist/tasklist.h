/*
 * PROJECT:     ReactOS Tasklist Command
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Displays a list of currently running processes on the computer.
 * COPYRIGHT:   Copyright 2020 He Yang <1160386205@qq.com>
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define WIN32_NO_STATUS
#include <windows.h>
#include <tlhelp32.h>
#include <wtsapi32.h>
#include <lmcons.h>
#include <ndk/exfuncs.h>
#include <strsafe.h>

#include <conutils.h>

#include "resource.h"

#define NT_SYSTEM_QUERY_MAX_RETRY 5

#ifndef TH32CS_SNAPMODULE32
#define TH32CS_SNAPMODULE32 0x10
#endif

/* the max string length of a resource string */
#define RES_STR_MAXLEN 64

#define COLUMNWIDTH_IMAGENAME   25
#define COLUMNWIDTH_PID         8
#define COLUMNWIDTH_SESSIONNAME 16
#define COLUMNWIDTH_SESSION     11
#define COLUMNWIDTH_MEMUSAGE    12
#define COLUMNWIDTH_STATUS      15
#define COLUMNWIDTH_USERNAME    50
#define COLUMNWIDTH_CPUTIME     12
#define COLUMNWIDTH_WINDOWTITLE 72
#define COLUMNWIDTH_LIST        44
#define COLUMNWIDTH_LISTLABEL   14
