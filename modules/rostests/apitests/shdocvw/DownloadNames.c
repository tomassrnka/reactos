/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the file names and files of the browser's downloads (ieframe download_name.c)
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include <apitest.h>
#include <shlwapi.h>

#include "download_name.h"

NTSYSAPI ULONG NTAPI RtlIsDosDeviceName_U(PCWSTR Name);

static void fill(WCHAR *p, WCHAR c, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        p[i] = c;
    p[n] = 0;
}

static void test_snprintf(void)
{
    WCHAR buf[8];
    int len;

    memset(buf, 0x55, sizeof(buf));
    len = dl_snprintf(buf, ARRAY_SIZE(buf), L"%s", L"0123456789");
    ok_int(len, 7);
    ok(!wcscmp(buf, L"0123456"), "got %S\n", buf);

    len = dl_snprintf(buf, ARRAY_SIZE(buf), L"%u.%s", 12, L"ab");
    ok_int(len, 5);
    ok(!wcscmp(buf, L"12.ab"), "got %S\n", buf);
}

static void test_find_header(void)
{
    static const WCHAR headers[] = L"HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\ncontent-disposition:\t attachment; filename=a.zip\r\n"
                                   L"X-Content-Disposition: no\r\nContent-Length: 12\r\n\r\n";
    WCHAR value[64];

    ok(dl_find_header(headers, L"Content-Disposition", value, ARRAY_SIZE(value)), "header not found\n");
    ok(!wcscmp(value, L"attachment; filename=a.zip"), "got %S\n", value);
    ok(dl_find_header(headers, L"Content-Length", value, ARRAY_SIZE(value)), "header not found\n");
    ok(!wcscmp(value, L"12"), "got %S\n", value);
    ok(!dl_find_header(headers, L"Content-Encoding", value, ARRAY_SIZE(value)), "found a header that is not there\n");
    ok(!dl_find_header(headers, L"Content-Type", value, 10), "found a value that does not fit\n");
    ok(!dl_find_header(headers, L"Disposition", value, ARRAY_SIZE(value)), "found part of a name\n");
}

static void test_disposition(void)
{
    static const struct {
        const WCHAR *value;
        const WCHAR *name;      /* NULL: names no file */
    } tests[] = {
        { L"attachment; filename=\"a.txt\"", L"a.txt" },
        { L"attachment; filename=a.txt", L"a.txt" },
        { L"attachment;filename=a.txt;size=3", L"a.txt" },
        { L"attachment; filename = \"spaced name.zip\" ; x=y", L"spaced name.zip" },
        { L"ATTACHMENT; FILENAME=\"X.TXT\"", L"X.TXT" },
        { L"inline; filename*=UTF-8''%E2%82%AC%20rates.txt", L"\x20ac rates.txt" },
        { L"attachment; filename=\"plain.txt\"; filename*=UTF-8''better.txt", L"better.txt" },
        { L"attachment; filename*=UTF-8''better.txt; filename=\"plain.txt\"", L"better.txt" },
        { L"attachment; filename*=ISO-8859-1''%E9.txt", L"\xe9.txt" },
        { L"attachment; filename*=iso-8859-2''x.txt", NULL },
        { L"attachment; filename*=iso-8859-2''x.txt; filename=y.txt", L"y.txt" },
        { L"attachment", NULL },
        { L"attachment; filename=", NULL },
        { L"attachment; filename=\"\"", NULL },
        { L"attachment; note=\"x\"", NULL },
        { L"attachment; note=\"filename=evil.exe\"; filename=ok.txt", L"ok.txt" },
        { L"attachment; filename=\"a\\\"b.txt\"", L"a\"b.txt" },
        { L"attachment; filename=\"a\\\\b.txt\"", L"a\\b.txt" },
        { L"attachment; filename=\"..\\..\\evil.txt\"", L"..\\..\\evil.txt" },
        { L"attachment; filename=\"\xc3\xa9.txt\"", L"\xe9.txt" },         /* UTF-8 bytes, one per character */
        { L"attachment; filename=\"\xe9.txt\"", L"\xe9.txt" },             /* not UTF-8: kept */
        { L"attachment; filename=\"\x20ac.txt\"", L"\x20ac.txt" },
        { L"attachment; filename*=UTF-8''..%2F..%5Cevil.txt", L"../..\\evil.txt" },
        { L"attachment; filename*=UTF-8''a%00b.txt", L"a_b.txt" },
        { L"attachment; filename*=UTF-8''%", L"%" },
        { L"attachment; filename*=UTF-8''%4", L"%4" },
        { L"attachment; filename*=UTF-8''no-end%2", L"no-end%2" },
    };
    WCHAR name[300];
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        BOOL ret = dl_name_from_disposition(tests[i].value, name, ARRAY_SIZE(name));

        if (tests[i].name)
        {
            ok(ret, "%u: no name for %S\n", i, tests[i].value);
            ok(!wcscmp(name, tests[i].name), "%u: %S gave %S, expected %S\n", i, tests[i].value, name, tests[i].name);
        }
        else
        {
            ok(!ret, "%u: %S gave the name %S\n", i, tests[i].value, name);
        }
    }

    /* the buffer is the limit */
    ok(dl_name_from_disposition(L"attachment; filename=\"0123456789\"", name, 6), "no name\n");
    ok(!wcscmp(name, L"01234"), "got %S\n", name);
}

