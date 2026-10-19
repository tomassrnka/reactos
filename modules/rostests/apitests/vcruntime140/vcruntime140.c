/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests that the Visual C++ runtime DLLs are in the system and work
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pseh/pseh2.h>
#include <windef.h>
#include <winbase.h>
#include <winver.h>
#include <versionhelpers.h>

#define CXX_EXCEPTION 0xE06D7363

#ifdef _WIN64
#define X64_ONLY(a, b) a
#else
#define X64_ONLY(a, b) b
#endif

/*
 * The version resource that Wine declares for each module (an installer of Microsoft carries newer ones).
 * Some modules are not built in every configuration (the MSVC build, export levels below Windows Vista,
 * i386 for vcruntime140_1): the build tells which ones to expect.
 */
typedef struct
{
    const char *name;
    BOOL expected;
    WORD major, minor, build, revision;
} RUNTIME_DLL;

#ifdef VCRT_NO_MSVCP140
#define EXPECT_MSVCP140 FALSE
#else
#define EXPECT_MSVCP140 TRUE
#endif

#ifdef _WIN64
#ifdef VCRT_NO_VCRUNTIME140_1
#define EXPECT_VCRUNTIME140_1 FALSE
#else
#define EXPECT_VCRUNTIME140_1 TRUE
#endif
#else
#define EXPECT_VCRUNTIME140_1 FALSE
#endif

static const RUNTIME_DLL g_dlls[] =
{
    { "vcruntime140.dll",   TRUE,                  10, 0, 19045, 5796 },
    { "vcruntime140_1.dll", EXPECT_VCRUNTIME140_1, 14, 50, 35719, 0 },
    { "msvcp140.dll",       EXPECT_MSVCP140,       14, 50, 35719, 0 },
    { "msvcp140_1.dll",     EXPECT_MSVCP140,       10, 0, 19045, 5796 },
    { "msvcp140_2.dll",     TRUE,                  14, 50, 35719, 0 },
    { "concrt140.dll",      EXPECT_MSVCP140,       10, 0, 19045, 5796 },
};

static BOOL is_reactos_file(void *data)
{
    char *desc = NULL;
    UINT len = 0;

    return VerQueryValueA(data, "\\StringFileInfo\\040904b0\\FileDescription", (void **)&desc, &len) &&
           desc && !strncmp(desc, "ReactOS", 7);
}

