/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Test for the cmd ftype internal command
 */

#include "precomp.h"
#include <stdio.h>
#include <string.h>
#include <shlwapi.h>

#define CLASSES "SOFTWARE\\Classes\\"
/* Names no real file type has; the test stops if either key exists and removes only what it creates */
#define TYPE_A "ReactOSApitestFtypeA"
#define TYPE_NONE "ReactOSApitestFtypeNone"
#define NOT_FOUND(type) "File type '" type "' not found or no open command associated with it.\n"

/* Runs "cmd.exe /c <args>" with stdout and stderr in one pipe; line ends are reduced to LF */
static BOOL RunCmd(const char *args, char *out, DWORD cbOut, DWORD *pdwExit)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE hRead, hWrite;
    char cmdline[512];
    DWORD cb, len = 0;
    char buf[256];
    DWORD i, j;

    out[0] = 0;
    *pdwExit = (DWORD)-1;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return FALSE;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    snprintf(cmdline, sizeof(cmdline), "cmd.exe /c %s", args);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return FALSE;
    }
    CloseHandle(hWrite);
    CloseHandle(pi.hThread);

    while (ReadFile(hRead, buf, sizeof(buf), &cb, NULL) && cb)
    {
        if (len + cb < cbOut)
        {
            memcpy(out + len, buf, cb);
            len += cb;
        }
    }
    out[len] = 0;
    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, pdwExit);
    CloseHandle(pi.hProcess);

    for (i = j = 0; out[i]; i++)
    {
        if (out[i] != '\r')
            out[j++] = out[i];
    }
    out[j] = 0;
    return TRUE;
}

static void CheckRun(int line, const char *args, DWORD expectedExit, const char *expectedOut)
{
    char out[4096];
    DWORD dwExit;

    ok(RunCmd(args, out, sizeof(out), &dwExit), "line %d: could not run '%s'\n", line, args);
    ok(strcmp(out, expectedOut) == 0, "line %d: '%s' printed '%s', expected '%s'\n", line, args, out, expectedOut);
    ok(dwExit == expectedExit, "line %d: '%s' exit code %lu, expected %lu\n",
       line, args, dwExit, expectedExit);
}
#define CHECK_RUN(args, exit, out) CheckRun(__LINE__, args, exit, out)

static void CheckSyntaxError(int line, const char *args)
{
    char out[4096], cmd[256];
    DWORD dwExit;

    ok(RunCmd(args, out, sizeof(out), &dwExit), "line %d: could not run '%s'\n", line, args);
    ok(out[0] != 0, "line %d: '%s' printed nothing\n", line, args);
    ok(dwExit == 0, "line %d: '%s' exit code %lu\n", line, args, dwExit);
    snprintf(cmd, sizeof(cmd), "%s 2>nul", args);
    ok(RunCmd(cmd, out, sizeof(out), &dwExit), "line %d: could not run '%s'\n", line, cmd);
    ok(out[0] == 0, "line %d: '%s' printed '%s' on the output stream\n", line, cmd, out);
}

/* Reads the default value of Classes\<type>\shell\open\command: 0 not there, else the registry type */
static DWORD ReadCommandValue(const char *type, char *value, DWORD cbValue)
{
    char path[256];
    HKEY hKey;
    DWORD dwType = 0, cb = cbValue - 1;
    LONG lRet;

    value[0] = 0;
    snprintf(path, sizeof(path), CLASSES "%s\\shell\\open\\command", type);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return 0;
    lRet = RegQueryValueExA(hKey, NULL, NULL, &dwType, (LPBYTE)value, &cb);
    RegCloseKey(hKey);
    if (lRet != ERROR_SUCCESS)
        return 0;
    value[cb] = 0;
    return dwType;
}

static BOOL KeyExists(const char *type)
{
    char path[256];
    HKEY hKey;

    snprintf(path, sizeof(path), CLASSES "%s", type);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(hKey);
    return TRUE;
}