static void test_split_url(void)
{
    static const struct {
        const WCHAR *url;
        const WCHAR *host;
        const WCHAR *name;
    } tests[] = {
        { L"http://host/file.exe", L"host", L"file.exe" },
        { L"http://user:secret@host:8080/dir/file%20name.zip?x=1#frag", L"host:8080", L"file name.zip" },
        { L"http://host/", L"host", L"" },
        { L"http://host", L"host", L"" },
        { L"http://host/a/b.tar.gz", L"host", L"b.tar.gz" },
        { L"http://host/file.exe?cd=att", L"host", L"file.exe" },
        { L"http://host/file.exe#a?b", L"host", L"file.exe" },
        { L"http://host/%E2%82%AC.txt", L"host", L"\x20ac.txt" },
        { L"http://host/%FF.bin", L"host", L"\xff.bin" },
        { L"http://host/a%2Fb.txt", L"host", L"a/b.txt" },
        { L"http://host/%00x.exe", L"host", L"_x.exe" },
        { L"http://host/dir\\file.exe", L"host", L"file.exe" },
        { L"http://host/100%.bin", L"host", L"100%.bin" },
        { L"http://host/%4", L"host", L"%4" },
        { L"http://a@b@host/f.bin", L"host", L"f.bin" },
    };
    WCHAR host[64], name[64];
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        dl_split_url(tests[i].url, host, ARRAY_SIZE(host), name, ARRAY_SIZE(name));
        ok(!wcscmp(host, tests[i].host), "%u: %S host %S, expected %S\n", i, tests[i].url, host, tests[i].host);
        ok(!wcscmp(name, tests[i].name), "%u: %S name %S, expected %S\n", i, tests[i].url, name, tests[i].name);
    }

    dl_split_url(L"http://averyveryverylonghostname/averyverylongfilename.bin", host, 8, name, 10);
    ok(!wcscmp(host, L"averyve"), "got %S\n", host);
    ok(!wcscmp(name, L"averyvery"), "got %S\n", name);
}

