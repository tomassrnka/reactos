/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     What user32_apitest_dpi prints for each scenario on Windows 10 22H2 (x64 and WOW64 x86 print the same)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

typedef struct _DPI_MANIFEST
{
    const char *Name;
    const char *DpiAware;
    const char *DpiAwareness;
} DPI_MANIFEST;

static const DPI_MANIFEST g_DpiManifests[] =
{
    { "aware-true", "true", NULL },
    { "aware-false", "false", NULL },
    { "aware-pm", "true/pm", NULL },
    { "aware-permonitor", "per monitor", NULL },
    { "aware-TRUE", "True", NULL },
    { "aware-junk", "maybe", NULL },
    { "awn-system", NULL, "system" },
    { "awn-permonitor", NULL, "permonitor" },
    { "awn-pmv2", NULL, "permonitorv2" },
    { "awn-unaware", NULL, "unaware" },
    { "awn-list", NULL, "PerMonitorV2, PerMonitor" },
    { "awn-badfirst", NULL, "foo, system" },
    { "awn-junk", NULL, "foo" },
    { "both", "true", "permonitorv2" },
    { "both2", "true/pm", "unaware" },
    { "none", NULL, NULL },
    { "awn-len63", NULL, "xx,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,system" },
    { "awn-len64", NULL, "xxx,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,system" },
    { "awn-len65", NULL, "xx,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,x,system" },
    { "awn-len242", NULL, "y0,y1,y2,y3,y4,y5,y6,y7,y8,y9,y10,y11,y12,y13,y14,y15,y16,y17,y18,y19,y20,y21,y22,y23,y24,y25,y26,y27,y28,y29,y30,y31,y32,y33,y34,y35,y36,y37,y38,y39,y40,y41,y42,y43,y44,y45,y46,y47,y48,y49,y50,y51,y52,y53,y54,y55,y56,y57,y58,y59,permonitorv2" },
};

typedef struct _DPI_SCENARIO
{
    const char *Name;
    const char *Args;
    const char *Manifest;
    const char *Expected;
} DPI_SCENARIO;

