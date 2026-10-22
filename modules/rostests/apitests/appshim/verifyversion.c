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
