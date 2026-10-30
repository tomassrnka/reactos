/*
 * PROJECT:     appshim_apitest
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Runs the VerifyVersionInfo cases of the Wine kernel32 version test against the version lie shims
 * COPYRIGHT:   Copyright 2006 Robert Shearman (the cases, from dlls/kernel32/tests/version.c of Wine)
 *              Copyright 2026 Tomas Srnka (running them against a version that is not the system's)
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#include "wine/test.h"
#include <string.h>

#include "appshim_apitest.h"

typedef BOOL(WINAPI* VERIFYVERSIONINFOAPROC)(LPOSVERSIONINFOEXA, DWORD, DWORDLONG);

/*
 * 'verify' is the VerifyVersionInfoA replacement of a lie that says the system is major.minor.build with
 * the given service pack. Every case builds the wanted version from that one, as the original does from the
 * real version, so the answers are the ones Windows gives for these conditions.
 */
void test_verify_version_cases(PVOID verify, DWORD major, DWORD minor, DWORD build, WORD sp_major, WORD sp_minor, PCSTR shim)
{
    enum srcversion_mode
    {
        SRCVERSION_ZERO         = 0,
        SRCVERSION_CURRENT      = 1,
        SRCVERSION_INC_MINOR    = 2,
        SRCVERSION_INC_SP_MINOR = 3,
        SRCVERSION_INC_SP_MAJOR = 4,
        SRCVERSION_DEC_SP_MAJOR = 5,
        SRCVERSION_DEC_MAJOR    = 6,
        SRCVERSION_INC_BUILD    = 7,
        SRCVERSION_REQUIRES_SP  = 0x1000,
    };

    struct verify_version_test
    {
        DWORD verifymask; /* Type mask for VerifyVersionInfo() */
        DWORD srcinfo;    /* The way current version info is modified. */
        DWORD err;        /* Error code on failure, 0 on success. */

        DWORD typemask1;
        DWORD condition1;
        DWORD typemask2;
        DWORD condition2;
        DWORD typemask3;
        DWORD condition3;
        DWORD typemask4;
        DWORD condition4;
    } verify_version_tests[] =
    {
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            0,

            VER_MAJORVERSION, VER_EQUAL,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_CURRENT,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_CURRENT,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_AND,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            0,

            VER_MAJORVERSION, VER_LESS_EQUAL,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_AND,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_OR,
            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_EQUAL,
        },
        {
            VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            0,

            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            0,

            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            0,

            VER_MAJORVERSION, VER_EQUAL,
            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_MINORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_EQUAL,
            VER_SERVICEPACKMINOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            0,

            VER_MAJORVERSION, VER_LESS_EQUAL,
            VER_SERVICEPACKMAJOR, VER_GREATER,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            0,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            0,

            VER_MAJORVERSION, VER_EQUAL,
            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_MAJOR,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_CURRENT,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_LESS_EQUAL,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_AND,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION,
            SRCVERSION_ZERO,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER,
            SRCVERSION_ZERO,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_SUITENAME,
            SRCVERSION_ZERO,
            0,

            VER_SUITENAME, VER_AND,
        },
        {
            VER_SUITENAME,
            SRCVERSION_ZERO,
            0,

            VER_SUITENAME, VER_OR,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            0,

            VER_MINORVERSION, VER_LESS,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            0,

            VER_MINORVERSION, VER_LESS_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_SP_MAJOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_MINOR,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_CURRENT,
            0,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_BUILD,
            ERROR_OLD_WIN_VERSION,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_INC_BUILD,
            0,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            0,

            VER_MINORVERSION, VER_GREATER,
        },
        {
            VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            0,

            VER_MINORVERSION, VER_GREATER_EQUAL,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            ERROR_OLD_WIN_VERSION,

            VER_MAJORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_GREATER,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_EQUAL,
            VER_SERVICEPACKMAJOR, VER_GREATER,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_LESS_EQUAL,
            VER_SERVICEPACKMAJOR, VER_GREATER,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            0,

            VER_MAJORVERSION, VER_GREATER_EQUAL,
            VER_MINORVERSION, VER_AND,
            VER_SERVICEPACKMAJOR, VER_GREATER,
        },
        {
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
            SRCVERSION_DEC_SP_MAJOR | SRCVERSION_REQUIRES_SP,
            ERROR_OLD_WIN_VERSION,

            VER_SERVICEPACKMAJOR, VER_GREATER,
            VER_SERVICEPACKMINOR, VER_EQUAL,
        },
    };


    OSVERSIONINFOEXA info;
    unsigned int i;
    BOOL ret;

    for (i = 0; i < ARRAY_SIZE(verify_version_tests); i++)
    {
        struct verify_version_test *test = &verify_version_tests[i];
        DWORD srcinfo = test->srcinfo;
        ULONGLONG mask;

        /* These cases need a service pack to take away */
        if (sp_major == 0 && srcinfo & SRCVERSION_REQUIRES_SP)
            continue;
        srcinfo &= ~SRCVERSION_REQUIRES_SP;

        memset(&info, 0, sizeof(info));
        info.dwOSVersionInfoSize = sizeof(info);
        info.dwMajorVersion = major;
        info.dwMinorVersion = minor;
        info.dwBuildNumber = build;
        info.dwPlatformId = VER_PLATFORM_WIN32_NT;
        info.wServicePackMajor = sp_major;
        info.wServicePackMinor = sp_minor;

        switch (srcinfo)
        {
        case SRCVERSION_ZERO:
            memset(&info, 0, sizeof(info));
            break;
        case SRCVERSION_INC_MINOR:
            info.dwMinorVersion++;
            break;
        case SRCVERSION_INC_SP_MINOR:
            info.wServicePackMinor++;
            break;
        case SRCVERSION_INC_SP_MAJOR:
            info.wServicePackMajor++;
            break;
        case SRCVERSION_DEC_SP_MAJOR:
            info.wServicePackMajor--;
            break;
        case SRCVERSION_DEC_MAJOR:
            info.dwMajorVersion--;
            break;
        case SRCVERSION_INC_BUILD:
            info.dwBuildNumber++;
            break;
        default:
            ;
        }

        mask = VerSetConditionMask(0, test->typemask1, test->condition1);
        if (test->typemask2)
            mask = VerSetConditionMask(mask, test->typemask2, test->condition2);
        if (test->typemask3)
            mask = VerSetConditionMask(mask, test->typemask3, test->condition3);
        if (test->typemask4)
            mask = VerSetConditionMask(mask, test->typemask4, test->condition4);

        SetLastError(0xdeadbeef);
        ret = ((VERIFYVERSIONINFOAPROC)verify)(&info, test->verifymask, mask);
        winetest_ok(test->err ? !ret : ret, "%u: unexpected return value %d for %s.\n", i, ret, shim);
        if (!ret)
            winetest_ok(GetLastError() == test->err, "%u: unexpected error code %lu, expected %lu for %s.\n", i, GetLastError(), test->err, shim);
    }

    /* v3.10 is always less than v4.x even if the minor version is tested */
    memset(&info, 0, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);
    info.dwMajorVersion = 3;
    info.dwMinorVersion = 10;
    ret = ((VERIFYVERSIONINFOAPROC)verify)(&info, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
        VerSetConditionMask(VerSetConditionMask(0, VER_MINORVERSION, VER_GREATER_EQUAL), VER_MAJORVERSION, VER_GREATER_EQUAL));
    winetest_ok(ret, "Version 3.10 is not less than the lie of %s, error %lu\n", shim, GetLastError());

    /* a size of 0 is not looked at */
    info.dwOSVersionInfoSize = 0;
    info.dwMajorVersion = major;
    ret = ((VERIFYVERSIONINFOAPROC)verify)(&info, VER_MAJORVERSION, VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL));
    winetest_ok(ret, "Bad dwOSVersionInfoSize was refused for %s, error %lu\n", shim, GetLastError());
}