static void test_dll_file(const RUNTIME_DLL *dll)
{
    char path[MAX_PATH], sysdir[MAX_PATH], *slash;
    HMODULE module;
    DWORD len, dummy, size;
    void *data;
    VS_FIXEDFILEINFO *info;
    UINT infolen;
    BYTE linker_minor = 0;

    SetLastError(0xdeadbeef);
    module = LoadLibraryA(dll->name);
    ok(module != NULL, "%s: LoadLibrary failed, error %lu\n", dll->name, GetLastError());
    if (!module)
        return;

    /* programs such as PHP compare this with the linker version of their own build */
    {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)module;
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE *)module + dos->e_lfanew);

        ok(nt->OptionalHeader.MajorLinkerVersion == 14, "%s: linker version %u.%u\n",
           dll->name, nt->OptionalHeader.MajorLinkerVersion, nt->OptionalHeader.MinorLinkerVersion);
        linker_minor = nt->OptionalHeader.MinorLinkerVersion;
    }

    len = GetModuleFileNameA(module, path, sizeof(path));
    ok(len > 0, "%s: GetModuleFileName failed\n", dll->name);
    slash = strrchr(path, '\\');
    GetSystemDirectoryA(sysdir, sizeof(sysdir));
    if (slash)
    {
        *slash = 0;
        ok(!_stricmp(path, sysdir), "%s: loaded from %s, expected %s\n", dll->name, path, sysdir);
    }

    size = GetFileVersionInfoSizeA(dll->name, &dummy);
    if (!size)
    {
        /* the name alone searches the application directory first: use the full path */
        GetSystemDirectoryA(path, sizeof(path));
        strcat(path, "\\");
        strcat(path, dll->name);
        size = GetFileVersionInfoSizeA(path, &dummy);
    }
    ok(size != 0, "%s: no version resource, error %lu\n", dll->name, GetLastError());
    if (size)
    {
        data = malloc(size);
        GetSystemDirectoryA(path, sizeof(path));
        strcat(path, "\\");
        strcat(path, dll->name);
        ok(GetFileVersionInfoA(path, 0, size, data), "%s: GetFileVersionInfo failed\n", dll->name);
        info = NULL;
        infolen = 0;
        if (VerQueryValueA(data, "\\", (void **)&info, &infolen) && info)
        {
            ok(HIWORD(info->dwFileVersionMS) >= 10, "%s: file version %u.%u\n",
               dll->name, HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS));
            /* the installers of Microsoft replace a file when theirs is newer: keep Wine's version in ours */
            if (IsReactOS() && is_reactos_file(data))
            {
#ifdef VCRT_EXPECT_LINKER_MINOR
                ok(linker_minor == VCRT_EXPECT_LINKER_MINOR, "%s: linker version minor %u, expected %u\n",
                   dll->name, linker_minor, VCRT_EXPECT_LINKER_MINOR);
#endif
                ok(HIWORD(info->dwFileVersionMS) == dll->major, "%s: major %u, expected %u\n",
                   dll->name, HIWORD(info->dwFileVersionMS), dll->major);
                ok(LOWORD(info->dwFileVersionMS) == dll->minor, "%s: minor %u, expected %u\n",
                   dll->name, LOWORD(info->dwFileVersionMS), dll->minor);
                ok(HIWORD(info->dwFileVersionLS) == dll->build, "%s: build %u, expected %u\n",
                   dll->name, HIWORD(info->dwFileVersionLS), dll->build);
                ok(LOWORD(info->dwFileVersionLS) == dll->revision, "%s: revision %u, expected %u\n",
                   dll->name, LOWORD(info->dwFileVersionLS), dll->revision);
            }
        }
        else
        {
            ok(0, "%s: VerQueryValue failed\n", dll->name);
        }
        free(data);
    }

    FreeLibrary(module);
}

static void test_exports(void)
{
    static const char *vcruntime_names[] =
    {
        "_CxxThrowException", "__CxxFrameHandler3", "__std_exception_copy", "__std_exception_destroy",
        "__std_type_info_compare", "__std_type_info_hash", "__std_type_info_name", "__std_terminate",
        "__RTDynamicCast", "__uncaught_exceptions", "memcpy", "memset", "strchr",
    };
    HMODULE vcruntime = GetModuleHandleA("vcruntime140.dll");
    HMODULE msvcp = GetModuleHandleA("msvcp140.dll");
    HMODULE concrt = GetModuleHandleA("concrt140.dll");
    HMODULE msvcp1 = GetModuleHandleA("msvcp140_1.dll");
    HMODULE msvcp2 = GetModuleHandleA("msvcp140_2.dll");
    unsigned i;

    for (i = 0; i < _countof(vcruntime_names); i++)
        ok(vcruntime && GetProcAddress(vcruntime, vcruntime_names[i]) != NULL,
           "vcruntime140: no export %s\n", vcruntime_names[i]);

    if (msvcp)
    {
        ok(GetProcAddress(msvcp, "?_Xbad_alloc@std@@YAXXZ") != NULL, "msvcp140: no _Xbad_alloc\n");
        ok(GetProcAddress(msvcp, "?uncaught_exception@std@@YA_NXZ") != NULL, "msvcp140: no uncaught_exception\n");
        ok(GetProcAddress(msvcp, "_Thrd_start") != NULL, "msvcp140: no _Thrd_start\n");
        ok(GetProcAddress(msvcp, "_Mtx_init") != NULL, "msvcp140: no _Mtx_init\n");
        ok(GetProcAddress(msvcp, "_Cnd_init") != NULL, "msvcp140: no _Cnd_init\n");
    }
    if (msvcp1)
    {
        ok(GetProcAddress(msvcp1, "_Aligned_get_default_resource") != NULL,
           "msvcp140_1: no _Aligned_get_default_resource\n");
    }
    ok(msvcp2 && GetProcAddress(msvcp2, X64_ONLY("__std_smf_hypot3", "___std_smf_hypot3@24")) != NULL,
       "msvcp140_2: no hypot3\n");
    if (concrt)
    {
        ok(GetProcAddress(concrt, "?_GetConcurrency@details@Concurrency@@YAIXZ") != NULL,
           "concrt140: no _GetConcurrency\n");
    }
#ifdef _WIN64
    {
        HMODULE v1 = GetModuleHandleA("vcruntime140_1.dll");
        if (v1)
            ok(GetProcAddress(v1, "__CxxFrameHandler4") != NULL, "vcruntime140_1: no __CxxFrameHandler4\n");
    }
#endif
}

