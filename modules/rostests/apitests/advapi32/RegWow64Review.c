/* ReactOS registry view regressions. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"
#include <ndk/cmfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/setypes.h>

static void value(HKEY key, DWORD expected)
{
    DWORD data = 0, size = sizeof(data), type = 0;
    LONG error = RegQueryValueExW(key, L"Value", NULL, &type, (BYTE *)&data, &size);
    ok(error == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(data) && data == expected,
       "query: error %ld type %lu size %lu value %lu expected %lu\n", error, type, size, data, expected);
}

static void set_value(HKEY key, DWORD data)
{
    LONG error = RegSetValueExW(key, L"Value", 0, REG_DWORD, (BYTE *)&data, sizeof(data));
    ok(error == ERROR_SUCCESS, "set value: %ld\n", error);
}

static BOOL EndsWithPath(PCWSTR Name, ULONG Length, PCWSTR Path)
{
    UNICODE_STRING Actual, Expected;
    RtlInitUnicodeString(&Expected, Path);
    if (Length < Expected.Length) return FALSE;
    Actual.Buffer = (PWSTR)((const BYTE *)Name + Length - Expected.Length);
    Actual.Length = Actual.MaximumLength = Expected.Length;
    return RtlEqualUnicodeString(&Actual, &Expected, TRUE);
}

START_TEST(RegWow64Unicode)
{
    static const REGSAM views[] = {0, KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    WCHAR path[180], child[32];
    BYTE buffer[1024];
    PKEY_NAME_INFORMATION name = (PKEY_NAME_INFORMATION)buffer;
    HKEY parent, key;
    DWORD i, j, length;
    LONG error;
    NTSTATUS status;
    BOOL wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow64);
    for (i = 0; i < _countof(views); ++i)
    {
        _snwprintf(path, _countof(path), L"Software\\Wow64Unicode_%lu_%lu_", GetCurrentProcessId(), i);
        length = wcslen(path);
        for (j = 0; j < 20; ++j) path[length + j] = i == 0 ? 0xe0 + j : (i == 1 ? 0x400 + j : 0x131);
        path[length + j] = 0;
        error = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0,
                                KEY_ALL_ACCESS | views[i], NULL, &parent, NULL);
        ok(error == ERROR_SUCCESS, "Unicode create view %lx: %ld\n", views[i], error);
        if (error) continue;
        status = NtQueryKey(parent, KeyNameInformation, buffer, sizeof(buffer), &length);
        ok(status == STATUS_SUCCESS, "Unicode name view %lx: %lx\n", views[i], status);
        if (NT_SUCCESS(status))
        {
            ok(EndsWithPath(name->Name, name->NameLength, path),
               "Unicode name contents view %lx, length %lu\n", views[i], name->NameLength);
        }
        if (!wow64)
        {
        status = NtQueryObject(parent, ObjectNameInformation, buffer, sizeof(buffer), &length);
        ok(status == STATUS_SUCCESS, "Unicode object name view %lx: %lx\n", views[i], status);
        if (NT_SUCCESS(status))
        {
            PUNICODE_STRING object = (PUNICODE_STRING)buffer;
            ok(EndsWithPath(object->Buffer, object->Length, path),
               "Unicode object name contents view %lx\n", views[i]);
        }
        }
        wcscpy(child, L"Child_\x6500\x6501\x6502");
        error = RegCreateKeyExW(parent, child, 0, NULL, 0, KEY_ALL_ACCESS | views[i], NULL, &key, NULL);
        ok(error == ERROR_SUCCESS, "Unicode relative create view %lx: %ld\n", views[i], error);
        if (!error)
        {
            set_value(key, 123);
            RegCloseKey(key);
            error = RegOpenKeyExW(parent, child, 0, KEY_READ | views[i], &key);
            ok(error == ERROR_SUCCESS, "Unicode relative open view %lx: %ld\n", views[i], error);
            if (!error) { value(key, 123); RegCloseKey(key); }
            ok(RegDeleteKeyExW(parent, child, views[i], 0) == ERROR_SUCCESS, "Unicode child delete\n");
        }
        RegCloseKey(parent);
        ok(RegDeleteKeyExW(HKEY_CURRENT_USER, path, views[i], 0) == ERROR_SUCCESS, "Unicode parent delete\n");
    }
}

START_TEST(RegWow64Views)
{
    static const REGSAM views[] = {0, KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    SYSTEM_INFO native;
    WCHAR path[160], physical[180], nested[200];
    HKEY roots[2], key, physical_root;
    LONG error;
    DWORD i, j;
    GetNativeSystemInfo(&native);
    /* Every call without a selector is a native behavior control. */
    for (i = 0; i < _countof(views); ++i)
    {
        _snwprintf(path, _countof(path), L"Software\\Wow64Nested_%lu_%lu", GetCurrentProcessId(), i);
        _snwprintf(nested, _countof(nested), L"%ls\\Middle\\Leaf", path);
        error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, nested, 0, NULL, 0,
                               KEY_ALL_ACCESS | views[i], NULL, &key, NULL);
        ok(error == ERROR_SUCCESS, "nested create view %lx: %ld\n", views[i], error);
        if (!error)
        {
            set_value(key, 42); RegCloseKey(key);
            error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, nested, 0, KEY_READ | views[i], &key);
            ok(error == ERROR_SUCCESS, "nested reopen: %ld\n", error);
            if (!error) { value(key, 42); RegCloseKey(key); }
            if (views[i] && native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
            {
                error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0,
                    KEY_READ | (views[i] == KEY_WOW64_32KEY ? KEY_WOW64_64KEY : KEY_WOW64_32KEY), &key);
                ok(error == ERROR_FILE_NOT_FOUND, "nested create left wrong-view parent: %ld\n", error);
                if (!error) RegCloseKey(key);
            }
            ok(RegDeleteKeyExW(HKEY_LOCAL_MACHINE, nested, views[i], 0) == ERROR_SUCCESS, "nested leaf delete\n");
            *wcsrchr(nested, L'\\') = 0;
            ok(RegDeleteKeyExW(HKEY_LOCAL_MACHINE, nested, views[i], 0) == ERROR_SUCCESS, "nested middle delete\n");
        }
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, path, KEY_WOW64_32KEY, 0);
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, path, KEY_WOW64_64KEY, 0);
    }
    if (native.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) return;
    _snwprintf(path, _countof(path), L"Software\\Wow64Physical_%lu", GetCurrentProcessId());
    _snwprintf(physical, _countof(physical), L"Software\\Wow6432Node\\Wow64Physical_%lu", GetCurrentProcessId());
    for (i = 0; i < 2; ++i)
    {
        error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0,
            KEY_ALL_ACCESS | (i ? KEY_WOW64_64KEY : KEY_WOW64_32KEY), NULL, &roots[i], NULL);
        ok(error == ERROR_SUCCESS, "physical setup %lu: %ld\n", i, error);
        if (error) return;
    }
    error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, physical, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &physical_root);
    ok(error == ERROR_SUCCESS, "physical root: %ld\n", error);
    if (!error)
    {
        for (i = 0; i < _countof(views); ++i)
        {
            for (j = 0; j < 2; ++j)
            {
                error = RegCreateKeyExW(roots[j], L"Child", 0, NULL, 0,
                    KEY_ALL_ACCESS | (j ? KEY_WOW64_64KEY : KEY_WOW64_32KEY), NULL, &key, NULL);
                ok(error == ERROR_SUCCESS, "physical child setup: %ld\n", error);
                if (!error) { set_value(key, j ? 64 : 32); RegCloseKey(key); }
            }
            error = RegOpenKeyExW(physical_root, L"Child", 0, KEY_READ | views[i], &key);
            ok(error == ERROR_SUCCESS, "physical relative view %lx: %ld\n", views[i], error);
            if (!error) { value(key, 32); RegCloseKey(key); }
            error = RegDeleteKeyExW(physical_root, L"Child", views[i], 0);
            ok(error == ERROR_SUCCESS, "physical relative delete view %lx: %ld\n", views[i], error);
            error = RegOpenKeyExW(roots[1], L"Child", 0, KEY_READ | KEY_WOW64_64KEY, &key);
            ok(error == ERROR_SUCCESS, "physical delete removed native child: %ld\n", error);
            if (!error) { value(key, 64); RegCloseKey(key); }
            error = RegOpenKeyExW(roots[0], L"Child", 0, KEY_READ | KEY_WOW64_32KEY, &key);
            ok(error == ERROR_FILE_NOT_FOUND, "physical delete left 32 child: %ld\n", error);
            if (!error) RegCloseKey(key);
            RegDeleteKeyExW(roots[0], L"Child", KEY_WOW64_32KEY, 0);
            RegDeleteKeyExW(roots[1], L"Child", KEY_WOW64_64KEY, 0);
        }
        RegCloseKey(physical_root);
    }
    for (i = 0; i < 2; ++i) RegCloseKey(roots[i]);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, path, KEY_WOW64_32KEY, 0);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, path, KEY_WOW64_64KEY, 0);
}