static const DPI_SCENARIO g_DpiScenarios[] =
{
    {
        "default",
        "get funcs",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# funcs\n"
        "ctx U (pseudo-1): valid=1/- aw=0/- dpi=96/-\n"
        "ctx S (pseudo-2): valid=1/- aw=1/- dpi=96/-\n"
        "ctx P (pseudo-3): valid=1/- aw=2/- dpi=0/-\n"
        "ctx P2 (pseudo-4): valid=1/- aw=2/- dpi=0/-\n"
        "ctx G (pseudo-5): valid=1/- aw=0/- dpi=96/-\n"
        "ctx N (a0.v0.d0.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:1 (a1.v0.d0.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:6010 (a0.v1.d96.f0): valid=1/- aw=0/- dpi=96/-\n"
        "ctx sys:0 (a1.v1.SYS.f0): valid=1/- aw=1/- dpi=96/-\n"
        "ctx raw:12 (a2.v1.d0.f0): valid=1/- aw=2/- dpi=0/-\n"
        "ctx raw:22 (a2.v2.d0.f0): valid=1/- aw=2/- dpi=0/-\n"
        "ctx raw:40006010 (a0.v1.d96.f40000000): valid=1/- aw=0/- dpi=96/-\n"
        "ctx raw:80006010 (a0.v1.d96.f80000000): valid=1/- aw=0/- dpi=96/-\n"
        "ctx raw:80000012 (a2.v1.d0.f80000000): valid=1/- aw=2/- dpi=0/-\n"
        "ctx raw:6011 (a1.v1.SYS.f0): valid=1/- aw=1/- dpi=96/-\n"
        "ctx raw:10 (a0.v1.d0.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:7811 (a1.v1.d120.f0): valid=1/- aw=1/- dpi=120/-\n"
        "ctx raw:32 (a2.v3.d0.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:20 (a0.v2.d0.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:40000012 (a2.v1.d0.f40000000): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:2 (a2.v0.d0.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:1010 (a0.v1.d16.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:30 (a0.v3.d0.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx dpi:95 (a1.v1.d95.f0): valid=1/- aw=1/- dpi=95/-\n"
        "ctx dpi:100 (a1.v1.d100.f0): valid=1/- aw=1/- dpi=100/-\n"
        "ctx dpi:192 (a1.v1.d192.f0): valid=1/- aw=1/- dpi=192/-\n"
        "ctx dpi:255 (a1.v1.d255.f0): valid=1/- aw=1/- dpi=255/-\n"
        "ctx dpi:480 (a1.v1.d480.f0): valid=1/- aw=1/- dpi=480/-\n"
        "ctx dpi:511 (a1.v1.d511.f0): valid=1/- aw=1/- dpi=511/-\n"
        "ctx dpi:0 (a1.v1.d0.f0): valid=1/- aw=1/- dpi=0/-\n"
        "ctx dpi:1 (a1.v1.d1.f0): valid=1/- aw=1/- dpi=1/-\n"
        "ctx raw:20011 (a1.v1.d0.f20000): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:100012 (a2.v1.d0.f100000): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:2006010 (a0.v1.d96.f2000000): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:40000022 (a2.v2.d0.f40000000): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:80000022 (a2.v2.d0.f80000000): valid=1/- aw=2/- dpi=0/-\n"
        "ctx raw:c0006010 (a0.v1.d96.fc0000000): valid=1/- aw=0/- dpi=96/-\n"
        "ctx raw:c0000012 (a2.v1.d0.fc0000000): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:6012 (a2.v1.d96.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:6020 (a0.v2.d96.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:7010 (a0.v1.d112.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:6030 (a0.v3.d96.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:5010 (a0.v1.d80.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx raw:6110 (a0.v1.d97.f0): valid=0/87 aw=-1/87 dpi=0/87\n"
        "ctx sx:80006010 (a0.v1.d96.f80000000): valid=1/- aw=0/- dpi=96/-\n"
        "ctx sx:80000012 (a2.v1.d0.f80000000): valid=1/- aw=2/- dpi=0/-\n"
        "ctx sx:c0006010 (a0.v1.d96.fc0000000): valid=1/- aw=0/- dpi=96/-\n"
        "ctx sx:6010 (a0.v1.d96.f0): valid=1/- aw=0/- dpi=96/-\n"
        "equal U: 1000001100000\n"
        "equal S: 0100010000000\n"
        "equal P: 0010000011000\n"
        "equal P2: 0001000000000\n"
        "equal G: 0000100000110\n"
        "equal sys:0: 0100010000000\n"
        "equal raw:6010: 1000001100000\n"
        "equal raw:80006010: 1000001100000\n"
        "equal raw:12: 0010000011000\n"
        "equal raw:80000012: 0010000011000\n"
        "equal raw:40006010: 0000100000110\n"
        "equal raw:c0006010: 0000100000110\n"
        "equal raw:7811: 0000000000001\n"
        "equal U invalid=0/87\n"
        "equal invalid U=0/87\n"
        "equal invalid invalid=0/87\n"
        "equal NULL NULL=0/87\n"
    },
    {
        "threadchain",
        "get sett:P get sett:S get sett:N get sett:raw:80006010 get sett:P2 get sett:G get sett:U get sett:raw:7 get sett:raw:12 get sett:sys:0 get sett:sys:80000000 get sett:U get sett:Cur get sett:raw:80000012 get sett:raw:40006010 get sett:raw:22 get sett:dpi:120 get sett:raw:6010 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:P\n"
        "SetThreadDpiAwarenessContext(P) gle=- prev=a0.v1.d96.f80000000\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# sett:S\n"
        "SetThreadDpiAwarenessContext(S) gle=- prev=a2.v1.d0.f0\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# sett:N\n"
        "SetThreadDpiAwarenessContext(N) gle=87 prev=a0.v0.d0.f0\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# sett:raw:80006010\n"
        "SetThreadDpiAwarenessContext(raw:80006010) gle=- prev=a1.v1.SYS.f0\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:P2\n"
        "SetThreadDpiAwarenessContext(P2) gle=- prev=a0.v1.d96.f80000000\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# sett:G\n"
        "SetThreadDpiAwarenessContext(G) gle=- prev=a2.v2.d0.f0\n"
        "# get\n"
        "get: thread=a0.v1.d96.f40000000 proc=a0.v1.d96.f40000000/a0.v1.d96.f40000000 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:U\n"
        "SetThreadDpiAwarenessContext(U) gle=- prev=a0.v1.d96.f40000000\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:raw:7\n"
        "SetThreadDpiAwarenessContext(raw:7) gle=87 prev=a0.v0.d0.f0\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:raw:12\n"
        "SetThreadDpiAwarenessContext(raw:12) gle=- prev=a0.v1.d96.f0\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# sett:sys:0\n"
        "SetThreadDpiAwarenessContext(sys:0) gle=- prev=a2.v1.d0.f0\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# sett:sys:80000000\n"
        "SetThreadDpiAwarenessContext(sys:80000000) gle=- prev=a1.v1.SYS.f0\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:U\n"
        "SetThreadDpiAwarenessContext(U) gle=- prev=a0.v1.d96.f80000000\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:Cur\n"
        "SetThreadDpiAwarenessContext(Cur) gle=- prev=a0.v1.d96.f0\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:raw:80000012\n"
        "SetThreadDpiAwarenessContext(raw:80000012) gle=- prev=a0.v1.d96.f0\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:raw:40006010\n"
        "SetThreadDpiAwarenessContext(raw:40006010) gle=- prev=a0.v1.d96.f80000000\n"
        "# get\n"
        "get: thread=a0.v1.d96.f40000000 proc=a0.v1.d96.f40000000/a0.v1.d96.f40000000 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:raw:22\n"
        "SetThreadDpiAwarenessContext(raw:22) gle=- prev=a0.v1.d96.f40000000\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# sett:dpi:120\n"
        "SetThreadDpiAwarenessContext(dpi:120) gle=87 prev=a0.v0.d0.f0\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# sett:raw:6010\n"
        "SetThreadDpiAwarenessContext(raw:6010) gle=- prev=a2.v2.d0.f0\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "win-default",
        "win",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# win\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "win-pmv2",
        "win:P2",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# win:P2\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "windows under P2\n"
        "overlapped: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "first window again: ctx=a0.v1.d96.f0, thread ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "win-pm",
        "win:P",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# win:P\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "windows under P\n"
        "overlapped: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "first window again: ctx=a0.v1.d96.f0, thread ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "win-system",
        "win:S",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# win:S\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "windows under S\n"
        "overlapped: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "first window again: ctx=a0.v1.d96.f0, thread ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "win-gdiscaled",
        "win:G",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# win:G\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "windows under G\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f40000000, later ctx=a0.v1.d96.f40000000 dpi=SYS enable=0/-\n"
        "first window again: ctx=a0.v1.d96.f0, thread ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "win-unaware",
        "win:U",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# win:U\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "windows under U\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "first window again: ctx=a0.v1.d96.f0, thread ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "set-unaware",
        "setp:U get setp:U setp:S get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:U\n"
        "SetProcessDpiAwarenessContext(U)=1 gle=-\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:U\n"
        "SetProcessDpiAwarenessContext(U)=0 gle=5\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "set-system",
        "setp:S get setp:P2 get sett:P get sett:U get sett:N get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=1 gle=-\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:P2\n"
        "SetProcessDpiAwarenessContext(P2)=0 gle=5\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# sett:P\n"
        "SetThreadDpiAwarenessContext(P) gle=- prev=a1.v1.SYS.f80000000\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# sett:U\n"
        "SetThreadDpiAwarenessContext(U) gle=- prev=a2.v1.d0.f0\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:N\n"
        "SetThreadDpiAwarenessContext(N) gle=87 prev=a0.v0.d0.f0\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "set-pm-win",
        "setp:P get win:P2 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:P\n"
        "SetProcessDpiAwarenessContext(P)=1 gle=-\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# win:P2\n"
        "windows under a2.v1.d0.f0\n"
        "overlapped: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a2.v1.d0.f0\n"
        "windows under P2\n"
        "overlapped: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "first window again: ctx=a2.v1.d0.f0, thread ctx=a2.v1.d0.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a2.v1.d0.f0\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
    },
    {
        "set-pmv2-win",
        "setp:P2 get win:U",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:P2\n"
        "SetProcessDpiAwarenessContext(P2)=1 gle=-\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# win:U\n"
        "windows under a2.v2.d0.f0\n"
        "overlapped: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a2.v2.d0.f0\n"
        "windows under U\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "first window again: ctx=a2.v2.d0.f0, thread ctx=a2.v2.d0.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a2.v2.d0.f0\n"
    },
    {
        "set-gdiscaled",
        "setp:G get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:G\n"
        "SetProcessDpiAwarenessContext(G)=1 gle=-\n"
        "# get\n"
        "get: thread=a0.v1.d96.f40000000 proc=a0.v1.d96.f40000000/a0.v1.d96.f40000000 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "set-raw-unaware",
        "setp:raw:6010 get setp:S",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:raw:6010\n"
        "SetProcessDpiAwarenessContext(raw:6010)=1 gle=-\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
    },
    {
        "set-raw-system",
        "setp:sys:0 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:sys:0\n"
        "SetProcessDpiAwarenessContext(sys:0)=1 gle=-\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
    },
    {
        "set-raw-processflag",
        "setp:raw:80006010 get setp:S",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:raw:80006010\n"
        "SetProcessDpiAwarenessContext(raw:80006010)=1 gle=-\n"
        "# get\n"
        "get: thread=a0.v1.d96.f80000000 proc=a0.v1.d96.f80000000/a0.v1.d96.f80000000 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
    },
    {
        "set-null",
        "setp:N get setp:S get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:N\n"
        "SetProcessDpiAwarenessContext(N)=0 gle=87\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=1 gle=-\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
    },
    {
        "set-invalid",
        "setp:raw:7 get setp:S get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:raw:7\n"
        "SetProcessDpiAwarenessContext(raw:7)=0 gle=87\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=1 gle=-\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
    },
    {
        "set-pm-then-pmv2",
        "setp:raw:12 get setp:raw:22 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:raw:12\n"
        "SetProcessDpiAwarenessContext(raw:12)=1 gle=-\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# setp:raw:22\n"
        "SetProcessDpiAwarenessContext(raw:22)=0 gle=5\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
    },
    {
        "set-wrong-dpi",
        "setp:dpi:120 get setp:dpi:96 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:dpi:120\n"
        "SetProcessDpiAwarenessContext(dpi:120)=0 gle=5\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:dpi:96\n"
        "SetProcessDpiAwarenessContext(dpi:96)=1 gle=-\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
    },
    {
        "legacy-aware",
        "setaware get setp:S get setaware setp:P2 setaware",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setaware\n"
        "SetProcessDPIAware=1 gle=-\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setaware\n"
        "SetProcessDPIAware=1 gle=-\n"
        "# setp:P2\n"
        "SetProcessDpiAwarenessContext(P2)=0 gle=5\n"
        "# setaware\n"
        "SetProcessDPIAware=1 gle=-\n"
    },
    {
        "legacy-after-pm",
        "setp:P setaware get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:P\n"
        "SetProcessDpiAwarenessContext(P)=1 gle=-\n"
        "# setaware\n"
        "SetProcessDPIAware=1 gle=-\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
    },
    {
        "legacy-after-unaware",
        "setp:U setaware get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:U\n"
        "SetProcessDpiAwarenessContext(U)=1 gle=-\n"
        "# setaware\n"
        "SetProcessDPIAware=1 gle=-\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "legacy-after-gdiscaled",
        "setp:G setaware get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:G\n"
        "SetProcessDpiAwarenessContext(G)=1 gle=-\n"
        "# setaware\n"
        "SetProcessDPIAware=1 gle=-\n"
        "# get\n"
        "get: thread=a0.v1.d96.f40000000 proc=a0.v1.d96.f40000000/a0.v1.d96.f40000000 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "internal-unaware",
        "intset0 get intset1 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# intset0\n"
        "SetProcessDpiAwarenessInternal(0)=1 gle=-\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# intset1\n"
        "SetProcessDpiAwarenessInternal(1)=0 gle=5\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "internal-system",
        "intset1 get setp:S",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# intset1\n"
        "SetProcessDpiAwarenessInternal(1)=1 gle=-\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
    },
    {
        "internal-pm",
        "intset2 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# intset2\n"
        "SetProcessDpiAwarenessInternal(2)=1 gle=-\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
    },
    {
        "internal-invalid",
        "intset3 get intset-1 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# intset3\n"
        "SetProcessDpiAwarenessInternal(3)=0 gle=87\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# intset-1\n"
        "SetProcessDpiAwarenessInternal(-1)=0 gle=87\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "shcore-system",
        "shset1 get setp:S shset1 shset0 shset2 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# shset1\n"
        "SetProcessDpiAwareness(1)=0x00000000\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# shset1\n"
        "SetProcessDpiAwareness(1)=0x80070005\n"
        "# shset0\n"
        "SetProcessDpiAwareness(0)=0x80070005\n"
        "# shset2\n"
        "SetProcessDpiAwareness(2)=0x80070005\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
    },
    {
        "shcore-pm",
        "shset2 get win:U",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# shset2\n"
        "SetProcessDpiAwareness(2)=0x00000000\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# win:U\n"
        "windows under a2.v1.d0.f0\n"
        "overlapped: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a2.v1.d0.f0\n"
        "windows under U\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "first window again: ctx=a2.v1.d0.f0, thread ctx=a2.v1.d0.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a2.v1.d0.f0\n"
    },
    {
        "shcore-invalid",
        "shset3 get shset-1 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# shset3\n"
        "SetProcessDpiAwareness(3)=0x80070057\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# shset-1\n"
        "SetProcessDpiAwareness(-1)=0x80070057\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "shcore-unaware",
        "shset0 get shset1 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# shset0\n"
        "SetProcessDpiAwareness(0)=0x00000000\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# shset1\n"
        "SetProcessDpiAwareness(1)=0x80070005\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "thread-over-process-1",
        "setp:S sett:U get win",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=1 gle=-\n"
        "# sett:U\n"
        "SetThreadDpiAwarenessContext(U) gle=- prev=a1.v1.SYS.f80000000\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# win\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a1.v1.SYS.f0\n"
    },
    {
        "thread-over-process-2",
        "setp:U sett:S get win",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:U\n"
        "SetProcessDpiAwarenessContext(U)=1 gle=-\n"
        "# sett:S\n"
        "SetThreadDpiAwarenessContext(S) gle=- prev=a0.v1.d96.f80000000\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# win\n"
        "windows under a1.v1.SYS.f0\n"
        "overlapped: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a1.v1.SYS.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "thread-over-process-3",
        "setp:S sett:P2 win:U",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=1 gle=-\n"
        "# sett:P2\n"
        "SetThreadDpiAwarenessContext(P2) gle=- prev=a1.v1.SYS.f80000000\n"
        "# win:U\n"
        "windows under a2.v2.d0.f0\n"
        "overlapped: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a2.v2.d0.f0\n"
        "windows under U\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "first window again: ctx=a2.v2.d0.f0, thread ctx=a2.v2.d0.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a1.v1.SYS.f0\n"
    },
    {
        "thread-before-process",
        "sett:S setp:P get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# sett:S\n"
        "SetThreadDpiAwarenessContext(S) gle=- prev=a0.v1.d96.f80000000\n"
        "# setp:P\n"
        "SetProcessDpiAwarenessContext(P)=1 gle=-\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
    },
    {
        "hosting",
        "hosting",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# hosting\n"
        "hosting: get=0\n"
        "hosting: set(1)=0 gle=-\n"
        "hosting: get=1\n"
        "hosting: set(7)=-1 gle=87\n"
        "hosting: set(-1)=-1 gle=87\n"
        "hosting: set(0)=1 gle=-\n"
        "hosting: get=0\n"
    },
    {
        "manifest-aware-true",
        "get setp:S get win",
        "aware-true",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# win\n"
        "windows under a1.v1.SYS.f0\n"
        "overlapped: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a1.v1.SYS.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a1.v1.SYS.f0\n"
    },
    {
        "manifest-aware-false",
        "get setp:S get win",
        "aware-false",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# win\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "manifest-aware-pm",
        "get setp:S get win",
        "aware-pm",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# win\n"
        "windows under a2.v1.d0.f0\n"
        "overlapped: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a2.v1.d0.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a2.v1.d0.f0\n"
    },
    {
        "manifest-aware-permonitor",
        "get setp:S get win",
        "aware-permonitor",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# win\n"
        "windows under a2.v1.d0.f0\n"
        "overlapped: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a2.v1.d0.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a2.v1.d0.f0\n"
    },
    {
        "manifest-aware-TRUE",
        "get setp:S get win",
        "aware-TRUE",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# win\n"
        "windows under a1.v1.SYS.f0\n"
        "overlapped: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a1.v1.SYS.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a1.v1.SYS.f0\n"
    },
    {
        "manifest-aware-junk",
        "get setp:S get win",
        "aware-junk",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# win\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "manifest-awn-system",
        "get setp:S get win",
        "awn-system",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# win\n"
        "windows under a1.v1.SYS.f0\n"
        "overlapped: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a1.v1.SYS.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a1.v1.SYS.f0\n"
    },
    {
        "manifest-awn-permonitor",
        "get setp:S get win",
        "awn-permonitor",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a2.v1.d0.f0 proc=a2.v1.d0.f0/a2.v1.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# win\n"
        "windows under a2.v1.d0.f0\n"
        "overlapped: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=1 ctx=a2.v1.d0.f0, later ctx=a2.v1.d0.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a2.v1.d0.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a2.v1.d0.f0\n"
    },
    {
        "manifest-awn-pmv2",
        "get setp:S get win",
        "awn-pmv2",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# win\n"
        "windows under a2.v2.d0.f0\n"
        "overlapped: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a2.v2.d0.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a2.v2.d0.f0\n"
    },
    {
        "manifest-awn-unaware",
        "get setp:S get win",
        "awn-unaware",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# win\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "manifest-awn-list",
        "get setp:S get win",
        "awn-list",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# win\n"
        "windows under a2.v2.d0.f0\n"
        "overlapped: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a2.v2.d0.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a2.v2.d0.f0\n"
    },
    {
        "manifest-awn-badfirst",
        "get setp:S get win",
        "awn-badfirst",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# win\n"
        "windows under a1.v1.SYS.f0\n"
        "overlapped: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a1.v1.SYS.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a1.v1.SYS.f0\n"
    },
    {
        "manifest-awn-junk",
        "get setp:S get win",
        "awn-junk",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=1 gle=-\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# win\n"
        "windows under a1.v1.SYS.f0\n"
        "overlapped: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a1.v1.SYS.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a1.v1.SYS.f0\n"
    },
    {
        "manifest-both",
        "get setp:S get win",
        "both",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# win\n"
        "windows under a2.v2.d0.f0\n"
        "overlapped: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=1 ctx=a2.v2.d0.f0, later ctx=a2.v2.d0.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a2.v2.d0.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a2.v2.d0.f0\n"
    },
    {
        "manifest-both2",
        "get setp:S get win",
        "both2",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# win\n"
        "windows under a0.v1.d96.f0\n"
        "overlapped: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a0.v1.d96.f0, later ctx=a0.v1.d96.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a0.v1.d96.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a0.v1.d96.f0\n"
    },
    {
        "manifest-none",
        "get setp:S get win",
        "none",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=1 gle=-\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# win\n"
        "windows under a1.v1.SYS.f0\n"
        "overlapped: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "owned popup: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "child: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "tool window: nccreate enable=0 ctx=a1.v1.SYS.f0, later ctx=a1.v1.SYS.f0 dpi=SYS enable=0/-\n"
        "static child: ctx=a1.v1.SYS.f0\n"
        "GetDpiForWindow NULL=0/87 bad=0/87 destroyed=0/87\n"
        "GetWindowDpiAwarenessContext NULL=a0.v0.d0.f0/1400 bad=a0.v0.d0.f0/1400, EnableNonClientDpiScaling NULL=0/1400 bad=0/1400\n"
        "desktop: ctx=a2.v1.d0.f0 dpi=SYS\n"
        "GetWindowDpiHostingBehavior desktop=0 NULL=-1/87\n"
        "new thread: ctx=a1.v1.SYS.f0\n"
    },
    {
        "nest-pmv2",
        "nest:P2",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# nest:P2\n"
        "parent under default, thread P2: parent ctx=a0.v1.d96.f0, child ctx=a0.v1.d96.f0 enable=0, owned popup ctx=a2.v2.d0.f0 enable=1\n"
        "  owned overlapped ctx=a2.v2.d0.f0 enable=1\n"
        "parent under P2, thread default: parent ctx=a2.v2.d0.f0, child ctx=a2.v2.d0.f0, owned overlapped ctx=a0.v1.d96.f0 enable=0\n"
        "mixed hosting, thread unaware: child ctx=a2.v2.d0.f0, top ctx=a0.v1.d96.f0\n"
        "hosting of the window made under mixed: 1, of its parent: 0, of the child: 1\n"
        "hosting of a window made after: 0\n"
    },
    {
        "nest-pm",
        "nest:P",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# nest:P\n"
        "parent under default, thread P: parent ctx=a0.v1.d96.f0, child ctx=a0.v1.d96.f0 enable=0, owned popup ctx=a2.v1.d0.f0 enable=1\n"
        "  owned overlapped ctx=a2.v1.d0.f0 enable=1\n"
        "parent under P, thread default: parent ctx=a2.v1.d0.f0, child ctx=a2.v1.d0.f0, owned overlapped ctx=a0.v1.d96.f0 enable=0\n"
        "mixed hosting, thread unaware: child ctx=a2.v1.d0.f0, top ctx=a0.v1.d96.f0\n"
        "hosting of the window made under mixed: 1, of its parent: 0, of the child: 1\n"
        "hosting of a window made after: 0\n"
    },
    {
        "nest-system",
        "nest:S",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# nest:S\n"
        "parent under default, thread S: parent ctx=a0.v1.d96.f0, child ctx=a0.v1.d96.f0 enable=0, owned popup ctx=a1.v1.SYS.f0 enable=0\n"
        "  owned overlapped ctx=a1.v1.SYS.f0 enable=0\n"
        "parent under S, thread default: parent ctx=a1.v1.SYS.f0, child ctx=a1.v1.SYS.f0, owned overlapped ctx=a0.v1.d96.f0 enable=0\n"
        "mixed hosting, thread unaware: child ctx=a1.v1.SYS.f0, top ctx=a0.v1.d96.f0\n"
        "hosting of the window made under mixed: 1, of its parent: 0, of the child: 1\n"
        "hosting of a window made after: 0\n"
    },
    {
        "nest-gdiscaled",
        "nest:G",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# nest:G\n"
        "parent under default, thread G: parent ctx=a0.v1.d96.f0, child ctx=a0.v1.d96.f0 enable=0, owned popup ctx=a0.v1.d96.f40000000 enable=0\n"
        "  owned overlapped ctx=a0.v1.d96.f40000000 enable=0\n"
        "parent under G, thread default: parent ctx=a0.v1.d96.f40000000, child ctx=a0.v1.d96.f40000000, owned overlapped ctx=a0.v1.d96.f0 enable=0\n"
        "mixed hosting, thread unaware: child ctx=a0.v1.d96.f40000000, top ctx=a0.v1.d96.f0\n"
        "hosting of the window made under mixed: 1, of its parent: 0, of the child: 1\n"
        "hosting of a window made after: 0\n"
    },
    {
        "nest-unaware",
        "nest:U",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# nest:U\n"
        "parent under default, thread U: parent ctx=a0.v1.d96.f0, child ctx=a0.v1.d96.f0 enable=0, owned popup ctx=a0.v1.d96.f0 enable=0\n"
        "  owned overlapped ctx=a0.v1.d96.f0 enable=0\n"
        "parent under U, thread default: parent ctx=a0.v1.d96.f0, child ctx=a0.v1.d96.f0, owned overlapped ctx=a0.v1.d96.f0 enable=0\n"
        "mixed hosting, thread unaware: child ctx=a0.v1.d96.f0, top ctx=a0.v1.d96.f0\n"
        "hosting of the window made under mixed: 1, of its parent: 0, of the child: 1\n"
        "hosting of a window made after: 0\n"
    },
    {
        "nest-unaware-in-system-process",
        "setp:S nest:U",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=1 gle=-\n"
        "# nest:U\n"
        "parent under default, thread U: parent ctx=a1.v1.SYS.f0, child ctx=a1.v1.SYS.f0 enable=0, owned popup ctx=a0.v1.d96.f0 enable=0\n"
        "  owned overlapped ctx=a0.v1.d96.f0 enable=0\n"
        "parent under U, thread default: parent ctx=a0.v1.d96.f0, child ctx=a0.v1.d96.f0, owned overlapped ctx=a1.v1.SYS.f0 enable=0\n"
        "mixed hosting, thread unaware: child ctx=a0.v1.d96.f0, top ctx=a0.v1.d96.f0\n"
        "hosting of the window made under mixed: 1, of its parent: 0, of the child: 1\n"
        "hosting of a window made after: 0\n"
    },
    {
        "nest-pmv2-in-pm-process",
        "setp:P nest:P2",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:P\n"
        "SetProcessDpiAwarenessContext(P)=1 gle=-\n"
        "# nest:P2\n"
        "parent under default, thread P2: parent ctx=a2.v1.d0.f0, child ctx=a2.v1.d0.f0 enable=0, owned popup ctx=a2.v2.d0.f0 enable=1\n"
        "  owned overlapped ctx=a2.v2.d0.f0 enable=1\n"
        "parent under P2, thread default: parent ctx=a2.v2.d0.f0, child ctx=a2.v2.d0.f0, owned overlapped ctx=a2.v1.d0.f0 enable=1\n"
        "mixed hosting, thread unaware: child ctx=a2.v2.d0.f0, top ctx=a0.v1.d96.f0\n"
        "hosting of the window made under mixed: 1, of its parent: 0, of the child: 1\n"
        "hosting of a window made after: 0\n"
    },
    {
        "bad-process-handle",
        "badproc",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# badproc\n"
        "GetProcessDpiAwarenessInternal(bad)=0 aw=-1 gle=87\n"
        "GetDpiAwarenessContextForProcess(bad)=a0.v0.d0.f0 gle=87\n"
        "GetDpiAwarenessContextForProcess(-1)=a0.v1.d96.f0 gle=-\n"
    },
    {
        "sign-extended-thread",
        "sett:sx:80006010 get sett:sx:80000012 get sett:sx:c0006010 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# sett:sx:80006010\n"
        "SetThreadDpiAwarenessContext(sx:80006010) gle=- prev=a0.v1.d96.f80000000\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:sx:80000012\n"
        "SetThreadDpiAwarenessContext(sx:80000012) gle=- prev=a0.v1.d96.f80000000\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
        "# sett:sx:c0006010\n"
        "SetThreadDpiAwarenessContext(sx:c0006010) gle=- prev=a0.v1.d96.f80000000\n"
        "# get\n"
        "get: thread=a0.v1.d96.f0 proc=a0.v1.d96.f0/a0.v1.d96.f0 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "sign-extended-process",
        "setp:sx:80006010 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:sx:80006010\n"
        "SetProcessDpiAwarenessContext(sx:80006010)=1 gle=-\n"
        "# get\n"
        "get: thread=a0.v1.d96.f80000000 proc=a0.v1.d96.f80000000/a0.v1.d96.f80000000 IsProcessDPIAware=0 internal=1:0/1:0 shcore=00000000:0/00000000:0 gle=-\n"
    },
    {
        "sign-extended-process-pm",
        "setp:sx:80000012 get setp:sx:80000022 get",
        NULL,
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# setp:sx:80000012\n"
        "SetProcessDpiAwarenessContext(sx:80000012)=1 gle=-\n"
        "# get\n"
        "get: thread=a2.v1.d0.f80000000 proc=a2.v1.d0.f80000000/a2.v1.d0.f80000000 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# setp:sx:80000022\n"
        "SetProcessDpiAwarenessContext(sx:80000022)=0 gle=5\n"
        "# get\n"
        "get: thread=a2.v1.d0.f80000000 proc=a2.v1.d0.f80000000/a2.v1.d0.f80000000 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
    },
    {
        "manifest-awn-len63",
        "get setp:S get",
        "awn-len63",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
    },
    {
        "manifest-awn-len64",
        "get setp:S get",
        "awn-len64",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
    },
    {
        "manifest-awn-len65",
        "get setp:S get",
        "awn-len65",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a1.v1.SYS.f0 proc=a1.v1.SYS.f0/a1.v1.SYS.f0 IsProcessDPIAware=1 internal=1:1/1:1 shcore=00000000:1/00000000:1 gle=-\n"
    },
    {
        "manifest-awn-len242",
        "get setp:S get",
        "awn-len242",
        "GetDpiForSystem==LOGPIXELSY: 1\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
        "# setp:S\n"
        "SetProcessDpiAwarenessContext(S)=0 gle=5\n"
        "# get\n"
        "get: thread=a2.v2.d0.f0 proc=a2.v2.d0.f0/a2.v2.d0.f0 IsProcessDPIAware=1 internal=1:2/1:2 shcore=00000000:2/00000000:2 gle=-\n"
    },
};
