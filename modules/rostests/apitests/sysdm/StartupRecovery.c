/*
 * PROJECT:     ReactOS sysdm.cpl API Tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Drives System Properties > Advanced > Startup and Recovery through its window
 *              messages and compares the debugging-information choice with the registry value
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <commctrl.h>
#include <prsht.h>
#include <cpl.h>
#include <stdio.h>
#include <wctype.h>

#define CRASHCONTROL L"System\\CurrentControlSet\\Control\\CrashControl"
#define MAX_CTRL 64

/* Windows titles the sheet "System Properties", the property sheet code of ReactOS "Properties for System" */
static const WCHAR *const SheetTitles[] = { L"System Properties", L"Properties for System", NULL };
static const WCHAR *const DialogTitles[] = { L"Startup and Recovery", NULL };

static const WCHAR DUMP_NAME[] = L"%SystemRoot%\\ssm-test.dmp";
static const WCHAR MINI_NAME[] = L"%SystemRoot%\\ssm-test-mini";

typedef struct
{
    HWND hwnd;
    WCHAR cls[32];
    WCHAR text[160];
    BOOL enabled;
    BOOL visible;
    LRESULT check;
    int cbCount;
    int cbSel;
    WCHAR cbItems[8][80];
} CTRL;

typedef struct
{
    int count;
    CTRL c[MAX_CTRL];
} CTRLS;

static void GetText(HWND hwnd, WCHAR *buf, int cch)
{
    DWORD_PTR res = 0;
    buf[0] = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, cch, (LPARAM)buf, SMTO_ABORTIFHUNG, 3000, &res))
        buf[0] = 0;
}