static const struct { PCWSTR Path; BOOL Shared; } Matrix[] = {
    {L"Software", FALSE},
    {L"Software\\Classes", TRUE},
    {L"Software\\Clients", TRUE},
    {L"Software\\Microsoft\\COM3", TRUE},
    {L"Software\\Microsoft\\Cryptography\\Calais\\Current", TRUE},
    {L"Software\\Microsoft\\Cryptography\\Calais\\Readers", TRUE},
    {L"Software\\Microsoft\\Cryptography\\Services", TRUE},
    {L"Software\\Microsoft\\CTF\\SystemShared", TRUE},
    {L"Software\\Microsoft\\CTF\\TIP", TRUE},
    {L"Software\\Microsoft\\DFS", TRUE},
    {L"Software\\Microsoft\\Driver Signing", TRUE},
    {L"Software\\Microsoft\\EnterpriseCertificates", TRUE},
    {L"Software\\Microsoft\\EventSystem", TRUE},
    {L"Software\\Microsoft\\MSMQ", TRUE},
    {L"Software\\Microsoft\\Non-Driver Signing", TRUE},
    {L"Software\\Microsoft\\Notepad\\DefaultFonts", TRUE},
    {L"Software\\Microsoft\\Ole", TRUE},
    {L"Software\\Microsoft\\Ras", TRUE},
    {L"Software\\Microsoft\\Rpc", TRUE},
    {L"Software\\Microsoft\\Software\\Microsoft\\Shared Tools\\Msinfo", TRUE},
    {L"Software\\Microsoft\\SystemCertificates", TRUE},
    {L"Software\\Microsoft\\TermServLicensing", TRUE},
    {L"Software\\Microsoft\\Transaction Server", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\Cursors\\Schemes", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\AutoplayHandlers", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\DriveIcons", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\Group Policy", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\Setup", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\Telephony\\Locations", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Console", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\FontDPI", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\FontLink", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\FontMapper", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\GRE_Initialize", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\LanguagePack", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\NetworkCards", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Perflib", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Ports", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Print", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", TRUE},
    {L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones", TRUE},
    {L"Software\\Policies", TRUE},
    {L"Software\\RegisteredApplications", TRUE},
    {L"Software\\Classes\\CLSID", FALSE},
    {L"Software\\Classes\\DirectShow", FALSE},
    {L"Software\\Classes\\Interface", FALSE},
    {L"Software\\Classes\\Media Type", FALSE},
    {L"Software\\Classes\\MediaFoundation", FALSE},
    {L"Software\\Classes\\AppID", TRUE},
    {L"Software\\Classes\\TypeLib", TRUE},
    {L"Software\\Classes\\Protocols", TRUE},
    {L"Software\\Microsoft\\Windows\\CurrentVersion\\PoliciesSibling", FALSE},
};

START_TEST(RegWow64Matrix)
{
    SYSTEM_INFO native;
    BOOL wow64 = FALSE;
    DWORD i, view, which;
    WCHAR path[300], leaf[80];
    HKEY key[2], parent, opened, root, software32;
    LONG error;
    BOOL shared;
    GetNativeSystemInfo(&native);
    IsWow64Process(GetCurrentProcess(), &wow64);
    _snwprintf(leaf, _countof(leaf), L"Wow64Matrix_%lu", GetCurrentProcessId());
    for (which = 0; which < 3; ++which)
    for (i = 0; i < _countof(Matrix); ++i)
    {
        PCWSTR prefix = Matrix[i].Path;
        if (which && wcsncmp(prefix, L"Software\\Classes", 16)) continue;
        root = which == 0 ? HKEY_LOCAL_MACHINE : (which == 1 ? HKEY_CURRENT_USER : HKEY_CLASSES_ROOT);
        if (which == 2) prefix += prefix[16] ? 17 : 16;
        _snwprintf(path, _countof(path), L"%ls%ls%ls", prefix, *prefix ? L"\\" : L"", leaf);
        shared = Matrix[i].Shared || native.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64;
        trace("matrix root %lu %S shared=%u\n", which, prefix, shared);
        key[0] = key[1] = NULL;
        for (view = 0; view < 2; ++view)
        {
            error = RegCreateKeyExW(root, prefix, 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &parent, NULL);
            ok(error == ERROR_SUCCESS, "matrix native parent %S: %ld\n", prefix, error);
            if (error) break;
            RegCloseKey(parent);
            error = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_ALL_ACCESS |
                (view ? KEY_WOW64_64KEY : KEY_WOW64_32KEY), NULL, &key[view], NULL);
            ok(error == ERROR_SUCCESS, "matrix create %S view %lu: %ld\n", path, view, error);
            if (!error) set_value(key[view], view ? 64 : 32);
        }
        if (key[0] && key[1])
        {
            value(key[0], shared ? 64 : 32);
            value(key[1], 64);
            if (!which &&
                native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
            {
                error = RegOpenKeyExW(root, L"Software", 0, KEY_READ | KEY_WOW64_32KEY, &software32);
                ok(error == ERROR_SUCCESS, "matrix Software32 root: %ld\n", error);
                if (!error)
                {
                    error = RegOpenKeyExW(software32, path + 9, 0, KEY_READ | KEY_WOW64_32KEY, &opened);
                    ok(error == ERROR_SUCCESS, "relative Software32 matrix %S: %ld\n", path, error);
                    if (!error) { value(opened, shared ? 64 : 32); RegCloseKey(opened); }
                    RegCloseKey(software32);
                }
            }
            {
                WCHAR repeated[600];
                DWORD src, dst = 0;
                for (src = 0; path[src]; ++src)
                {
                    repeated[dst++] = path[src];
                    if (path[src] == L'\\') repeated[dst++] = L'\\';
                }
                repeated[dst] = 0;
                for (view = 0; view < 3; ++view)
                {
                    error = RegOpenKeyExW(root, repeated, 0, KEY_READ |
                        (view == 0 ? KEY_WOW64_32KEY : view == 1 ? KEY_WOW64_64KEY : 0), &opened);
                    ok(error == ERROR_SUCCESS, "repeated separators %S view %lu: %ld\n", repeated, view, error);
                    if (!error)
                    {
                        value(opened, shared || view == 1 || (view == 2 && !wow64) ? 64 : 32);
                        RegCloseKey(opened);
                    }
                }
            }
            error = RegOpenKeyExW(root, path, 0, KEY_READ, &opened);
            ok(error == ERROR_SUCCESS, "matrix default open %S: %ld\n", path, error);
            if (!error) { value(opened, shared || !wow64 ? 64 : 32); RegCloseKey(opened); }
        }
        for (view = 0; view < 2; ++view) if (key[view]) RegCloseKey(key[view]);
        RegDeleteKeyExW(root, path, KEY_WOW64_32KEY, 0);
        if (!shared) RegDeleteKeyExW(root, path, KEY_WOW64_64KEY, 0);
    }
}

START_TEST(RegWow64LongName)
{
    WCHAR component[241], path[80];
    HKEY keys[145], root;
    BYTE buffer[1024];
    DWORD i, count = 0, length;
    LONG error;
    NTSTATUS status;
    BOOL wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow64);
    if (wow64)
    {
        skip("Native control builds an ancestry without registry view reconstruction\n");
        return;
    }
    _snwprintf(path, _countof(path), L"Software\\Wow64LongName_%lu", GetCurrentProcessId());
    error = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &root, NULL);
    ok(error == ERROR_SUCCESS, "long-name root: %ld\n", error);
    if (error) return;
    for (i = 0; i < 240; ++i) component[i] = L'a';
    component[240] = 0;
    for (i = 0; i < _countof(keys); ++i)
    {
        error = RegCreateKeyExW(i ? keys[i-1] : root, component, 0, NULL, 0,
                               KEY_ALL_ACCESS, NULL, &keys[i], NULL);
        ok(error == ERROR_SUCCESS, "long-name depth %lu: %ld\n", i, error);
        if (error) break;
        ++count;
    }
    if (count == _countof(keys))
    {
        HKEY middle, leaf;
        error = RegCreateKeyExW(keys[count-1], L"Wow6432Node\\Leaf", 0, NULL, 0,
                               KEY_ALL_ACCESS, NULL, &leaf, NULL);
        ok(error == ERROR_SUCCESS, "native long relative nested create: %ld\n", error);
        if (!error)
        {
            set_value(leaf, 123);
            NtDeleteKey(leaf);
            RegCloseKey(leaf);
            error = RegOpenKeyExW(keys[count-1], L"Wow6432Node", 0, KEY_ALL_ACCESS, &middle);
            ok(error == ERROR_SUCCESS, "native long relative parent: %ld\n", error);
            if (!error) { NtDeleteKey(middle); RegCloseKey(middle); }
        }
        status = NtQueryObject(keys[count-1], ObjectNameInformation, buffer, sizeof(buffer), &length);
        ok(status == STATUS_INSUFFICIENT_RESOURCES, "unrepresentable object name: %lx\n", status);
    }
    while (count)
    {
        --count;
        status = NtDeleteKey(keys[count]);
        ok(status == STATUS_SUCCESS, "long-name delete %lu: %lx\n", count, status);
        RegCloseKey(keys[count]);
    }
    RegCloseKey(root);
    ok(RegDeleteKeyW(HKEY_CURRENT_USER, path) == ERROR_SUCCESS, "long-name root delete\n");
}