/*
 * The sweep: every way the wanted version can differ by -1, 0 or +1 in the four fields, every combination of
 * those fields in the type mask, and every condition for one or two fields (the same condition for all fields
 * when three or four are tested). The order of the loops is the order of the bits in the tables, 6 bits to a
 * character of the alphabet below, first bit on top.
 *
 * Windows 10 (10.0.19045, x86 and x64) answered the same 15795 requests with exactly these bits when the version
 * it compares with has no service pack; the same comparison, run with a version that has one, gives the second
 * table.
 */
/* The version has no service pack: one less than the service pack major version is the largest number */
static const char g_SweepNoSp[] =
    "YwH/gAMH/gAD/wAYMH/gAD/wAYAAP/YxjGA/8ABg/8AAf+ADKg/8AAf+ADAAB/7G"
    "MYwH/gAMH/gAD/wAYMH/gAD/wAYAAP/YxjGA/8APg/8AAf+ADBg/8AAf+ADBgB/7"
    "GMYwH/gB8H/gAD/wAZUH/gAD/wAZUHwfYxjGA/8APg/8AAf+ADBg/8AAf+ADBgB/"
    "7GMYwH/gAMH/gAD/wAYMH/gAD/wAYAAP/YxjGA/8ABg/8AAf+ADKg/8AAf+ADAAB"
    "/7GMYwH/gAMH/gAD/wAYMH/gAD/wAYAAP/YxjPg/8ABg/8ABgB/7Bg/8ABgB/7AA"
    "B/7BsZ8H/gAMH/gAMAP/ZUH/gBUHwfYAAP/YNjPg/8ABg/8ABgB/7Bg/8ABgB/7A"
    "AB/7BsZ8H/gB8H/gB////YMH/gAMAP/YMAP/YNjPg/8APg/8AP///7Kg/8AKg+D7"
    "Kg+D7KsZ8H/gB8H/gB////YMH/gAMAP/YMAP/YNjPg/8ABg/8ABgB/7Bg/8ABgB/"
    "7AAB/7BsZ8H/gAMH/gAMAP/ZUH/gBUHwfYAAP/YNjPg/8ABg/8ABgB/7Bg/8ABgB"
    "/7AAB/7BsYMH/gAMH/gAAAP/YMH/gAAAP/YAAP/YNjBg/8ABg/8AAAB/7Kg/8AAA"
    "B/7AAB/7BsYMH/gAMH/gAAAP/YMH/gAAAP/YAAP/YNjBg/8APg/8AAAB/7Bg/8AA"
    "AB/7BgB/7BsYMH/gB8H/gAAAP/ZUH/gAAAP/ZUHwfYNjBg/8APg/8AAAB/7Bg/8A"
    "AAB/7BgB/7BsYMH/gAMH/gAAAP/YMH/gAAAP/YAAP/YNjBg/8ABg/8AAAB/7Kg/8"
    "AAAB/7AAB/7BsYMH/gAMH/gAAAP/YMH/gAAAP/YAAP/YNn2M/8ABjAD/wf+ADBjA"
    "D/wf+ADAAB/42M+xn/gAMYAf+D/wAZWoPg+D/wAYAAP/Gxn2M/8ABjAD/wf+ADBj"
    "AD/wf+ADAAB/42M+xn/gB////+D/wAYMYAf+D/wAYMAP/Gxn2M/8AP////wf+ADK"
    "1B8Hwf+ADKg+D9WM+xn/gB////+D/wAYMYAf+D/wAYMAP/Gxn2M/8ABjAD/wf+AD"
    "BjAD/wf+ADAAB/42M+xn/gAMYAf+D/wAZWoPg+D/wAYAAP/Gxn2M/8ABjAD/wf+A"
    "DBjAD/wf+ADAAB/42M/////+MYAf+MAP/GMYAf+MAP/GAAP/GMf/////xjAD/xgB"
    "/461B8H6g+D9QAB/4xj/////+MYAf+MAP/GMYAf+MAP/GAAP/GMf////////////"
    "///xjAD/xgB/4xgB/4xj////////////////WoPg/UHwfrUHwfrWv///////////"
    "////xjAD/xgB/4xgB/4xj/////+MYAf+MAP/GMYAf+MAP/GAAP/GMf/////xjAD/"
    "xgB/461B8H6g+D9QAB/4xj/////+MYAf+MAP/GMYAf+MAP/GAAP/GMfxjAD/xjAD"
    "/wAB/4xjAD/wAB/4wAB/4xj+MYAf+MYAf+AAP/HWoPg+AAP/GAAP/GMfxjAD/xjA"
    "D/wAB/4xjAD/wAB/4wAB/4xj+MYAf/////+AAP/GMYAf+AAP/GMAP/GMfxjAD///"
    "///wAB/461B8HwAB/46g+D9Rj+MYAf/////+AAP/GMYAf+AAP/GMAP/GMfxjAD/x"
    "jAD/wAB/4xjAD/wAB/4wAB/4xj+MYAf+MYAf+AAP/HWoPg+AAP/GAAP/GMfxjAD/"
    "xjAD/wAB/4xjAD/wAB/4wAB/4xjGwAAf+MAAf+D/wAGMAAf+D/wAGAAP/GwY2AAD"
    "/xgAD/wf+AA6gAD/wf+AAwAB/42DGwAAf+MAAf+D/wAGMAAf+D/wAGAAP/GwY2AA"
    "D//gAD/wf+AAxgAD/wf+AAxgB/42DGwAAf/8AAf+D/wAHUAAf+D/wAHUHwfGwY2A"
    "AD//gAD/wf+AAxgAD/wf+AAxgB/42DGwAAf+MAAf+D/wAGMAAf+D/wAGAAP/GwY2"
    "AAD/xgAD/wf+AA6gAD/wf+AAwAB/42DGwAAf+MAAf+D/wAGMAAf+D/wAGAAP/GwY"
    "/gAD/xgAD/xgB/4xgAD/xgB/4wAB/4xjH8AAf+MAAf+MAP/HUAAf/UHwfGAAP/GM"
    "Y/gAD/xgAD/xgB/4xgAD/xgB/4wAB/4xjH8AAf/8AAf/////GMAAf+MAP/GMAP/G"
    "MY/gAD//gAD/////46gAD/6g+D46g+D46jH8AAf/8AAf/////GMAAf+MAP/GMAP/"
    "GMY/gAD/xgAD/xgB/4xgAD/xgB/4wAB/4xjH8AAf+MAAf+MAP/HUAAf/UHwfGAAP"
    "/GMY/gAD/xgAD/xgB/4xgAD/xgB/4wAB/4xjGMAAf+MAAf+AAP/GMAAf+AAP/GAA"
    "P/GMYxgAD/xgAD/wAB/46gAD/wAB/4wAB/4xjGMAAf+MAAf+AAP/GMAAf+AAP/GA"
    "AP/GMYxgAD//gAD/wAB/4xgAD/wAB/4xgB/4xjGMAAf/8AAf+AAP/HUAAf+AAP/H"
    "UHwfGMYxgAD//gAD/wAB/4xgAD/wAB/4xgB/4xjGMAAf+MAAf+AAP/GMAAf+AAP/"
    "GAAP/GMYxgAD/xgAD/wAB/46gAD/wAB/4wAB/4xjGMAAf+MAAf+AAP/GMAAf+AAP"
    "/GAAP/GMY";