START_TEST(ftype)
{
    char value[512], out[65536], *line, *next;
    DWORD dwType, dwExit;
    HKEY hKey;
    LONG lRet;
    BOOL bFound;

    if (KeyExists(TYPE_A) || KeyExists(TYPE_NONE))
    {
        skip("A key of the test already exists in HKLM\\SOFTWARE\\Classes, not touching it\n");
        return;
    }

    /* The command writes to HKLM */
    lRet = RegCreateKeyExA(HKEY_LOCAL_MACHINE, CLASSES TYPE_A, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL);
    if (lRet != ERROR_SUCCESS)
    {
        skip("Cannot write HKLM\\" CLASSES " (error %ld)\n", lRet);
        return;
    }
    RegCloseKey(hKey);
    SHDeleteKeyA(HKEY_LOCAL_MACHINE, CLASSES TYPE_A);

    /* Unknown type: the text goes to the error stream, the exit code is 2 */
    CHECK_RUN("ftype " TYPE_A, 2, NOT_FOUND(TYPE_A));
    CHECK_RUN("ftype " TYPE_A " 2>nul", 2, "");
    CHECK_RUN("ftype " TYPE_A " >nul", 2, NOT_FOUND(TYPE_A));

    /* Set prints the new definition */
    CHECK_RUN("ftype " TYPE_A "=C:\\Tools\\x.exe \"%1\" %*", 0, TYPE_A "=C:\\Tools\\x.exe \"%1\" %*\n");
    dwType = ReadCommandValue(TYPE_A, value, sizeof(value));
    ok(dwType == REG_EXPAND_SZ, "stored type %lu\n", dwType);
    ok(strcmp(value, "C:\\Tools\\x.exe \"%1\" %*") == 0, "stored '%s'\n", value);

    /* Query shows the stored text unexpanded, with the name as typed */
    CHECK_RUN("ftype " TYPE_A, 0, TYPE_A "=C:\\Tools\\x.exe \"%1\" %*\n");
    CHECK_RUN("ftype reactosAPITESTftypea", 0, "reactosAPITESTftypea=C:\\Tools\\x.exe \"%1\" %*\n");
    /* The last word of a longer text is dropped, like on Windows */
    CHECK_RUN("ftype " TYPE_A " extra", 0, TYPE_A "=C:\\Tools\\x.exe \"%1\" %*\n");
    CHECK_RUN("ftype " TYPE_A " more words", 2, NOT_FOUND(TYPE_A " more"));

    /* A second set replaces the text */
    CHECK_RUN("ftype " TYPE_A "=\"C:\\Tools\\y.exe\" /x %1", 0, TYPE_A "=\"C:\\Tools\\y.exe\" /x %1\n");
    dwType = ReadCommandValue(TYPE_A, value, sizeof(value));
    ok(dwType == REG_EXPAND_SZ, "stored type %lu\n", dwType);
    ok(strcmp(value, "\"C:\\Tools\\y.exe\" /x %1") == 0, "stored '%s'\n", value);

    /* The list holds every type with an open command, one NAME=COMMAND line each */
    ok(RunCmd("ftype", out, sizeof(out), &dwExit), "could not run ftype\n");
    ok(dwExit == 0, "ftype exit code %lu\n", dwExit);
    bFound = FALSE;
    for (line = out; *line; line = next)
    {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        else
            next = line + strlen(line);
        ok(strchr(line, '=') != NULL, "list line without '=': '%s'\n", line);
        if (!strcmp(line, TYPE_A "=\"C:\\Tools\\y.exe\" /x %1"))
            bFound = TRUE;
    }
    ok(bFound, "the list does not show " TYPE_A "\n");

    /* A second equal sign, or no name, is a syntax error (on the error stream) and changes nothing */
    CheckSyntaxError(__LINE__, "ftype " TYPE_A "=a=b");
    CheckSyntaxError(__LINE__, "ftype =cmd");
    dwType = ReadCommandValue(TYPE_A, value, sizeof(value));
    ok(strcmp(value, "\"C:\\Tools\\y.exe\" /x %1") == 0, "stored '%s'\n", value);

    /* Remove deletes the value and leaves the keys */
    CHECK_RUN("ftype " TYPE_A "=", 0, "");
    ok(ReadCommandValue(TYPE_A, value, sizeof(value)) == 0, "the value is still there\n");
    ok(KeyExists(TYPE_A "\\shell\\open\\command"), "the command key was removed\n");
    CHECK_RUN("ftype " TYPE_A, 0, NOT_FOUND(TYPE_A));

    /* Removing from a type without an open command key fails and creates nothing */
    CHECK_RUN("ftype " TYPE_NONE "=", 2, NOT_FOUND(TYPE_NONE));
    ok(!KeyExists(TYPE_NONE), "the key was created\n");
    if (KeyExists(TYPE_NONE))
        SHDeleteKeyA(HKEY_LOCAL_MACHINE, CLASSES TYPE_NONE); // it did not exist when the test began

    SHDeleteKeyA(HKEY_LOCAL_MACHINE, CLASSES TYPE_A);
}