START_TEST(RegWow64Volatile)
{
    WCHAR source[80], mount[80], file[MAX_PATH], child[160];
    HKEY seed, parent, key;
    BOOLEAN backup = FALSE, restore = FALSE;
    LONG error;
    DWORD view;
    BOOL wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow64);
    if (wow64)
    {
        skip("Hive save/load is exercised by the native caller fixture\n");
        return;
    }
    ok(RtlAdjustPrivilege(SE_BACKUP_PRIVILEGE, TRUE, FALSE, &backup) == STATUS_SUCCESS, "backup privilege\n");
    ok(RtlAdjustPrivilege(SE_RESTORE_PRIVILEGE, TRUE, FALSE, &restore) == STATUS_SUCCESS, "restore privilege\n");
    _snwprintf(source, _countof(source), L"Software\\Wow64Volatile_%lu", GetCurrentProcessId());
    _snwprintf(mount, _countof(mount), L"Wow64Volatile_%lu", GetCurrentProcessId());
    GetTempPathW(_countof(file), file);
    wcscat(file, L"wow64-volatile-test.hiv");
    DeleteFileW(file);
    error = RegCreateKeyExW(HKEY_CURRENT_USER, source, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &seed, NULL);
    ok(error == ERROR_SUCCESS, "seed key: %ld\n", error);
    if (error) goto privileges;
    error = RegSaveKeyW(seed, file, NULL);
    ok(error == ERROR_SUCCESS, "save seed: %ld\n", error);
    RegCloseKey(seed);
    RegDeleteKeyW(HKEY_CURRENT_USER, source);
    if (error) goto privileges;
    error = RegLoadKeyW(HKEY_USERS, mount, file);
    ok(error == ERROR_SUCCESS, "load fixture hive: %ld\n", error);
    if (error) goto cleanup;
    _snwprintf(child, _countof(child), L"%ls\\Software\\Classes", mount);
    error = RegCreateKeyExW(HKEY_USERS, child, 0, NULL, 0,
                           KEY_ALL_ACCESS, NULL, &parent, NULL);
    ok(error == ERROR_SUCCESS, "stable Classes parent: %ld\n", error);
    if (!error)
    {
        error = RegCreateKeyExW(parent, L"CLSID", 0, NULL, REG_OPTION_VOLATILE,
                               KEY_ALL_ACCESS | KEY_WOW64_32KEY, NULL, &key, NULL);
        ok(error == ERROR_SUCCESS, "stable parent volatile first child: %ld\n", error);
        if (!error) { NtDeleteKey(key); RegCloseKey(key); }
        error = RegCreateKeyExW(parent, L"Interface", 0, NULL, 0,
                               KEY_ALL_ACCESS | KEY_WOW64_32KEY, NULL, &key, NULL);
        ok(error == ERROR_SUCCESS, "stable parent stable second child: %ld\n", error);
        if (!error) { NtDeleteKey(key); RegCloseKey(key); }
        RegDeleteKeyW(parent, L"Wow6432Node");
        NtDeleteKey(parent);
        RegCloseKey(parent);
    }
    _snwprintf(child, _countof(child), L"%ls\\Software\\Classes", mount);
    error = RegCreateKeyExW(HKEY_USERS, child, 0, NULL, REG_OPTION_VOLATILE,
                           KEY_ALL_ACCESS, NULL, &parent, NULL);
    ok(error == ERROR_SUCCESS, "volatile Classes parent: %ld\n", error);
    if (!error)
    {
        for (view = 0; view < 2; ++view)
        {
            error = RegCreateKeyExW(parent, L"CLSID", 0, NULL, REG_OPTION_VOLATILE,
                KEY_ALL_ACCESS | (view ? KEY_WOW64_64KEY : KEY_WOW64_32KEY), NULL, &key, NULL);
            ok(error == ERROR_SUCCESS, "volatile Classes view %lu: %ld\n", view, error);
            if (!error) { NtDeleteKey(key); RegCloseKey(key); }
        }
        error = RegCreateKeyExW(parent, L"Interface", 0, NULL, 0,
                               KEY_ALL_ACCESS | KEY_WOW64_32KEY, NULL, &key, NULL);
        ok(error == ERROR_CHILD_MUST_BE_VOLATILE, "volatile parent stable child: %ld\n", error);
        if (!error) { NtDeleteKey(key); RegCloseKey(key); }
        RegDeleteKeyW(parent, L"Wow6432Node");
        NtDeleteKey(parent);
        RegCloseKey(parent);
    }
    error = RegUnLoadKeyW(HKEY_USERS, mount);
    ok(error == ERROR_SUCCESS, "unload fixture hive: %ld\n", error);