static void test_cxx_exception(void)
{
    HMODULE msvcp = GetModuleHandleA("msvcp140.dll");
    void (__cdecl *p_Xbad_alloc)(void);
    void (__cdecl *p_Xlength_error)(const char *);
    BYTE (__cdecl *p_uncaught_exception)(void);
    DWORD code;

    if (!msvcp)
    {
        skip("msvcp140 is not part of this build\n");
        return;
    }

    p_Xbad_alloc = (void *)GetProcAddress(msvcp, "?_Xbad_alloc@std@@YAXXZ");
    p_Xlength_error = (void *)GetProcAddress(msvcp, X64_ONLY("?_Xlength_error@std@@YAXPEBD@Z", "?_Xlength_error@std@@YAXPBD@Z"));
    p_uncaught_exception = (void *)GetProcAddress(msvcp, "?uncaught_exception@std@@YA_NXZ");
    if (!p_Xbad_alloc || !p_Xlength_error || !p_uncaught_exception)
    {
        skip("msvcp140 does not export the exception helpers\n");
        return;
    }

    ok(p_uncaught_exception() == 0, "uncaught_exception is true outside of a throw\n");

    code = 0;
    _SEH2_TRY
    {
        p_Xbad_alloc();
    }
    _SEH2_EXCEPT((code = _SEH2_GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER))
    {
    }
    _SEH2_END;
    ok(code == CXX_EXCEPTION, "bad_alloc raised %#lx\n", code);

    code = 0;
    _SEH2_TRY
    {
        p_Xlength_error("vector too long");
    }
    _SEH2_EXCEPT((code = _SEH2_GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER))
    {
    }
    _SEH2_END;
    ok(code == CXX_EXCEPTION, "length_error raised %#lx\n", code);
}

static void test_math_and_concurrency(void)
{
    HMODULE msvcp2 = GetModuleHandleA("msvcp140_2.dll");
    HMODULE concrt = GetModuleHandleA("concrt140.dll");
    double (__stdcall *p_hypot3)(double, double, double);
    unsigned (__cdecl *p_GetConcurrency)(void);
    SYSTEM_INFO si;
    unsigned conc;

    p_hypot3 = (void *)GetProcAddress(msvcp2, X64_ONLY("__std_smf_hypot3", "___std_smf_hypot3@24"));
    if (p_hypot3)
        ok(p_hypot3(3.0, 4.0, 12.0) == 13.0, "hypot3(3,4,12) = %f\n", p_hypot3(3.0, 4.0, 12.0));

    p_GetConcurrency = concrt ? (void *)GetProcAddress(concrt, "?_GetConcurrency@details@Concurrency@@YAIXZ") : NULL;
    if (p_GetConcurrency)
    {
        GetSystemInfo(&si);
        conc = p_GetConcurrency();
        ok(conc == si.dwNumberOfProcessors, "_GetConcurrency returned %u, expected %lu\n", conc, si.dwNumberOfProcessors);
    }
}

START_TEST(vcruntime140)
{
    unsigned i;

    for (i = 0; i < _countof(g_dlls); i++)
    {
        if (g_dlls[i].expected)
            test_dll_file(&g_dlls[i]);
    }

    /* the DLLs have to be loaded for the next tests: they were freed above */
    for (i = 0; i < _countof(g_dlls); i++)
    {
        if (g_dlls[i].expected)
            LoadLibraryA(g_dlls[i].name);
    }

    test_exports();
    test_cxx_exception();
    test_math_and_concurrency();
}