static BOOL CALLBACK EnumTop(HWND hwnd, LPARAM lParam)
{
    HWND *found = (HWND *)((void **)lParam)[0];
    DWORD pid = *(DWORD *)((void **)lParam)[1];
    const WCHAR *const *titles = (const WCHAR *const *)((void **)lParam)[2];
    DWORD wpid = 0;
    int i;
    WCHAR text[160];

    GetWindowThreadProcessId(hwnd, &wpid);
    if ((pid && wpid != pid) || !IsWindowVisible(hwnd))
        return TRUE;
    GetText(hwnd, text, 160);
    for (i = 0; titles[i]; i++)
    {
        if (!lstrcmpW(text, titles[i]))
        {
            *found = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL CALLBACK EnumList(HWND hwnd, LPARAM lParam)
{
    WCHAR text[160], cls[64];
    DWORD pid = 0;

    if (!IsWindowVisible(hwnd))
        return TRUE;
    GetWindowThreadProcessId(hwnd, &pid);
    GetText(hwnd, text, 160);
    GetClassNameW(hwnd, cls, 64);
    trace("    top pid %lu class %ls title \"%ls\"\n", pid, cls, text);
    return TRUE;
}

static HWND WaitTop(DWORD pid, const WCHAR *const *title, DWORD ms)
{
    DWORD t0 = GetTickCount();
    HWND found = NULL;
    void *args[3];

    args[0] = &found;
    args[1] = &pid;
    args[2] = (void *)title;
    do
    {
        EnumWindows(EnumTop, (LPARAM)args);
        if (found)
            return found;
        Sleep(100);
    } while (GetTickCount() - t0 < ms);
    return NULL;
}

static BOOL CALLBACK EnumCtrl(HWND hwnd, LPARAM lParam)
{
    CTRLS *cs = (CTRLS *)lParam;
    CTRL *c;
    DWORD_PTR res;
    int i;

    if (cs->count >= MAX_CTRL)
        return FALSE;
    c = &cs->c[cs->count++];
    ZeroMemory(c, sizeof(*c));
    c->hwnd = hwnd;
    GetClassNameW(hwnd, c->cls, 32);
    GetText(hwnd, c->text, 160);
    c->enabled = IsWindowEnabled(hwnd);
    c->visible = IsWindowVisible(hwnd);
    c->cbSel = -2;
    if (!lstrcmpiW(c->cls, L"Button") &&
        SendMessageTimeoutW(hwnd, BM_GETCHECK, 0, 0, SMTO_ABORTIFHUNG, 3000, &res))
        c->check = (LRESULT)res;
    if (!lstrcmpiW(c->cls, L"ComboBox"))
    {
        if (SendMessageTimeoutW(hwnd, CB_GETCOUNT, 0, 0, SMTO_ABORTIFHUNG, 3000, &res))
            c->cbCount = (int)res;
        if (SendMessageTimeoutW(hwnd, CB_GETCURSEL, 0, 0, SMTO_ABORTIFHUNG, 3000, &res))
            c->cbSel = (int)res;
        for (i = 0; i < c->cbCount && i < 8; i++)
        {
            WCHAR *buf;
            DWORD_PTR len = 0;

            c->cbItems[i][0] = 0;
            if (!SendMessageTimeoutW(hwnd, CB_GETLBTEXTLEN, i, 0, SMTO_ABORTIFHUNG, 3000, &len) || len == (DWORD_PTR)CB_ERR)
                continue;
            buf = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
            if (!buf)
                continue;
            buf[0] = 0;
            if (SendMessageTimeoutW(hwnd, CB_GETLBTEXT, i, (LPARAM)buf, SMTO_ABORTIFHUNG, 3000, &res))
                lstrcpynW(c->cbItems[i], buf, 80);
            HeapFree(GetProcessHeap(), 0, buf);
        }
    }
    return TRUE;
}

static void Collect(HWND hwnd, CTRLS *cs)
{
    cs->count = 0;
    EnumChildWindows(hwnd, EnumCtrl, (LPARAM)cs);
}

enum { CH_NONE, CH_SMALL, CH_KERNEL, CH_COMPLETE, CH_UNKNOWN };

static int Classify(const WCHAR *text)
{
    WCHAR low[80];
    int i;

    for (i = 0; text[i] && i < 79; i++)
        low[i] = (WCHAR)towlower(text[i]);
    low[i] = 0;
    if (wcsstr(low, L"none"))
        return CH_NONE;
    if (wcsstr(low, L"small") || wcsstr(low, L"minidump"))
        return CH_SMALL;
    if (wcsstr(low, L"kernel"))
        return CH_KERNEL;
    if (wcsstr(low, L"complete"))
        return CH_COMPLETE;
    return CH_UNKNOWN;
}

static CTRL *Nth(CTRLS *cs, const WCHAR *cls, int nth)
{
    int i;

    for (i = 0; i < cs->count; i++)
        if (!lstrcmpiW(cs->c[i].cls, cls) && nth-- == 0)
            return &cs->c[i];
    return NULL;
}

static CTRL *ByText(CTRLS *cs, const WCHAR *cls, const WCHAR *part)
{
    int i;

    for (i = 0; i < cs->count; i++)
        if (!lstrcmpiW(cs->c[i].cls, cls) && wcsstr(cs->c[i].text, part))
            return &cs->c[i];
    return NULL;
}

typedef struct
{
    BOOL opened;
    CTRLS before;
    CTRLS afterSel;
    BOOL selected;
    BOOL committed;
    DWORD afterEnabled;
    BOOL afterEnabledPresent;
    WCHAR afterDumpFile[MAX_PATH];
    WCHAR afterMinidumpDir[MAX_PATH];
    DWORD afterLogEvent, afterAutoReboot, afterOverwrite;
} RUN;

static BOOL CALLBACK FindDesc(HWND hwnd, LPARAM lParam)
{
    HWND *btns = (HWND *)lParam;
    WCHAR cls[32], text[160], clean[160], *s, *d;
    int n;

    GetClassNameW(hwnd, cls, 32);
    if (lstrcmpiW(cls, L"Button") || !IsWindowVisible(hwnd))
        return TRUE;
    GetText(hwnd, text, 160);
    for (s = text, d = clean; *s; s++)
        if (*s != L'&')
            *d++ = *s;
    *d = 0;
    if (wcsncmp(clean, L"Settings", 8))
        return TRUE;
    for (n = 0; n < 3 && btns[n]; n++)
        ;
    if (n < 3)
        btns[n] = hwnd;
    return TRUE;
}

static void ReadRegistry(RUN *r)
{
    HKEY key;
    DWORD type, len;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CRASHCONTROL, 0, KEY_READ, &key))
        return;
    len = sizeof(DWORD);
    r->afterEnabledPresent = !RegQueryValueExW(key, L"CrashDumpEnabled", NULL, &type, (BYTE *)&r->afterEnabled, &len) &&
                             type == REG_DWORD;
    len = sizeof(DWORD);
    RegQueryValueExW(key, L"LogEvent", NULL, &type, (BYTE *)&r->afterLogEvent, &len);
    len = sizeof(DWORD);
    RegQueryValueExW(key, L"AutoReboot", NULL, &type, (BYTE *)&r->afterAutoReboot, &len);
    len = sizeof(DWORD);
    RegQueryValueExW(key, L"Overwrite", NULL, &type, (BYTE *)&r->afterOverwrite, &len);
    len = sizeof(r->afterDumpFile);
    if (RegQueryValueExW(key, L"DumpFile", NULL, &type, (BYTE *)r->afterDumpFile, &len))
        r->afterDumpFile[0] = 0;
    len = sizeof(r->afterMinidumpDir);
    if (RegQueryValueExW(key, L"MinidumpDir", NULL, &type, (BYTE *)r->afterMinidumpDir, &len))
        r->afterMinidumpDir[0] = 0;
    RegCloseKey(key);
}

typedef LONG (WINAPI *CPLAPPLETPROC)(HWND, UINT, LPARAM, LPARAM);

/* ReactOS: the applet runs in a thread of this process, so that the controls of the dialog are read with plain
   messages; user32 passes the text messages of a combo box between processes only on Windows */
static DWORD WINAPI AppletThread(LPVOID param)
{
    CPLAPPLETPROC proc = (CPLAPPLETPROC)param;

    proc(NULL, CPL_INIT, 0, 0);
    proc(NULL, CPL_STARTWPARMSW, 0, (LPARAM)L"3");
    return 0;
}

/* Windows 7 has no CPlApplet in sysdm.cpl: Control_RunDLL starts the sheet in a process of its own */
static BOOL StartSheet(HANDLE *pHandle, BOOL *pbThread, DWORD *pPid, HMODULE *pCpl)
{
    HMODULE hCpl = LoadLibraryW(L"sysdm.cpl");
    CPLAPPLETPROC proc = hCpl ? (CPLAPPLETPROC)GetProcAddress(hCpl, "CPlApplet") : NULL;
    WCHAR cmd[] = L"rundll32.exe shell32.dll,Control_RunDLL sysdm.cpl,,3";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    *pCpl = NULL;
    if (proc)
    {
        *pbThread = TRUE;
        *pPid = GetCurrentProcessId();
        *pHandle = CreateThread(NULL, 0, AppletThread, proc, 0, NULL);
        if (*pHandle)
        {
            *pCpl = hCpl;
            return TRUE;
        }
        FreeLibrary(hCpl);
        return FALSE;
    }

    if (hCpl)
        FreeLibrary(hCpl);
    *pbThread = FALSE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return FALSE;
    CloseHandle(pi.hThread);
    *pHandle = pi.hProcess;
    /* The window of Control_RunDLL is not always owned by the process that was started: the check for a sheet that
       is open already, before anything is changed, is what keeps another one out */
    *pPid = 0;
    return TRUE;
}

/* FALSE when the worker had to be ended by force or did not end: that can leave locks of this process held */
static BOOL CloseSheet(HWND sheet, HANDLE handle, BOOL bThread)
{
    DWORD t0 = GetTickCount();

    if (sheet && IsWindow(sheet))
        PostMessageW(sheet, WM_CLOSE, 0, 0);
    while (sheet && IsWindow(sheet) && GetTickCount() - t0 < 5000)
        Sleep(100);
    if (WaitForSingleObject(handle, 5000) == WAIT_OBJECT_0)
        return TRUE;
    if (bThread)
        TerminateThread(handle, 1);
    else
        TerminateProcess(handle, 1);
    WaitForSingleObject(handle, 5000);
    return FALSE;
}

static BOOL g_abort;

/* value == NULL deletes CrashDumpEnabled */
static BOOL RunDialog(const DWORD *value, BOOL pressOk, int target, RUN *r)
{
    HWND sheet, dlg = NULL, btns[3] = { 0 };
    HKEY key;
    HANDLE handle;
    HMODULE hCpl;
    BOOL bThread, exited;
    int idx;
    DWORD t0, pid;

    ZeroMemory(r, sizeof(*r));

    /* A sheet that is open already would take the place of ours: its owner is left alone */
    if (WaitTop(0, SheetTitles, 0))
        return FALSE;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, CRASHCONTROL, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL))
        return FALSE;
    if (value)
        RegSetValueExW(key, L"CrashDumpEnabled", 0, REG_DWORD, (const BYTE *)value, sizeof(DWORD));
    else
        RegDeleteValueW(key, L"CrashDumpEnabled");
    RegSetValueExW(key, L"DumpFile", 0, REG_EXPAND_SZ, (const BYTE *)DUMP_NAME, sizeof(DUMP_NAME));
    RegSetValueExW(key, L"MinidumpDir", 0, REG_EXPAND_SZ, (const BYTE *)MINI_NAME, sizeof(MINI_NAME));
    RegCloseKey(key);

    if (!StartSheet(&handle, &bThread, &pid, &hCpl))
        return FALSE;

    sheet = WaitTop(pid, SheetTitles, 20000);
    if (!sheet)
    {
        trace("no System Properties window; visible top-level windows:\n");
        EnumWindows(EnumList, 0);
        goto cleanup;
    }
    for (idx = 0; idx < 8 && !btns[2]; idx++)
    {
        DWORD_PTR res;

        ZeroMemory(btns, sizeof(btns));
        SendMessageTimeoutW(sheet, PSM_SETCURSEL, idx, 0, SMTO_ABORTIFHUNG, 3000, &res);
        Sleep(300);
        EnumChildWindows(sheet, FindDesc, (LPARAM)btns);
    }
    if (!btns[2])
    {
        trace("no third Settings button\n");
        goto cleanup;
    }
    PostMessageW(btns[2], BM_CLICK, 0, 0);
    dlg = WaitTop(pid, DialogTitles, 15000);
    if (!dlg)
    {
        trace("no Startup and Recovery window; visible top-level windows:\n");
        EnumWindows(EnumList, 0);
    }
    if (dlg)
    {
        r->opened = TRUE;
        Sleep(500);
        Collect(dlg, &r->before);
        if (target >= 0)
        {
            CTRL *combo = Nth(&r->before, L"ComboBox", 1);
            int i;

            if (combo)
                for (i = 0; i < combo->cbCount && i < 8; i++)
                    if (Classify(combo->cbItems[i]) == target)
                    {
                        DWORD_PTR res;

                        SendMessageTimeoutW(combo->hwnd, CB_SETCURSEL, i, 0, SMTO_ABORTIFHUNG, 3000, &res);
                        PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo->hwnd), CBN_SELCHANGE), (LPARAM)combo->hwnd);
                        Sleep(500);
                        Collect(dlg, &r->afterSel);
                        r->selected = TRUE;
                        break;
                    }
        }
        if (pressOk)
            PostMessageW(GetDlgItem(dlg, IDOK), BM_CLICK, 0, 0);
        else
            PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
        t0 = GetTickCount();
        while (IsWindow(dlg) && GetTickCount() - t0 < 10000)
            Sleep(100);
        r->committed = !IsWindow(dlg);
        if (!r->committed)
        {
            PostMessageW(dlg, WM_CLOSE, 0, 0);
            t0 = GetTickCount();
            while (IsWindow(dlg) && GetTickCount() - t0 < 5000)
                Sleep(100);
        }
    }
    ReadRegistry(r);