static void test_make_leaf(void)
{
    static const struct {
        const WCHAR *in;
        const WCHAR *out;
    } tests[] = {
        { L"a.txt", L"a.txt" },
        { L"dir\\sub/x.txt", L"x.txt" },
        { L"..\\..\\evil.txt", L"evil.txt" },
        { L"/etc/passwd", L"passwd" },
        { L"a:b.txt", L"a_b.txt" },
        { L"x::$DATA", L"x__$DATA" },
        { L"con:", L"con_" },
        { L"x<>|?*\"y.txt", L"x______y.txt" },
        { L"tab\there.txt", L"tab_here.txt" },
        { L"evil\x202egnp.exe", L"evil_gnp.exe" },
        { L"a\x200f\x2066.txt", L"a__.txt" },
        { L"name. . ", L"name" },
        { L"name.", L"name" },
        { L"...", L"download" },
        { L"..", L"download" },
        { L".", L"download" },
        { L"", L"download" },
        { L"dir\\", L"download" },
        { L" lead.txt", L" lead.txt" },
        { L".htaccess", L".htaccess" },
        { L"con.txt", L"con.txt" },
        { L"two.dots.tar.gz", L"two.dots.tar.gz" },
        { L"\x20ac.txt", L"\x20ac.txt" },
    };
    WCHAR name[1200], expected[1200];
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        wcscpy(name, tests[i].in);
        dl_make_leaf(name);
        ok(!wcscmp(name, tests[i].out), "%u: %S gave %S, expected %S\n", i, tests[i].in, name, tests[i].out);
    }

    /* a name of 211 characters (100 + dot + 110) is kept whole */
    fill(name, L'a', 100);
    name[100] = L'.';
    fill(name + 101, L'e', 110);
    wcscpy(expected, name);
    dl_make_leaf(name);
    ok(!wcscmp(name, expected), "the whole name was not kept (%u characters)\n", (unsigned)wcslen(name));

    /* too long: the base goes, the extension stays */
    fill(name, L'b', 600);
    wcscat(name, L".ext");
    dl_make_leaf(name);
    ok_int((int)wcslen(name), DL_LEAF_MAX);
    ok(!wcscmp(name + DL_LEAF_MAX - 4, L".ext"), "extension lost: %S\n", name + wcslen(name) - 6);

    /* an extension that is itself too long is cut */
    wcscpy(name, L"b.");
    fill(name + 2, L'e', 600);
    dl_make_leaf(name);
    ok_int((int)wcslen(name), DL_LEAF_MAX);

    /* exactly the limit stays */
    fill(name, L'c', DL_LEAF_MAX - 4);
    wcscat(name, L".txt");
    wcscpy(expected, name);
    dl_make_leaf(name);
    ok(!wcscmp(name, expected), "a name of the limit changed\n");
    ok_int((int)wcslen(name), DL_LEAF_MAX);

    /* one more character loses a character of the base */
    fill(name, L'c', DL_LEAF_MAX - 3);
    wcscat(name, L".txt");
    dl_make_leaf(name);
    ok_int((int)wcslen(name), DL_LEAF_MAX);
    ok(!wcscmp(name + DL_LEAF_MAX - 4, L".txt"), "extension lost\n");
}

static void make_dir(WCHAR *dir, size_t length)
{
    size_t i;

    wcscpy(dir, L"C:\\");
    for (i = 3; i < length - 1; i++)
        dir[i] = L'd';
    dir[length - 1] = L'\\';
    dir[length] = 0;
}