/* The version has a service pack */
static const char g_SweepSp[] =
    "YwH/gAwH/gAD/wAYMH/gAD/wAYD/wAYxjGA/8AGA/8AAf+ADKg/8AAf+ADAf+ADG"
    "MYwH/gAwH/gAD/wAYMH/gAD/wAYD/wAYxjGA/8APg/8AAf+ADBg/8AAf+ADBgB/7"
    "GMYwH/gB8H/gAD/wAZUH/gAD/wAZUHwfYxjGA/8APg/8AAf+ADBg/8AAf+ADBgB/"
    "7GMYwH/gAMH/gAD/wAYMH/gAD/wAYAAP/YxjGA/8ABg/8AAf+ADKg/8AAf+ADAAB"
    "/7GMYwH/gAMH/gAD/wAYMH/gAD/wAYAAP/YxjPg/8AGA/8AGf+ADBg/8ABgB/7Af"
    "+ADGMZ8H/gAwH/gAz/wAZUH/gBUHwfYD/wAYxjPg/8AGA/8AGf+ADBg/8ABgB/7A"
    "f+ADGMZ8H/gB8H/gB////YMH/gAMAP/YMAP/YNjPg/8APg/8AP///7Kg/8AKg+D7"
    "Kg+D7KsZ8H/gB8H/gB////YMH/gAMAP/YMAP/YNjPg/8ABg/8ABgB/7Bg/8ABgB/"
    "7AAB/7BsZ8H/gAMH/gAMAP/ZUH/gBUHwfYAAP/YNjPg/8ABg/8ABgB/7Bg/8ABgB"
    "/7AAB/7BsYMH/gAwH/gAAAP/YMH/gAAAP/YD/wAYNjBg/8AGA/8AAAB/7Kg/8AAA"
    "B/7Af+ADBsYMH/gAwH/gAAAP/YMH/gAAAP/YD/wAYNjBg/8APg/8AAAB/7Bg/8AA"
    "AB/7BgB/7BsYMH/gB8H/gAAAP/ZUH/gAAAP/ZUHwfYNjBg/8APg/8AAAB/7Bg/8A"
    "AAB/7BgB/7BsYMH/gAMH/gAAAP/YMH/gAAAP/YAAP/YNjBg/8ABg/8AAAB/7Kg/8"
    "AAAB/7AAB/7BsYMH/gAMH/gAAAP/YMH/gAAAP/YAAP/YNn2M/8AGM/8AAf+ADBjA"
    "D/wf+ADAf+ADGM+xn/gAxn/gAD/wAZWoPg+D/wAYD/wAYxn2M/8AGM/8AAf+ADBj"
    "AD/wf+ADAf+ADGM+xn/gB////+D/wAYMYAf+D/wAYMAP/Gxn2M/8AP////wf+ADK"
    "1B8Hwf+ADKg+D9WM+xn/gB////+D/wAYMYAf+D/wAYMAP/Gxn2M/8ABjAD/wf+AD"
    "BjAD/wf+ADAAB/42M+xn/gAMYAf+D/wAZWoPg+D/wAYAAP/Gxn2M/8ABjAD/wf+A"
    "DBjAD/wf+ADAAB/42M/////+xn/gAz/wAYMYAf+MAP/GD/wAYxn/////2M/8AGf+"
    "ADK1B8H6g+D9Qf+ADGM/////+xn/gAz/wAYMYAf+MAP/GD/wAYxn////////////"
    "///xjAD/xgB/4xgB/4xj////////////////WoPg/UHwfrUHwfrWv///////////"
    "////xjAD/xgB/4xgB/4xj/////+MYAf+MAP/GMYAf+MAP/GAAP/GMf/////xjAD/"
    "xgB/461B8H6g+D9QAB/4xj/////+MYAf+MAP/GMYAf+MAP/GAAP/GMfxjAD/2M/8"
    "AAAB/4xjAD/wAB/4wf+ADBj+MYAf+xn/gAAAP/HWoPg+AAP/GD/wAYMfxjAD/2M/"
    "8AAAB/4xjAD/wAB/4wf+ADBj+MYAf/////+AAP/GMYAf+AAP/GMAP/GMfxjAD///"
    "///wAB/461B8HwAB/46g+D9Rj+MYAf/////+AAP/GMYAf+AAP/GMAP/GMfxjAD/x"
    "jAD/wAB/4xjAD/wAB/4wAB/4xj+MYAf+MYAf+AAP/HWoPg+AAP/GAAP/GMfxjAD/"
    "xjAD/wAB/4xjAD/wAB/4wAB/4xjGwAAf+wAAf+D/wAGMAAf+D/wAGD/wAGwY2AAD"
    "/2AAD/wf+AA6gAD/wf+AAwf+AA2DGwAAf+wAAf+D/wAGMAAf+D/wAGD/wAGwY2AA"
    "D//gAD/wf+AAxgAD/wf+AAxgB/42DGwAAf/8AAf+D/wAHUAAf+D/wAHUHwfGwY2A"
    "AD//gAD/wf+AAxgAD/wf+AAxgB/42DGwAAf+MAAf+D/wAGMAAf+D/wAGAAP/GwY2"
    "AAD/xgAD/wf+AA6gAD/wf+AAwAB/42DGwAAf+MAAf+D/wAGMAAf+D/wAGAAP/GwY"
    "/gAD/2AAD/2f+AAxgAD/xgB/4wf+AA2DH8AAf+wAAf+z/wAHUAAf/UHwfGD/wAGw"
    "Y/gAD/2AAD/2f+AAxgAD/xgB/4wf+AA2DH8AAf/8AAf/////GMAAf+MAP/GMAP/G"
    "MY/gAD//gAD/////46gAD/6g+D46g+D46jH8AAf/8AAf/////GMAAf+MAP/GMAP/"
    "GMY/gAD/xgAD/xgB/4xgAD/xgB/4wAB/4xjH8AAf+MAAf+MAP/HUAAf/UHwfGAAP"
    "/GMY/gAD/xgAD/xgB/4xgAD/xgB/4wAB/4xjGMAAf+wAAf+AAP/GMAAf+AAP/GD/"
    "wAGMYxgAD/2AAD/wAB/46gAD/wAB/4wf+AAxjGMAAf+wAAf+AAP/GMAAf+AAP/GD"
    "/wAGMYxgAD//gAD/wAB/4xgAD/wAB/4xgB/4xjGMAAf/8AAf+AAP/HUAAf+AAP/H"
    "UHwfGMYxgAD//gAD/wAB/4xgAD/wAB/4xgB/4xjGMAAf+MAAf+AAP/GMAAf+AAP/"
    "GAAP/GMYxgAD/xgAD/wAB/46gAD/wAB/4wAB/4xjGMAAf+MAAf+AAP/GMAAf+AAP"
    "/GAAP/GMY";