cleanup:
    DeleteFileW(file);
privileges:
    RtlAdjustPrivilege(SE_BACKUP_PRIVILEGE, backup, FALSE, &backup);
    RtlAdjustPrivilege(SE_RESTORE_PRIVILEGE, restore, FALSE, &restore);
}


/* Start with an absent shared boundary, so intermediate creation cannot be
 * hidden by a native parent prepared by the matrix fixture. */
START_TEST(RegWow64SharedCreate)
{
    static const REGSAM views[] = {KEY_WOW64_32KEY, 0, KEY_WOW64_64KEY};
    SYSTEM_INFO native;
    HKEY root, key, opened;
    WCHAR path[180];
    DWORD i, relative, disposition, forms;
    LONG error;
    GetNativeSystemInfo(&native);
    forms = native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? 4 : 2;
#ifdef _WIN64
    forms = 6; /* Include roots duplicated with zero granted access. */
#endif
    for (relative = 0; relative < forms; ++relative)
    for (i = 0; i < _countof(views); ++i)
    {
        error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Notepad", 0,
                              KEY_READ | KEY_WOW64_64KEY, &key);
        ok(error == ERROR_FILE_NOT_FOUND, "native Notepad must be absent: %ld\n", error);
        if (!error) { RegCloseKey(key); return; }
        if (native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
        {
            error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Microsoft\\Notepad", 0,
                                  KEY_READ | KEY_WOW64_64KEY, &key);
            ok(error == ERROR_FILE_NOT_FOUND, "physical Notepad must be absent: %ld\n", error);
            if (!error) { RegCloseKey(key); return; }
        }
        root = HKEY_LOCAL_MACHINE;
        if ((relative & 1) || relative >= 4)
        {
            error = RegOpenKeyExW(root, relative == 3 || relative == 5 ? L"Software\\Wow6432Node\\Microsoft" : L"Software\\Microsoft", 0,
                                  KEY_ALL_ACCESS | KEY_WOW64_64KEY, &root);
            ok(error == ERROR_SUCCESS, "relative Microsoft root: %ld\n", error);
            if (error) return;
#ifdef _WIN64
            if (relative >= 4)
            {
                HKEY duplicate = NULL;
                NTSTATUS status = NtDuplicateObject(NtCurrentProcess(), root, NtCurrentProcess(),
                                                     (PHANDLE)&duplicate, 0, 0, 0);
                ok(status == STATUS_SUCCESS, "zero-access root duplicate: %lx\n", status);
                RegCloseKey(root);
                if (!NT_SUCCESS(status)) return;
                root = duplicate;
            }
#endif
        }
        wcscpy(path, ((relative & 1) || relative >= 4) ? L"Notepad\\DefaultFonts\\Review\\Leaf" :
                     relative == 2 ? L"Software\\Wow6432Node\\Microsoft\\Notepad\\DefaultFonts\\Review\\Leaf" :
                                     L"Software\\Microsoft\\Notepad\\DefaultFonts\\Review\\Leaf");
        error = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_ALL_ACCESS | views[i],
                                NULL, &key, &disposition);
        ok(error == ERROR_SUCCESS, "shared nested create relative %lu view %lx: %ld\n", relative, views[i], error);
        if (!error)
        {
            ok(disposition == REG_CREATED_NEW_KEY, "shared nested disposition %lu\n", disposition);
            set_value(key, 123);
            RegCloseKey(key);
            error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"Software\\Microsoft\\Notepad\\DefaultFonts\\Review\\Leaf", 0,
                KEY_READ | KEY_WOW64_64KEY, &opened);
            ok(error == ERROR_SUCCESS, "shared nested native leaf: %ld\n", error);
            if (!error) { value(opened, 123); RegCloseKey(opened); }
        }
        if ((relative & 1) || relative >= 4) RegCloseKey(root);
        if (native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
        {
            error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Microsoft\\Notepad", 0,
                                  KEY_READ | KEY_WOW64_64KEY, &opened);
            ok(error == ERROR_FILE_NOT_FOUND, "shared create left wrong-view parent: %ld\n", error);
            if (!error) RegCloseKey(opened);
            RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Microsoft\\Notepad", KEY_WOW64_64KEY, 0);
        }
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Notepad\\DefaultFonts\\Review\\Leaf", KEY_WOW64_64KEY, 0);
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Notepad\\DefaultFonts\\Review", KEY_WOW64_64KEY, 0);
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Notepad\\DefaultFonts", KEY_WOW64_64KEY, 0);
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Notepad", KEY_WOW64_64KEY, 0);
    }
}