static void test_fit_leaf(void)
{
    WCHAR dir[MAX_PATH], name[600];
    size_t len, room;

    /* short names behind short folders are not touched */
    wcscpy(name, L"file.bin");
    ok(dl_fit_leaf(L"C:\\Temp\\", name, 0), "no room\n");
    ok(!wcscmp(name, L"file.bin"), "got %S\n", name);

    /* every folder length: the name is kept whole when it fits and cut, with its extension, when it does not */
    for (len = 3; len < 250; len += 7)
    {
        static const WCHAR ext[] = L".exttype";
        size_t name_len;

        make_dir(dir, len);
        room = MAX_PATH - 1 - len;

        for (name_len = 20; name_len <= DL_LEAF_MAX; name_len += 17)
        {
            size_t i;
            BOOL ret;

            for (i = 0; i < name_len - 8; i++)
                name[i] = L'n';
            name[i] = 0;
            wcscat(name, ext);

            ret = dl_fit_leaf(dir, name, 0);
            if (name_len <= room)
            {
                ok(ret, "dir %u name %u: no room\n", (unsigned)len, (unsigned)name_len);
                ok_int((int)wcslen(name), (int)name_len);
            }
            else if (room >= 9)
            {
                ok(ret, "dir %u name %u: no room\n", (unsigned)len, (unsigned)name_len);
                ok_int((int)wcslen(name), (int)room);
                ok(!wcscmp(name + wcslen(name) - 8, ext), "dir %u name %u: extension lost: %S\n", (unsigned)len, (unsigned)name_len, name);
            }
            if (ret)
                ok(len + wcslen(name) < MAX_PATH, "dir %u name %u: path of %u characters\n", (unsigned)len, (unsigned)name_len, (unsigned)(len + wcslen(name)));
        }
    }

    /* the reserve is kept free */
    make_dir(dir, 200);
    fill(name, L'a', 110);
    name[110] = L'.';
    fill(name + 111, L'a', 140);
    ok(dl_fit_leaf(dir, name, 6), "no room\n");
    ok(200 + wcslen(name) + 6 < MAX_PATH, "no room kept for the number\n");

    /* an extension that leaves no room for the base is cut itself */
    make_dir(dir, 245);
    wcscpy(name, L"b.123456789012345678901234567890");
    ok(dl_fit_leaf(dir, name, 0), "no room\n");
    ok(245 + wcslen(name) < MAX_PATH, "too long: %u\n", (unsigned)wcslen(name));

    /* no room at all */
    make_dir(dir, 258);
    wcscpy(name, L"a.txt");
    ok(!dl_fit_leaf(dir, name, 0) || 258 + wcslen(name) < MAX_PATH, "name does not fit\n");
    make_dir(dir, 259);
    wcscpy(name, L"a.txt");
    ok(!dl_fit_leaf(dir, name, 0), "a folder of 259 characters leaves room\n");
    make_dir(dir, 200);
    wcscpy(name, L"a.txt");
    ok(!dl_fit_leaf(dir, name, 70), "a reserve of 70 leaves room\n");
}

static BOOL is_device(const WCHAR *dir, const WCHAR *name)
{
    WCHAR path[MAX_PATH];

    wcscpy(path, dir);
    wcscat(path, name);
    return RtlIsDosDeviceName_U(path) != 0;
}

static void test_unmask_device(void)
{
    static const struct {
        const WCHAR *in;
        const WCHAR *out;
    } tests[] = {
        { L"NUL", L"NU_" },
        { L"nul.txt", L"nu_.txt" },
        { L"CON", L"CO_" },
        { L"PRN.log", L"PR_.log" },
        { L"AUX", L"AU_" },
        { L"COM1", L"COM_" },
        { L"COM1.exe", L"COM_.exe" },
        { L"com9.a.b", L"com_.a.b" },
        { L"LPT1", L"LPT_" },
        { L"LPT9.x", L"LPT_.x" },
        { L"CON .txt", L"CON_.txt" },
        { L"NULL", L"NULL" },
        { L"NUL1", L"NUL1" },
        { L"NU", L"NU" },
        { L"COM", L"COM" },
        { L"COM10", L"COM10" },
        { L"LPT10.txt", L"LPT10.txt" },
        { L"CONSOLE.txt", L"CONSOLE.txt" },
        { L"xNUL", L"xNUL" },
        { L"a.NUL", L"a.NUL" },
        { L"file.txt", L"file.txt" },
        { L".NUL", L".NUL" },
    };
    static const WCHAR *dirs[] = { L"C:\\", L"C:\\Temp\\", L"C:\\a\\b\\c\\", L"\\\\server\\share\\dir\\" };
    unsigned i, d;

    for (d = 0; d < ARRAY_SIZE(dirs); d++)
    {
        for (i = 0; i < ARRAY_SIZE(tests); i++)
        {
            WCHAR name[64];

            wcscpy(name, tests[i].in);
            dl_unmask_device(dirs[d], name);
            ok(!wcscmp(name, tests[i].out), "%S%S gave %S, expected %S\n", dirs[d], tests[i].in, name, tests[i].out);
            ok(!is_device(dirs[d], name), "%S%S still opens a device\n", dirs[d], name);
            /* the name may end up on a local drive whatever folder was meant */
            ok(!is_device(L"C:\\", name), "C:\\%S opens a device\n", name);
        }
    }
}