/* Build number: the build number alone, and with the major and the minor version, 5 conditions, -1, 0, +1 */
static const char g_SweepBuild[] =
    "Yxla1GMY";

#define SWEEP_CASES 15795
#define SWEEP_BUILD_CASES 45

typedef BOOL(WINAPI* SWEEPVERIFYAPROC)(LPOSVERSIONINFOEXA, DWORD, DWORDLONG);
typedef BOOL(WINAPI* SWEEPVERIFYWPROC)(LPOSVERSIONINFOEXW, DWORD, DWORDLONG);
typedef LONG(NTAPI* SWEEPRTLVERIFYPROC)(PVOID, ULONG, ULONGLONG);

static int sweep_bit(const char* table, unsigned index)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    return ((strchr(alphabet, table[index / 6]) - alphabet) >> (5 - index % 6)) & 1;
}

/* Ask the three functions about one request; they must agree with each other and with the table */
static void sweep_check(PVOID verifyA, PVOID verifyW, PVOID verifyRtl, const OSVERSIONINFOEXW* want,
                        DWORD type, DWORDLONG cond, int expected, unsigned* bad, unsigned* shown, PCSTR shim, unsigned index)
{
    OSVERSIONINFOEXA wanta;
    OSVERSIONINFOEXW wantw = *want;
    int res[3] = { expected, expected, expected };
    DWORD err[3] = { 0, 0, 0 };
    int n;

    memset(&wanta, 0, sizeof(wanta));
    wanta.dwOSVersionInfoSize = sizeof(wanta);
    wanta.dwMajorVersion = want->dwMajorVersion;
    wanta.dwMinorVersion = want->dwMinorVersion;
    wanta.dwBuildNumber = want->dwBuildNumber;
    wanta.wServicePackMajor = want->wServicePackMajor;
    wanta.wServicePackMinor = want->wServicePackMinor;

    if (verifyA)
    {
        SetLastError(0xdeadbeef);
        res[0] = ((SWEEPVERIFYAPROC)verifyA)(&wanta, type, cond) != 0;
        err[0] = res[0] ? 0 : GetLastError();
    }
    if (verifyW)
    {
        SetLastError(0xdeadbeef);
        res[1] = ((SWEEPVERIFYWPROC)verifyW)(&wantw, type, cond) != 0;
        err[1] = res[1] ? 0 : GetLastError();
    }
    if (verifyRtl)
    {
        LONG status = ((SWEEPRTLVERIFYPROC)verifyRtl)(&wantw, type, cond);

        res[2] = (status == 0);
        err[2] = res[2] ? 0 : (DWORD)status;
        if (!res[2] && status != (LONG)0xC0000059)
            res[2] = -1;
    }

    for (n = 0; n < 3; ++n)
    {
        BOOL wrong = (res[n] != expected) || (!expected && n < 2 && err[n] != ERROR_OLD_WIN_VERSION);

        if (!wrong)
            continue;
        ++*bad;
        if ((*shown)++ < 8)
        {
            static const char* const names[3] = { "VerifyVersionInfoA", "VerifyVersionInfoW", "RtlVerifyVersionInfo" };
            ok(0, "%s case %u (want %lu.%lu.%lu.%lu, type 0x%lx, conditions 0x%I64x): expected %d, got %d (error 0x%lx) for %s\n",
               names[n], index, want->dwMajorVersion, want->dwMinorVersion, (DWORD)want->wServicePackMajor, (DWORD)want->wServicePackMinor,
               type, cond, expected, res[n], err[n], shim);
        }
    }
}

