/*
 * PROJECT:     ReactOS net.exe Tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Tests which NET SHARE arguments the command takes and which it answers with its usage text
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 *
 * The command prints its usage text and exits with 1 before it asks the Server service; any other answer comes from
 * the service (or from the command after it asked) and ends with 0 or 2. The test looks at the exit code only, so it
 * does not depend on the language, and uses arguments that no Server service accepts: devices, which it refuses, and
 * paths that no file system has (a colon after the first component) or that do not exist (a directory with a name
 * made up for this test). Deletions use a name with a colon, which the command refuses before it asks the service.
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <stdio.h>
#include <string.h>

#define CHILD_TIMEOUT 60000

/* Returns the exit code of net.exe with the arguments, or -1 when it did not run to its end */
static LONG RunNet(const char *args)
{
    char cmd[512];
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE hNul, hOut;
    DWORD code, t0 = GetTickCount();
    LONG ret = -1;

    /* Input from NUL: a question the command asks gets the end of the file as the answer */
    hNul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (hNul == INVALID_HANDLE_VALUE)
        return -1;
    hOut = hNul;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hNul;
    si.hStdOutput = si.hStdError = hOut;
    sprintf(cmd, "net.exe share %s", args);
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT && GetTickCount() - t0 < CHILD_TIMEOUT)
            ;
        if (GetExitCodeProcess(pi.hProcess, &code) && code != 259 /* STILL_ACTIVE */)
            ret = (LONG)code;
        else
            TerminateProcess(pi.hProcess, 99);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(hNul);
    return ret;
}

START_TEST(NetShare)
{
    static const struct
    {
        const char *args;
        BOOL usage;
    } cases[] =
    {
        /* Devices: LPT1 to LPT9 and COM1 to COM9, trailing dots and a final colon allowed (measured on Windows Server 2008 R2) */
        { "ssmnotest1=LPT1:", FALSE },
        { "ssmnotest1=lpt1:", FALSE },
        { "ssmnotest1=LPT1", FALSE },
        { "ssmnotest1=LPT1.", FALSE },
        { "ssmnotest1=LPT1..", FALSE },
        { "ssmnotest1=LPT1.:", FALSE },
        { "ssmnotest1=LPT9:", FALSE },
        { "ssmnotest1=COM1:", FALSE },
        { "ssmnotest1=COM9", FALSE },
        { "ssmnotest1=LPT1: /remark:x", FALSE },
        { "ssmnotest1=\\\\.\\LPT1", FALSE },
        { "ssmnotest1=LPT0", TRUE },
        { "ssmnotest1=LPT10:", TRUE },
        { "ssmnotest1=LPT1::", TRUE },
        { "ssmnotest1=LPT1:.", TRUE },
        { "ssmnotest1=LPT1.txt", TRUE },
        { "ssmnotest1=COM0:", TRUE },
        { "ssmnotest1=PRN:", TRUE },
        { "ssmnotest1=NUL:", TRUE },
        { "ssmnotest1=LPT1:\\", TRUE },
        /* Paths (a colon in the last component: no file system has it) */
        { "ssmnotest1=Z:\\ssm:x", FALSE },
        { "ssmnotest1=Z:/ssm:x", FALSE },
        { "ssmnotest1=\\\\.\\Z:\\ssm:x", FALSE },
        { "ssmnotest1=Z:\\ssm:x\\", FALSE },
        { "ssmnotest1=Z:\\ssmnodir9f3a\\", TRUE },
        { "ssmnotest1=Z:\\ssmnodir9f3a\\\\", TRUE },
        { "ssmnotest1=Z:\\*", TRUE },
        { "ssmnotest1=Z:ssmnodir9f3a", TRUE },
        { "ssmnotest1=\\ssm", TRUE },
        { "ssmnotest1=ssm", TRUE },
        { "ssmnotest1=\\\\localhost\\ssm", TRUE },
        /* Characters a share name may not have */
        { "ssm:x=LPT1:", TRUE },
        { "ssm\\x=LPT1:", TRUE },
        { "ssm;x=LPT1:", TRUE },
        { "ssm+x=LPT1:", TRUE },
        { "ssm,x=LPT1:", TRUE },
        { "ssm[x=LPT1:", TRUE },
        { "ssm:x", TRUE },
        { "ssm\\x", TRUE },
        { "ssm;x", TRUE },
        { "ssm\\x /remark:y", TRUE },
        { "LPT1:", TRUE },
        { "LPT1: /remark:x", TRUE },
        { "XYZ: /users:3", TRUE },
        /* Deleting: the whole argument is the name; a colon in it is refused without asking the service */
        { "XYZ: /delete", FALSE },
        { "ssmnotest1=LPT1: /delete", FALSE },
        { "ssm;x /delete", FALSE },
        { "XYZ: ssmx /delete", FALSE },
        { "XYZ: ssmx ssmy /delete", FALSE },
        { "ssmnotest1 ssmx", TRUE },
    };
    int i;
    BOOL madeUpExists = GetFileAttributesA("Z:\\ssmnodir9f3a") != INVALID_FILE_ATTRIBUTES;

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
    {
        LONG code;

        /* An older parser forwards the made-up directory to the service: not if that directory exists */
        if (madeUpExists && strstr(cases[i].args, "ssmnodir9f3a"))
        {
            skip("Z:\\ssmnodir9f3a exists, \"%s\" is not run\n", cases[i].args);
            continue;
        }
        code = RunNet(cases[i].args);
        BOOL usage = code == 1;

        ok(code == 0 || code == 1 || code == 2, "net share %s: exit code %ld, expected 0, 1 or 2\n", cases[i].args, code);
        ok(usage == cases[i].usage, "net share %s: exit code %ld (usage text %s), expected usage text %s\n", cases[i].args, code,
           usage ? "printed" : "not printed", cases[i].usage ? "printed" : "not printed");
    }
}