static void test_path_opens_device(void)
{
    ok(dl_path_opens_device(L"C:\\dir\\NUL.txt"), "NUL.txt\n");
    ok(dl_path_opens_device(L"C:\\COM1"), "COM1\n");
    ok(!dl_path_opens_device(L"C:\\dir\\NUL1.txt"), "NUL1.txt\n");
    ok(!dl_path_opens_device(L"C:\\dir\\file.txt"), "file.txt\n");
    ok(!dl_path_opens_device(L"C:\\NUL\\file.txt"), "a folder named NUL\n");
}

/* the folders of the file tests */
static WCHAR g_root[MAX_PATH];

static BOOL make_folder(WCHAR *folder, size_t length)
{
    size_t n = 0;

    wcscpy(folder, g_root);
    while (wcslen(folder) < length)
    {
        size_t left = length - wcslen(folder), part, i;
        WCHAR *end = folder + wcslen(folder);

        if (left < 3)
            return FALSE;
        part = left - 1;
        if (part > 40)
        {
            part = 40;
            if (left - 41 < 3)
                part = left - 4;
        }
        end[0] = L's';
        end[1] = L'0' + (n++ % 10);
        for (i = 2; i < part; i++)
            end[i] = L'x';
        end[part] = L'\\';
        end[part + 1] = 0;
        if (!CreateDirectoryW(folder, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return FALSE;
    }
    return wcslen(folder) == length;
}

static void remove_folders(const WCHAR *folder)
{
    WCHAR path[MAX_PATH];
    WIN32_FIND_DATAW find;
    HANDLE handle;
    size_t len;

    wcscpy(path, folder);
    len = wcslen(path);
    wcscat(path, L"*");
    handle = FindFirstFileW(path, &find);
    if (handle != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(find.cFileName, L".") || !wcscmp(find.cFileName, L".."))
                continue;
            path[len] = 0;
            wcscat(path, find.cFileName);
            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                wcscat(path, L"\\");
                remove_folders(path);
            }
            else
            {
                SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path);
            }
        } while (FindNextFileW(handle, &find));
        FindClose(handle);
    }
    path[len] = 0;
    if (len > wcslen(g_root))
    {
        path[len - 1] = 0;
        RemoveDirectoryW(path);
    }
}

static BOOL file_has(const WCHAR *path, DWORD size)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    WCHAR extended[MAX_PATH + 8];

    /* the extended form names a file, never a device */
    swprintf(extended, ARRAY_SIZE(extended), L"\\\\?\\%s", path);
    return GetFileAttributesExW(extended, GetFileExInfoStandard, &data) && !data.nFileSizeHigh && data.nFileSizeLow == size;
}

static HANDLE create_unique(const WCHAR *dir, const WCHAR *leaf, WCHAR *path, DWORD size)
{
    HANDLE file = dl_create_unique_file(dir, leaf, path);
    DWORD written;

    if (file != INVALID_HANDLE_VALUE)
    {
        ok(WriteFile(file, "abcdefghijklmnopqrstuvwxyz", size, &written, NULL) && written == size, "write failed: %lu\n", GetLastError());
        CloseHandle(file);
    }
    return file;
}