cleanup:
    exited = CloseSheet(sheet, handle, bThread);
    if (!exited)
        g_abort = TRUE;
    CloseHandle(handle);
    if (exited && hCpl)
        FreeLibrary(hCpl);
    return r->opened;
}

static BOOL g_verbose;

static void PrintState(const char *tag, CTRLS *cs)
{
    CTRL *combo = Nth(cs, L"ComboBox", 1), *edit = Nth(cs, L"Edit", 2);
    CTRL *over = ByText(cs, L"Button", L"verwrite"), *label = ByText(cs, L"Static", L"ump");

    if (!combo || !edit || !over || !label)
        return;
    printf("  %s: combo sel %d \"%ls\" class %d items %d; label \"%ls\" edit en=%d \"%ls\"; overwrite en=%d chk=%ld\n", tag,
           combo->cbSel, combo->text, Classify(combo->text), combo->cbCount, label->text, edit->enabled, edit->text,
           over->enabled, (long)over->check);
}

static void CheckStart(const char *what, CTRLS *cs, int choice)
{
    CTRL *combo = Nth(cs, L"ComboBox", 1), *edit = Nth(cs, L"Edit", 2), *over = ByText(cs, L"Button", L"verwrite");

    if (!combo || !edit || !over)
    {
        ok(0, "%s: debugging information controls not found\n", what);
        return;
    }
    ok(Classify(combo->text) == choice, "%s: the choice is \"%ls\" (selection %d), expected class %d\n", what, combo->text,
       combo->cbSel, choice);
    ok(edit->enabled == (choice != CH_NONE), "%s: dump file box enabled %d\n", what, edit->enabled);
    ok(over->enabled == (choice == CH_KERNEL || choice == CH_COMPLETE), "%s: overwrite enabled %d\n", what, over->enabled);
    ok(!lstrcmpW(edit->text, choice == CH_SMALL ? MINI_NAME : DUMP_NAME), "%s: dump file box shows \"%ls\"\n", what, edit->text);
}

