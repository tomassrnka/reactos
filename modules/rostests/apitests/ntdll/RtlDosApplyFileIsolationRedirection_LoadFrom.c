/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the loadFrom attribute of a manifest <file> element
 *              (RtlDosApplyFileIsolationRedirection_Ustr and the loader)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static WCHAR g_TestDir[MAX_PATH];
static WCHAR g_SubDir[MAX_PATH];
static WCHAR g_SystemRoot[MAX_PATH];
static UNICODE_STRING DotDll = RTL_CONSTANT_STRING(L".DLL");

static BOOL
WriteManifest(
    _In_ PCWSTR Path)
{
    static const char Format[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n"
        "  <assemblyIdentity type=\"win32\" name=\"ntdll_apitest_loadfrom\" "
        "version=\"1.0.0.0\"/>\r\n"
        "  <file name=\"lfsys.dll\" loadFrom=\"%%SystemRoot%%\\system32\\\"/>\r\n"
        "  <file name=\"lfenv.dll\" loadFrom=\"%%LOADFROMTEST_DIR%%\\sub\\\"/>\r\n"
        "  <file name=\"lfplain.dll\"/>\r\n"
        "  <file name=\"lfabs.dll\" loadFrom=\"%s\\abs-dir\\\"/>\r\n"
        /* No trailing backslash: the value names the file itself */
        "  <file name=\"lffull.dll\" loadFrom=\"%s\\sub\\renamed.dll\"/>\r\n"
        /* An undefined variable stays as written */
        "  <file name=\"lfnovar.dll\" loadFrom=\"%%LOADFROMTEST_NOSUCHVAR%%\\x\\\"/>\r\n"
        "</assembly>\r\n";
    char Text[4 * MAX_PATH];
    char Dir[MAX_PATH];
    HANDLE File;
    DWORD Written;
    UINT i;
    BOOL Ok;

    /* The temp path is ASCII in the test environments; anything else makes the test skip */
    for (i = 0; g_TestDir[i]; i++)
    {
        if (g_TestDir[i] > 0x7f || g_TestDir[i] == L'&' || g_TestDir[i] == L'<')
            return FALSE;
        Dir[i] = (char)g_TestDir[i];
    }
    Dir[i] = 0;
    if (FAILED(StringCchPrintfA(Text, _countof(Text), Format, Dir, Dir)))
        return FALSE;

    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(File, Text, strlen(Text), &Written, NULL);
    CloseHandle(File);
    return Ok;
}

static void
CheckRedirection(
    _In_ INT Line,
    _In_ PCWSTR Name,
    _In_ NTSTATUS ExpectedStatus,
    _In_opt_ PCWSTR Expected)
{
    UNICODE_STRING Original, Dynamic;
    PUNICODE_STRING Used = NULL;
    NTSTATUS Status;

    RtlInitUnicodeString(&Original, Name);
    RtlInitEmptyUnicodeString(&Dynamic, NULL, 0);
    Status = RtlDosApplyFileIsolationRedirection_Ustr(TRUE, &Original, &DotDll, NULL, &Dynamic,
                                                      &Used, NULL, NULL, NULL);
    ok_(__FILE__, Line)(Status == ExpectedStatus,
                        "%S: status 0x%lx, expected 0x%lx\n", Name, Status, ExpectedStatus);
    if (!NT_SUCCESS(Status) || !Expected)
        return;

    ok_(__FILE__, Line)(Used == &Dynamic, "%S: string used %p, expected %p\n",
                        Name, Used, &Dynamic);
    ok_(__FILE__, Line)(Dynamic.Buffer != NULL, "%S: no string\n", Name);
    if (!Dynamic.Buffer)
        return;
    ok_(__FILE__, Line)(Dynamic.MaximumLength == Dynamic.Length + sizeof(WCHAR),
                        "%S: lengths %u and %u\n", Name, Dynamic.Length, Dynamic.MaximumLength);
    ok_(__FILE__, Line)(_wcsnicmp(Dynamic.Buffer, Expected, Dynamic.Length / sizeof(WCHAR)) == 0 &&
                        wcslen(Expected) * sizeof(WCHAR) == Dynamic.Length,
                        "%S: got %S, expected %S\n", Name, Dynamic.Buffer, Expected);
    RtlFreeUnicodeString(&Dynamic);
}

static void
TestPaths(void)
{
    WCHAR Expected[2 * MAX_PATH];

    StringCchPrintfW(Expected, _countof(Expected), L"%ls\\system32\\lfsys.dll", g_SystemRoot);
    CheckRedirection(__LINE__, L"lfsys.dll", STATUS_SUCCESS, Expected);
    /* The name is matched without regard to case and the extension is added */
    StringCchPrintfW(Expected, _countof(Expected), L"%ls\\system32\\LFSYS.DLL", g_SystemRoot);
    CheckRedirection(__LINE__, L"LFSYS", STATUS_SUCCESS, Expected);

    StringCchPrintfW(Expected, _countof(Expected), L"%ls\\sub\\lfenv.dll", g_TestDir);
    CheckRedirection(__LINE__, L"lfenv.dll", STATUS_SUCCESS, Expected);

    StringCchPrintfW(Expected, _countof(Expected), L"%ls\\abs-dir\\lfabs.dll", g_TestDir);
    CheckRedirection(__LINE__, L"lfabs.dll", STATUS_SUCCESS, Expected);

    StringCchPrintfW(Expected, _countof(Expected), L"%ls\\sub\\renamed.dll", g_TestDir);
    CheckRedirection(__LINE__, L"lffull.dll", STATUS_SUCCESS, Expected);

    CheckRedirection(__LINE__, L"lfnovar.dll", STATUS_SUCCESS,
                     L"%LOADFROMTEST_NOSUCHVAR%\\x\\lfnovar.dll");

    /* A file without loadFrom is in the directory of the manifest */
    StringCchPrintfW(Expected, _countof(Expected), L"%ls\\lfplain.dll", g_TestDir);
    CheckRedirection(__LINE__, L"lfplain.dll", STATUS_SUCCESS, Expected);

    CheckRedirection(__LINE__, L"lfmissing.dll", STATUS_SXS_KEY_NOT_FOUND, NULL);
}

static void
TestBuffers(void)
{
    UNICODE_STRING Original, Static, Dynamic;
    PUNICODE_STRING Used;
    WCHAR Buffer[2 * MAX_PATH], Expected[2 * MAX_PATH];
    UINT ExpectedLength;
    NTSTATUS Status;

    StringCchPrintfW(Expected, _countof(Expected), L"%ls\\sub\\lfenv.dll", g_TestDir);
    ExpectedLength = wcslen(Expected) * sizeof(WCHAR);
    RtlInitUnicodeString(&Original, L"lfenv.dll");

    /* A caller buffer that is large enough receives the expanded path */
    RtlInitEmptyUnicodeString(&Static, Buffer, sizeof(Buffer));
    Used = NULL;
    Status = RtlDosApplyFileIsolationRedirection_Ustr(TRUE, &Original, NULL, &Static, NULL, &Used,
                                                      NULL, NULL, NULL);
    ok(Status == STATUS_SUCCESS, "status 0x%lx\n", Status);
    ok(Used == &Static, "string used %p, expected %p\n", Used, &Static);
    ok(Static.Buffer == Buffer, "buffer replaced\n");
    ok(Static.MaximumLength == sizeof(Buffer), "MaximumLength %u\n", Static.MaximumLength);
    ok(Static.Length == ExpectedLength, "Length %u, expected %u\n", Static.Length, ExpectedLength);
    ok(_wcsicmp(Static.Buffer, Expected) == 0, "got %S, expected %S\n", Static.Buffer, Expected);

    /* Too small, and no dynamic string */
    RtlInitEmptyUnicodeString(&Static, Buffer, 5 * sizeof(WCHAR));
    Status = RtlDosApplyFileIsolationRedirection_Ustr(TRUE, &Original, NULL, &Static, NULL, &Used,
                                                      NULL, NULL, NULL);
    ok(Status == STATUS_BUFFER_TOO_SMALL, "status 0x%lx\n", Status);

    /* Too small, with a dynamic string */
    RtlInitEmptyUnicodeString(&Static, Buffer, 5 * sizeof(WCHAR));
    RtlInitEmptyUnicodeString(&Dynamic, NULL, 0);
    Used = NULL;
    Status = RtlDosApplyFileIsolationRedirection_Ustr(TRUE, &Original, NULL, &Static, &Dynamic,
                                                      &Used, NULL, NULL, NULL);
    ok(Status == STATUS_SUCCESS, "status 0x%lx\n", Status);
    ok(Used == &Dynamic, "string used %p, expected %p\n", Used, &Dynamic);
    ok(Dynamic.Buffer != NULL, "no dynamic string\n");
    if (Dynamic.Buffer)
    {
        ok(Dynamic.Length == ExpectedLength, "Length %u, expected %u\n",
           Dynamic.Length, ExpectedLength);
        ok(_wcsicmp(Dynamic.Buffer, Expected) == 0, "got %S, expected %S\n",
           Dynamic.Buffer, Expected);
        RtlFreeUnicodeString(&Dynamic);
    }
}

static void
TestLoad(void)
{
    WCHAR Source[MAX_PATH], Target[2 * MAX_PATH], Loaded[2 * MAX_PATH];
    HMODULE Module;
    DWORD Length;

    /* A copy of a small system DLL under the name the manifest knows */
    StringCchPrintfW(Source, _countof(Source), L"%ls\\system32\\lz32.dll", g_SystemRoot);
    StringCchPrintfW(Target, _countof(Target), L"%ls\\lfenv.dll", g_SubDir);
    if (!CopyFileW(Source, Target, FALSE))
    {
        skip("Cannot copy %S: %lu\n", Source, GetLastError());
        return;
    }

    SetLastError(0xdeaddead);
    Module = LoadLibraryW(L"lfenv.dll");
    ok(Module != NULL, "LoadLibrary failed with %lu\n", GetLastError());
    if (Module)
    {
        Length = GetModuleFileNameW(Module, Loaded, _countof(Loaded));
        ok(Length != 0, "GetModuleFileName failed with %lu\n", GetLastError());
        ok(_wcsicmp(Loaded, Target) == 0, "loaded %S, expected %S\n", Loaded, Target);
        FreeLibrary(Module);
    }

    DeleteFileW(Target);
}

static void
RunTests(
    _In_ PCWSTR Manifest)
{
    ACTCTXW ActCtx = { sizeof(ActCtx) };
    HANDLE Handle;
    ULONG_PTR Cookie;

    ActCtx.lpSource = Manifest;
    Handle = CreateActCtxW(&ActCtx);
    ok(Handle != INVALID_HANDLE_VALUE, "CreateActCtx failed with %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return;

    if (ActivateActCtx(Handle, &Cookie))
    {
        TestPaths();
        TestBuffers();
        TestLoad();
        DeactivateActCtx(0, Cookie);
    }
    else
    {
        ok(0, "ActivateActCtx failed with %lu\n", GetLastError());
    }
    ReleaseActCtx(Handle);
}

START_TEST(RtlDosApplyFileIsolationRedirection_LoadFrom)
{
    static const WCHAR DirVariable[] = L"LOADFROMTEST_DIR";
    static const WCHAR NoSuchVariable[] = L"LOADFROMTEST_NOSUCHVAR";
    static WCHAR SavedDir[UNICODE_STRING_MAX_CHARS], SavedNoSuch[UNICODE_STRING_MAX_CHARS];
    WCHAR Manifest[MAX_PATH];
    DWORD Length, SavedDirLength, SavedNoSuchLength;

    Length = GetEnvironmentVariableW(L"SystemRoot", g_SystemRoot, _countof(g_SystemRoot));
    if (!Length || Length >= _countof(g_SystemRoot))
    {
        skip("No SystemRoot\n");
        return;
    }

    Length = GetTempPathW(_countof(g_TestDir), g_TestDir);
    if (!Length || Length >= _countof(g_TestDir))
    {
        skip("No temp path\n");
        return;
    }
    StringCchPrintfW(g_TestDir + Length, _countof(g_TestDir) - Length, L"lftest%lu-%lu",
                     GetCurrentProcessId(), GetTickCount());
    StringCchPrintfW(g_SubDir, _countof(g_SubDir), L"%ls\\sub", g_TestDir);
    StringCchPrintfW(Manifest, _countof(Manifest), L"%ls\\lftest.manifest", g_TestDir);

    if (!CreateDirectoryW(g_TestDir, NULL) || !CreateDirectoryW(g_SubDir, NULL))
    {
        skip("Cannot create %S: %lu\n", g_SubDir, GetLastError());
        RemoveDirectoryW(g_TestDir);
        return;
    }

    SavedDirLength = GetEnvironmentVariableW(DirVariable, SavedDir, _countof(SavedDir));
    SavedNoSuchLength = GetEnvironmentVariableW(NoSuchVariable, SavedNoSuch, _countof(SavedNoSuch));
    ok(SetEnvironmentVariableW(DirVariable, g_TestDir), "SetEnvironmentVariable failed\n");
    SetEnvironmentVariableW(NoSuchVariable, NULL);

    if (WriteManifest(Manifest))
        RunTests(Manifest);
    else
        skip("Cannot write %S: %lu\n", Manifest, GetLastError());

    SetEnvironmentVariableW(DirVariable, (SavedDirLength && SavedDirLength < _countof(SavedDir))
                                         ? SavedDir : NULL);
    if (SavedNoSuchLength && SavedNoSuchLength < _countof(SavedNoSuch))
        SetEnvironmentVariableW(NoSuchVariable, SavedNoSuch);

    DeleteFileW(Manifest);
    RemoveDirectoryW(g_SubDir);
    RemoveDirectoryW(g_TestDir);
}