static void test_unique_file(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH], expected[MAX_PATH], other[MAX_PATH];
    HANDLE file;
    DWORD written;

    swprintf(dir, ARRAY_SIZE(dir), L"%ssimple\\", g_root);
    ok(CreateDirectoryW(dir, NULL), "cannot create %S: %lu\n", dir, GetLastError());

    /* an existing file stays; the next name keeps the extension */
    swprintf(other, ARRAY_SIZE(other), L"%skeep.exe", dir);
    file = CreateFileW(other, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(file != INVALID_HANDLE_VALUE, "cannot create %S\n", other);
    WriteFile(file, "old", 3, &written, NULL);
    CloseHandle(file);

    file = create_unique(dir, L"keep.exe", path, 5);
    ok(file != INVALID_HANDLE_VALUE, "no file: %lu\n", GetLastError());
    swprintf(expected, ARRAY_SIZE(expected), L"%skeep (1).exe", dir);
    ok(!wcscmp(path, expected), "got %S, expected %S\n", path, expected);
    ok(file_has(expected, 5), "new file has the wrong size\n");
    ok(file_has(other, 3), "the existing file changed\n");

    file = create_unique(dir, L"keep.exe", path, 6);
    swprintf(expected, ARRAY_SIZE(expected), L"%skeep (2).exe", dir);
    ok(file != INVALID_HANDLE_VALUE && !wcscmp(path, expected), "got %S, expected %S\n", path, expected);

    /* a name without an extension */
    file = create_unique(dir, L"plain", path, 1);
    swprintf(expected, ARRAY_SIZE(expected), L"%splain", dir);
    ok(file != INVALID_HANDLE_VALUE && !wcscmp(path, expected), "got %S\n", path);
    file = create_unique(dir, L"plain", path, 1);
    swprintf(expected, ARRAY_SIZE(expected), L"%splain (1)", dir);
    ok(file != INVALID_HANDLE_VALUE && !wcscmp(path, expected), "got %S\n", path);

    /* a device name becomes a file */
    file = create_unique(dir, L"NUL.txt", path, 7);
    swprintf(expected, ARRAY_SIZE(expected), L"%sNU_.txt", dir);
    ok(file != INVALID_HANDLE_VALUE && !wcscmp(path, expected), "got %S\n", path);
    ok(file_has(expected, 7), "the data did not go to a file\n");
    swprintf(other, ARRAY_SIZE(other), L"%sNUL.txt", dir);
    ok(GetFileAttributesW(other) == INVALID_FILE_ATTRIBUTES || !file_has(other, 7), "a file NUL.txt was written\n");
    file = create_unique(dir, L"COM1", path, 2);
    swprintf(expected, ARRAY_SIZE(expected), L"%sCOM_", dir);
    ok(file != INVALID_HANDLE_VALUE && !wcscmp(path, expected), "got %S\n", path);
    ok(file_has(expected, 2), "the data did not go to a file\n");

    /* the number of a taken name does not make a device name either; the name below is no device name as it is */
    file = create_unique(dir, L"NUL.txt", path, 7);
    swprintf(expected, ARRAY_SIZE(expected), L"%sNU_.txt", dir);
    ok(file != INVALID_HANDLE_VALUE && wcscmp(path, expected), "the name NU_.txt was used twice: %S\n", path);
    ok(file_has(path, 7), "the data did not go to a file\n");

    remove_folders(dir);
}

/* a name of 255 characters (the limit of one name) gets its number from the base name, and stays within the limit */
static void test_unique_file_limit(void)
{
    WCHAR dir[4] = { g_root[0], L':', L'\\', 0 }, leaf[300], first[MAX_PATH], second[MAX_PATH];
    HANDLE file;

    fill(leaf, L'a', 251);
    wcscpy(leaf + 251, L".exe");
    ok_int((int)wcslen(leaf), DL_LEAF_MAX);

    file = dl_create_unique_file(dir, leaf, first);
    if (file == INVALID_HANDLE_VALUE)
    {
        skip("cannot create a file in %S: %lu\n", dir, GetLastError());
        return;
    }
    CloseHandle(file);
    ok(!wcscmp(first + 3, leaf), "got %S\n", first);

    file = dl_create_unique_file(dir, leaf, second);
    ok(file != INVALID_HANDLE_VALUE, "no second file: %lu\n", GetLastError());
    if (file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file);
        ok(wcslen(second + 3) <= DL_LEAF_MAX, "the name has %u characters\n", (unsigned)wcslen(second + 3));
        ok(!wcscmp(second + wcslen(second) - 8, L" (1).exe"), "got %S\n", second + wcslen(second) - 12);
        DeleteFileW(second);
    }
    DeleteFileW(first);
}