START_TEST(RegWow64Aliases)
{
    static const REGSAM views[] = {0, KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    SYSTEM_INFO native;
    BOOL wow64 = FALSE, aliased;
    WCHAR logical[200], physical[240], leaf[80];
    HKEY root, key, opened, container;
    DWORD which, view, disposition;
    LONG error;
    GetNativeSystemInfo(&native);
    IsWow64Process(GetCurrentProcess(), &wow64);
    _snwprintf(leaf, _countof(leaf), L"ReviewAlias_%lu", GetCurrentProcessId());
    for (which = 0; which < 3; ++which)
    {
        root = which == 1 ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        _snwprintf(logical, _countof(logical), L"%ls\\%ls",
            which == 2 ? L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies" : L"Software\\Classes", leaf);
        _snwprintf(physical, _countof(physical), L"%ls\\%ls",
            which == 2 ? L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Policies" :
                         L"Software\\Classes\\Wow6432Node", leaf);
        error = RegCreateKeyExW(root, logical, 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &key, NULL);
        ok(error == ERROR_SUCCESS, "alias logical create %lu: %ld\n", which, error);
        if (error) continue;
        set_value(key, 64);
        for (view = 0; view < _countof(views); ++view)
        {
            aliased = native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 &&
                      (which == 2 || views[view] == KEY_WOW64_32KEY || (!views[view] && wow64));
            error = RegOpenKeyExW(root, physical, 0, KEY_READ | views[view], &opened);
            ok(error == (aliased ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND),
               "physical alias %lu view %lx: %ld expected alias %u\n", which, views[view], error, aliased);
            if (!error) { value(opened, 64); RegCloseKey(opened); }
            if (!aliased) continue;
            error = RegCreateKeyExW(root, physical, 0, NULL, 0, KEY_ALL_ACCESS | views[view], NULL, &opened, &disposition);
            ok(error == ERROR_SUCCESS, "alias create %lu view %lx: %ld\n", which, views[view], error);
            if (!error)
            {
                ok(disposition == REG_OPENED_EXISTING_KEY, "alias created divergent key: %lu\n", disposition);
                set_value(opened, 32);
                value(key, 32);
                set_value(key, 64);
                RegCloseKey(opened);
            }
        }
        if (which != 2 && native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
        {
            error = RegCreateKeyExW(root, L"Software\\Classes\\Wow6432Node", 0, NULL, 0,
                                    KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &container, NULL);
            ok(error == ERROR_SUCCESS, "physical Classes container: %ld\n", error);
            if (!error)
            {
                error = RegOpenKeyExW(container, leaf, 0, KEY_READ | KEY_WOW64_32KEY, &opened);
                ok(error == ERROR_SUCCESS, "relative physical Classes alias: %ld\n", error);
                if (!error) { value(opened, 64); RegCloseKey(opened); }
                RegCloseKey(container);
            }
        }
        NtDeleteKey(key);
        RegCloseKey(key);
        /* Clean a divergent key left by the before image, if any. */
        RegDeleteKeyExW(root, physical, KEY_WOW64_64KEY, 0);
    }
}