/*
 * Run the sweep against the functions (any of them may be NULL) for a system that is major.minor.build with
 * the given service pack major version (the minor one is 0).
 */
void test_verify_version_sweep(PVOID verifyA, PVOID verifyW, PVOID verifyRtl, DWORD major, DWORD minor, DWORD build, WORD sp_major, PCSTR shim)
{
    static const DWORD types[4] = { VER_MAJORVERSION, VER_MINORVERSION, VER_SERVICEPACKMAJOR, VER_SERVICEPACKMINOR };
    const char* table = sp_major ? g_SweepSp : g_SweepNoSp;
    unsigned index = 0, bad = 0, shown = 0;
    int d[4], mask, i;

    for (d[0] = -1; d[0] <= 1; d[0]++) for (d[1] = -1; d[1] <= 1; d[1]++) for (d[2] = -1; d[2] <= 1; d[2]++) for (d[3] = -1; d[3] <= 1; d[3]++)
    {
        OSVERSIONINFOEXW want;

        memset(&want, 0, sizeof(want));
        want.dwOSVersionInfoSize = sizeof(want);
        want.dwMajorVersion = major + d[0];
        want.dwMinorVersion = minor + d[1];
        want.dwBuildNumber = build;
        want.wServicePackMajor = (WORD)(sp_major + d[2]);
        want.wServicePackMinor = (WORD)(0 + d[3]);

        for (mask = 1; mask < 16; mask++)
        {
            int count = 0, c[4], hi[4];

            for (i = 0; i < 4; i++)
            {
                hi[i] = (mask & (1 << i)) ? 5 : 0;
                count += (mask & (1 << i)) ? 1 : 0;
            }
            for (c[0] = hi[0] ? 1 : 0; c[0] <= hi[0]; c[0]++) for (c[1] = hi[1] ? 1 : 0; c[1] <= hi[1]; c[1]++)
            for (c[2] = hi[2] ? 1 : 0; c[2] <= hi[2]; c[2]++) for (c[3] = hi[3] ? 1 : 0; c[3] <= hi[3]; c[3]++)
            {
                DWORD type = 0;
                DWORDLONG cond = 0;
                int same = 1, first = -1;

                for (i = 0; i < 4; i++)
                {
                    if (!(mask & (1 << i)))
                        continue;
                    type |= types[i];
                    cond = VerSetConditionMask(cond, types[i], (BYTE)c[i]);
                    if (first < 0)
                        first = c[i];
                    else if (c[i] != first)
                        same = 0;
                }
                if (count >= 3 && !same)
                    continue;

                sweep_check(verifyA, verifyW, verifyRtl, &want, type, cond, sweep_bit(table, index), &bad, &shown, shim, index);
                index++;
            }
        }
    }

    ok(index == SWEEP_CASES, "Expected %u cases in the sweep, ran %u for %s\n", SWEEP_CASES, index, shim);
    ok(bad == 0, "%u answers of the sweep of %s differ from Windows\n", bad, shim);

    /* The build number, which is compared before the version, with the versions equal */
    index = 0;
    bad = 0;
    shown = 0;
    for (d[0] = -1; d[0] <= 1; d[0]++) for (mask = 0; mask < 3; mask++) for (i = 1; i <= 5; i++)
    {
        OSVERSIONINFOEXW want;
        DWORD type = VER_BUILDNUMBER;
        DWORDLONG cond = 0;

        memset(&want, 0, sizeof(want));
        want.dwOSVersionInfoSize = sizeof(want);
        want.dwMajorVersion = major;
        want.dwMinorVersion = minor;
        want.dwBuildNumber = build + d[0];
        want.wServicePackMajor = sp_major;
        if (mask >= 1)
        {
            type |= VER_MAJORVERSION;
            cond = VerSetConditionMask(cond, VER_MAJORVERSION, (BYTE)i);
        }
        if (mask >= 2)
        {
            type |= VER_MINORVERSION;
            cond = VerSetConditionMask(cond, VER_MINORVERSION, (BYTE)i);
        }
        cond = VerSetConditionMask(cond, VER_BUILDNUMBER, (BYTE)i);
        sweep_check(verifyA, verifyW, verifyRtl, &want, type, cond, sweep_bit(g_SweepBuild, index), &bad, &shown, shim, index);
        index++;
    }
    ok(index == SWEEP_BUILD_CASES, "Expected %u build cases, ran %u for %s\n", SWEEP_BUILD_CASES, index, shim);
    ok(bad == 0, "%u answers of the build number sweep of %s differ from Windows\n", bad, shim);
}