static void test_unique_file_exhausted(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH], name[MAX_PATH];
    HANDLE file;
    unsigned i;

    swprintf(dir, ARRAY_SIZE(dir), L"%sfull\\", g_root);
    ok(CreateDirectoryW(dir, NULL), "cannot create %S: %lu\n", dir, GetLastError());

    for (i = 0; i < 1000; i++)
    {
        if (i)
            swprintf(name, ARRAY_SIZE(name), L"%sx (%u).bin", dir, i);
        else
            swprintf(name, ARRAY_SIZE(name), L"%sx.bin", dir);
        file = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            ok(0, "cannot create %S: %lu\n", name, GetLastError());
            break;
        }
        CloseHandle(file);
    }

    SetLastError(0xdeadbeef);
    file = dl_create_unique_file(dir, L"x.bin", path);
    ok(file == INVALID_HANDLE_VALUE, "a file was created: %S\n", path);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    ok_int(GetLastError(), ERROR_FILE_EXISTS);

    remove_folders(dir);
}

/* names that are shortened to fit a deep folder: the extension stays and no device name is left over */
static void test_unique_file_deep(void)
{
    static const char *devices[] = { "NUL", "COM1", "CON", "LPT3" };
    WCHAR folder[MAX_PATH], path[MAX_PATH], leaf[MAX_PATH], expected[MAX_PATH];
    size_t length, d, i, base_room;

    for (length = 248; length >= 236; length--)
    {
        if (make_folder(folder, length))
            break;
        remove_folders(g_root);
    }
    if (length < 236)
    {
        skip("cannot make a deep folder\n");
        return;
    }

    for (d = 0; d < ARRAY_SIZE(devices); d++)
    {
        size_t dev_len = strlen(devices[d]);
        WCHAR ext[MAX_PATH];
        HANDLE file;

        /* the base has exactly dev_len characters left: the cut name is the device name */
        base_room = dev_len;
        ext[0] = L'.';
        for (i = 1; i < MAX_PATH - 1 - length - base_room; i++)
            ext[i] = L'e';
        ext[i] = 0;

        swprintf(leaf, ARRAY_SIZE(leaf), L"%Spayload%s", devices[d], ext);
        file = create_unique(folder, leaf, path, 4);
        ok(file != INVALID_HANDLE_VALUE, "%S: no file: %lu\n", devices[d], GetLastError());
        if (file == INVALID_HANDLE_VALUE)
            continue;
        ok(wcslen(path) < MAX_PATH, "path too long\n");
        ok(wcslen(path) >= wcslen(ext) && !wcscmp(path + wcslen(path) - wcslen(ext), ext), "%S: extension lost: %S\n", devices[d], path);
        ok(!is_device(folder, path + length), "%S: %S opens a device\n", devices[d], path + length);
        ok(file_has(path, 4), "%S: the data did not go to a file\n", devices[d]);
        swprintf(expected, ARRAY_SIZE(expected), L"%s%.*S_%s", folder, (int)dev_len - 1, devices[d], ext);
        ok(!wcscmp(path, expected), "got %S, expected %S\n", path, expected);
    }

    /* a name that fits is not cut */
    swprintf(leaf, ARRAY_SIZE(leaf), L"short.bin");
    {
        HANDLE file = create_unique(folder, leaf, path, 1);

        swprintf(expected, ARRAY_SIZE(expected), L"%sshort.bin", folder);
        ok(file != INVALID_HANDLE_VALUE && !wcscmp(path, expected), "got %S\n", path);
    }

    /* the number is made room for, and the path stays inside the limit */
    {
        HANDLE file;
        WCHAR full[MAX_PATH];
        size_t room = MAX_PATH - 1 - length;

        for (i = 0; i < room; i++)
            full[i] = L'f';
        full[i] = 0;
        wcscpy(full + room - 4, L".bin");
        for (i = 0; i < 3; i++)
        {
            file = create_unique(folder, full, path, 1);
            ok(file != INVALID_HANDLE_VALUE, "%u: no file: %lu\n", (unsigned)i, GetLastError());
            if (file != INVALID_HANDLE_VALUE)
                ok(wcslen(path) < MAX_PATH && !wcscmp(path + wcslen(path) - 4, L".bin"), "%u: got %S\n", (unsigned)i, path);
        }
    }

    remove_folders(g_root);
    CreateDirectoryW(g_root, NULL);
}