static const DWORD ChoiceValue[] = { 0, 3, 2, 1 };

typedef struct
{
    const WCHAR *name;
    DWORD type;
    DWORD len;
    BOOL had;
    BYTE data[8192];
} SAVED;

static SAVED Saved[] =
{
    { L"CrashDumpEnabled" }, { L"DumpFile" }, { L"MinidumpDir" }, { L"LogEvent" },
    { L"AutoReboot" }, { L"Overwrite" }, { L"SendAlert" },
};

/* OK also writes the boot loader settings (time to show the operating systems) the dialog shows. ReactOS keeps them in
   freeldr.ini or boot.ini, which are put back here; Windows 7 keeps them in the boot configuration data. */
typedef struct
{
    WCHAR path[MAX_PATH];
    BYTE *data;
    DWORD len;
    BOOL had;
} BOOTFILE;

static BOOTFILE BootFiles[2];

/* FALSE when the file is there and cannot be saved whole */
static BOOL SaveBootFile(BOOTFILE *f, const WCHAR *name)
{
    WCHAR drive[MAX_PATH];
    HANDLE h;
    BOOL ret = FALSE;

    if (!GetEnvironmentVariableW(L"SystemDrive", drive, MAX_PATH))
        return TRUE;
    wsprintfW(f->path, L"%s\\%s", drive, name);
    h = CreateFileW(f->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    f->len = GetFileSize(h, NULL);
    if (f->len != INVALID_FILE_SIZE && f->len < 0x100000)
    {
        DWORD got = 0;

        f->data = HeapAlloc(GetProcessHeap(), 0, f->len + 1);
        if (f->data && ReadFile(h, f->data, f->len, &got, NULL) && got == f->len)
            f->had = ret = TRUE;
    }
    CloseHandle(h);
    return ret;
}

static void RestoreBootFile(BOOTFILE *f)
{
    HANDLE h;
    DWORD written, got = 0;
    BYTE *now;
    BOOL same = FALSE;

    if (!f->had)
        return;
    h = CreateFileW(f->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        ok(0, "Cannot reopen %ls to put it back (error %lu)\n", f->path, GetLastError());
        return;
    }
    now = HeapAlloc(GetProcessHeap(), 0, f->len + 1);
    same = GetFileSize(h, NULL) == f->len && now && ReadFile(h, now, f->len, &got, NULL) && got == f->len &&
           !memcmp(now, f->data, f->len);
    if (now)
        HeapFree(GetProcessHeap(), 0, now);
    CloseHandle(h);
    if (same)
        return;

    h = CreateFileW(f->path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(h != INVALID_HANDLE_VALUE && WriteFile(h, f->data, f->len, &written, NULL) && written == f->len && SetEndOfFile(h),
       "Cannot put back %ls (error %lu)\n", f->path, GetLastError());
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

/* ReactOS: the applet is in sysdm.cpl; Windows 7 has none */
static BOOL HasInProcessApplet(void)
{
    HMODULE hCpl = LoadLibraryW(L"sysdm.cpl");
    BOOL ret = hCpl && GetProcAddress(hCpl, "CPlApplet") != NULL;

    if (hCpl)
        FreeLibrary(hCpl);
    return ret;
}

START_TEST(StartupRecovery)
{
    static const struct
    {
        BOOL missing;
        DWORD value;
        int choice;
        BOOL select;
    } starts[] =
    {
        { FALSE, 0, CH_NONE, FALSE },
        { FALSE, 1, CH_COMPLETE, TRUE },
        { FALSE, 2, CH_KERNEL, FALSE },
        { FALSE, 3, CH_SMALL, TRUE },
        { FALSE, 4, CH_SMALL, FALSE },
        { FALSE, 7, CH_SMALL, TRUE },
        { FALSE, 0xFFFFFFFF, CH_SMALL, FALSE },
        { TRUE, 0, CH_SMALL, TRUE },
    };
    HKEY key;
    int i, target;
    LONG err;
    RUN r;
    char what[80];
    BOOL pressOk;

    g_verbose = GetEnvironmentVariableA("SYSDM_STARTREC_VERBOSE", NULL, 0) != 0;

    if (WaitTop(0, SheetTitles, 0))
    {
        skip("A System Properties window is open already\n");
        return;
    }

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, CRASHCONTROL, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL))
    {
        skip("Cannot write the CrashControl key (not an administrator)\n");
        return;
    }
    for (i = 0; i < (int)(sizeof(Saved) / sizeof(Saved[0])); i++)
    {
        Saved[i].len = sizeof(Saved[i].data);
        err = RegQueryValueExW(key, Saved[i].name, NULL, &Saved[i].type, Saved[i].data, &Saved[i].len);
        if (err != ERROR_SUCCESS && err != ERROR_FILE_NOT_FOUND)
        {
            RegCloseKey(key);
            skip("Cannot save %ls (error %ld)\n", Saved[i].name, err);
            return;
        }
        Saved[i].had = err == ERROR_SUCCESS;
    }
    if (!SaveBootFile(&BootFiles[0], L"freeldr.ini") || !SaveBootFile(&BootFiles[1], L"boot.ini"))
    {
        RegCloseKey(key);
        skip("Cannot save the boot loader settings\n");
        return;
    }

    /* OK writes the boot loader settings of the dialog: on ReactOS they are in files that are put back; on Windows
       they are in the boot configuration data, so there the dialog is closed with Cancel, unless the environment
       variable SYSDM_STARTREC_PRESS_OK is set (a disposable system) */
    pressOk = HasInProcessApplet() || GetEnvironmentVariableA("SYSDM_STARTREC_PRESS_OK", NULL, 0) != 0;

    for (i = 0; i < (int)(sizeof(starts) / sizeof(starts[0])) && !g_abort; i++)
    {
        const DWORD *v = starts[i].missing ? NULL : &starts[i].value;

        for (target = -1; target <= CH_COMPLETE && !g_abort; target++)
        {
            BOOL opened;

            if (target >= 0 && (!pressOk || !starts[i].select || target == CH_COMPLETE))
                continue;
            opened = RunDialog(v, pressOk, target, &r);
            if (v)
                sprintf(what, "value %lu, select %d", *v, target);
            else
                sprintf(what, "no value, select %d", target);
            if (!opened)
            {
                skip("%s: Startup and Recovery did not open\n", what);
                continue;
            }
            if (g_verbose)
            {
                printf("== %s\n", what);
                PrintState("start", &r.before);
            }
            ok(r.committed, "%s: Startup and Recovery did not close\n", what);
            if (target < 0)
            {
                CheckStart(what, &r.before, starts[i].choice);
                if (!pressOk)
                    continue;
                ok(r.afterEnabledPresent == !starts[i].missing && (starts[i].missing || r.afterEnabled == starts[i].value),
                   "%s: OK left CrashDumpEnabled %s%lu, expected it unchanged\n", what, r.afterEnabledPresent ? "" : "missing ",
                   r.afterEnabled);
                ok(!lstrcmpW(r.afterDumpFile, DUMP_NAME) && !lstrcmpW(r.afterMinidumpDir, MINI_NAME),
                   "%s: OK left DumpFile \"%ls\" and MinidumpDir \"%ls\"\n", what, r.afterDumpFile, r.afterMinidumpDir);
            }
            else if (!r.selected)
            {
                skip("%s: the choice is not listed\n", what);
            }
            else
            {
                ok(r.afterEnabledPresent && r.afterEnabled == ChoiceValue[target], "%s: OK wrote CrashDumpEnabled %s%lu, expected %lu\n",
                   what, r.afterEnabledPresent ? "" : "missing ", r.afterEnabled, ChoiceValue[target]);
            }
        }
    }

    ok(!g_abort, "The Startup and Recovery window did not end, the remaining checks were not run\n");

    for (i = 0; i < (int)(sizeof(BootFiles) / sizeof(BootFiles[0])); i++)
        RestoreBootFile(&BootFiles[i]);

    for (i = 0; i < (int)(sizeof(Saved) / sizeof(Saved[0])); i++)
    {
        if (Saved[i].had)
            err = RegSetValueExW(key, Saved[i].name, 0, Saved[i].type, Saved[i].data, Saved[i].len);
        else
        {
            err = RegDeleteValueW(key, Saved[i].name);
            if (err == ERROR_FILE_NOT_FOUND)
                err = ERROR_SUCCESS;
        }
        ok(err == ERROR_SUCCESS, "Cannot put back %ls (error %ld)\n", Saved[i].name, err);
    }
    RegCloseKey(key);
}