static void test_temp_dir(void)
{
    WCHAR dir[MAX_PATH], old_tmp[MAX_PATH], old_temp[MAX_PATH], huge[400];
    BOOL have_tmp, have_temp, ret;
    unsigned i;

    ok(dl_get_temp_dir(dir), "no temporary folder\n");
    ok(dir[wcslen(dir) - 1] == L'\\', "no closing backslash: %S\n", dir);

    have_tmp = GetEnvironmentVariableW(L"TMP", old_tmp, ARRAY_SIZE(old_tmp)) != 0;
    have_temp = GetEnvironmentVariableW(L"TEMP", old_temp, ARRAY_SIZE(old_temp)) != 0;
    wcscpy(huge, L"C:\\");
    for (i = 3; i < 380; i++)
        huge[i] = L'h';
    huge[i] = 0;
    SetEnvironmentVariableW(L"TMP", huge);
    SetEnvironmentVariableW(L"TEMP", huge);

    memset(dir, 0x55, sizeof(dir));
    ret = dl_get_temp_dir(dir);
    if (is_reactos())
    {
        ok(!ret, "a temporary folder of 380 characters was accepted\n");
    }
    else
    {
        /* Windows falls back to another folder when the one in the environment is too long */
        ok(!ret || (wcslen(dir) < MAX_PATH && dir[wcslen(dir) - 1] == L'\\'), "got %S\n", dir);
    }

    SetEnvironmentVariableW(L"TMP", have_tmp ? old_tmp : NULL);
    SetEnvironmentVariableW(L"TEMP", have_temp ? old_temp : NULL);
}

static void test_executable_ext(void)
{
    ok(dl_is_executable_ext(L".exe"), ".exe\n");
    ok(dl_is_executable_ext(L".EXE"), ".EXE\n");
    ok(dl_is_executable_ext(L".msi"), ".msi\n");
    ok(dl_is_executable_ext(L".js"), ".js\n");
    ok(!dl_is_executable_ext(L".txt"), ".txt\n");
    ok(!dl_is_executable_ext(L""), "no extension\n");
    ok(!dl_is_executable_ext(L".exe2"), ".exe2\n");
}

START_TEST(DownloadNames)
{
    test_snprintf();
    test_find_header();
    test_disposition();
    test_split_url();
    test_make_leaf();
    test_fit_leaf();
    test_unmask_device();
    test_path_opens_device();
    test_temp_dir();
    test_executable_ext();

    GetTempPathW(ARRAY_SIZE(g_root), g_root);
    wcscat(g_root, L"dlnames-test\\");
    RemoveDirectoryW(g_root);
    if (!CreateDirectoryW(g_root, NULL))
    {
        skip("cannot create %S: %lu\n", g_root, GetLastError());
        return;
    }
    test_unique_file();
    test_unique_file_limit();
    test_unique_file_exhausted();
    test_unique_file_deep();
    remove_folders(g_root);
    g_root[wcslen(g_root) - 1] = 0;
    RemoveDirectoryW(g_root);
}
