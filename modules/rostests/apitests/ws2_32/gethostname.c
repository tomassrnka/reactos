/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests for Hostname command and Winsock gethostname() function.
 * COPYRIGHT:   Copyright 2019 Doug Lyons <douglyons@douglyons.com>
 *              Copyright 2019 Hermes Belusca-Maito
 */

#include "ws2_32.h"

#include <winreg.h>

#define REG_HOSTNAME_KEY        L"System\\CurrentControlSet\\Services\\Tcpip\\Parameters"
#define REG_COMPUTERNAME_KEY    L"System\\CurrentControlSet\\Control\\ComputerName\\ComputerName"

HKEY
OpenRegKey(
    IN HKEY   hRootKey,
    IN PCWSTR pszSubKey,
    IN DWORD  ulOptions)
{
    HKEY hKey = NULL;
    LONG Error = RegOpenKeyExW(hRootKey, pszSubKey, 0, ulOptions, &hKey);
    if (Error == ERROR_SUCCESS)
        return hKey;
    return NULL;
}

BOOL
GetHostnameFromCommand(
    OUT PSTR pszHostnameBuffer,
    IN ULONG ulBufferSize)
{
    PCSTR file_name = "HostName123.txt";
    CHAR cmdline[MAX_PATH] = "hostname > ";
    FILE *fp;
    INT iResult;
    INT len;
    PCSTR ptr;

    ZeroMemory(pszHostnameBuffer, ulBufferSize * sizeof(CHAR));

    /* Run the 'hostname' command, piping out its result to the temporary file */
    strcat(cmdline, file_name);
    system(cmdline);

    /* Open the temporary file in read mode */
    fp = fopen(file_name, "r");
    ok(fp != NULL, "An error occurred while opening the file.\n");
    if (fp == NULL)
        return FALSE;

    /* Read its contents */
    ptr = fgets(pszHostnameBuffer, ulBufferSize * sizeof(CHAR), fp);
    ok(ptr != NULL, "An error occurred while reading the file.\n");
    if (ptr == NULL)
        goto Cleanup;

    /* Delete the expected ending line feed character */
    len = lstrlenA(pszHostnameBuffer);
    if (pszHostnameBuffer[len-1] == '\r' || pszHostnameBuffer[len-1] == '\n')
        pszHostnameBuffer[len-1] = ANSI_NULL;

Cleanup:
    /* Close and remove the file */
    iResult = fclose(fp);
    ok(iResult == 0, "An error occurred while closing the file: %i.\n", iResult);
    iResult = remove(file_name);
    ok(iResult == 0, "An error occurred while deleting the file: %i.\n", iResult);

    return (ptr ? TRUE : FALSE);
}

static BOOL
IsAllChar(const CHAR *p, SIZE_T cb, CHAR c)
{
    SIZE_T i;
    for (i = 0; i < cb; i++)
    {
        if (p[i] != c)
            return FALSE;
    }
    return TRUE;
}

/* Called with Winsock started */
static void
TestBufferSizes(void)
{
    CHAR szName[256], szShort[300];
    INT iResult, cchName;

    ZeroMemory(szName, sizeof(szName));
    iResult = gethostname(szName, sizeof(szName) - 1);
    ok(iResult == 0, "gethostname failed with %d\n", WSAGetLastError());
    if (iResult != 0)
        return;
    cchName = lstrlenA(szName);
    ok(cchName > 0, "Empty host name\n");

    /* No room for the terminator: the buffer is left alone */
    memset(szShort, 'x', sizeof(szShort));
    WSASetLastError(0xdeadbeef);
    iResult = gethostname(szShort, cchName);
    ok_dec(iResult, SOCKET_ERROR);
    ok_dec(WSAGetLastError(), WSAEFAULT);
    ok(IsAllChar(szShort, sizeof(szShort), 'x'), "Buffer changed: '%.*s'\n", cchName, szShort);

    /* Exactly the name and its terminator */
    memset(szShort, 'x', sizeof(szShort));
    WSASetLastError(0xdeadbeef);
    iResult = gethostname(szShort, cchName + 1);
    ok_dec(iResult, 0);
    ok(szShort[cchName] == ANSI_NULL, "Name not terminated\n");
    ok(!memcmp(szShort, szName, cchName), "Got '%.*s', expected '%s'\n", cchName, szShort, szName);
    ok(IsAllChar(szShort + cchName + 1, sizeof(szShort) - cchName - 1, 'x'), "Wrote past the buffer\n");

    /* One byte */
    memset(szShort, 'x', sizeof(szShort));
    WSASetLastError(0xdeadbeef);
    iResult = gethostname(szShort, 1);
    ok_dec(iResult, SOCKET_ERROR);
    ok_dec(WSAGetLastError(), WSAEFAULT);
    ok(IsAllChar(szShort, sizeof(szShort), 'x'), "Buffer changed\n");

    memset(szShort, 'x', sizeof(szShort));
    WSASetLastError(0xdeadbeef);
    iResult = gethostname(szShort, 0);
    ok_dec(iResult, SOCKET_ERROR);
    ok_dec(WSAGetLastError(), WSAEFAULT);
    ok(IsAllChar(szShort, sizeof(szShort), 'x'), "Buffer changed\n");

    WSASetLastError(0xdeadbeef);
    iResult = gethostname(NULL, sizeof(szShort));
    ok_dec(iResult, SOCKET_ERROR);
    ok_dec(WSAGetLastError(), WSAEFAULT);
}

/* Called with Winsock started and the host name key open for writing */
static void
TestLongName(HKEY hKeyHN, PCSTR pszOldName)
{
    static const CHAR szLong[] = "ROSHOSTNAME-LONGER-THAN-A-NETBIOS-NAME";
    CHAR szName[256];
    LONG Error;
    INT iResult;

    Error = RegSetValueExA(hKeyHN, "Hostname", 0, REG_SZ, (LPBYTE)szLong, sizeof(szLong));
    ok(Error == ERROR_SUCCESS, "Error setting the long host name (%ld).\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    /* A DNS host name can be longer than a NetBIOS computer name */
    ZeroMemory(szName, sizeof(szName));
    iResult = gethostname(szName, sizeof(szName) - 1);
    ok(iResult == 0, "gethostname failed with %d\n", WSAGetLastError());
    ok(!strcmp(szName, szLong), "Got '%s', expected '%s'\n", szName, szLong);

    Error = RegSetValueExA(hKeyHN, "Hostname", 0, REG_SZ, (LPBYTE)pszOldName, lstrlenA(pszOldName) + 1);
    ok(Error == ERROR_SUCCESS, "Error resetting the host name (%ld).\n", Error);
}

START_TEST(gethostname)
{
    INT pos;
    HKEY hKeyHN;
    DWORD cbData;
    LONG Error;
    INT hnError;
    INT iResult;
    WSADATA wsaData;
    DWORD uApiHostNameSize;
    CHAR szApiHostName[256] = "";
    CHAR szHostNameOld[256] = "";
    CHAR szHostNameNew[256] = "";
    CHAR hostbuffer[256] = "";

/*
 * Notes about the retrieval of the computer name on Windows.
 *
 * - GetComputerName() (equivalently, GetComputerNameEx(ComputerNameNetBIOS))
 *   retrieves the name cached in the registry value "ComputerName" under
 *   "System\\CurrentControlSet\\Control\\ComputerName\\ActiveComputerName" .
 *
 * - GetComputerNameEx(ComputerNameDnsHostname) as well as the 'hostname' command
     return the registry value "Hostname" under "System\\CurrentControlSet\\Services\\Tcpip\\Parameters" .
 *
 * - In case a new computer name is set, its value is cached in the registry value
 *   "ComputerName" under "System\\CurrentControlSet\\Control\\ComputerName\\ComputerName",
 *   and also in the registry value "NV Hostname" under "System\\CurrentControlSet\\Services\\Tcpip\\Parameters" .
 */

    /* Buffer sizes, independent of the host name changes below */
    iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    ok(iResult == 0, "Error occurred starting Winsock");
    if (iResult == 0)
    {
        TestBufferSizes();
        WSACleanup();
    }

    /* Retrieve the current host name using API */
    uApiHostNameSize = _countof(szApiHostName);
    if (!GetComputerNameExA(ComputerNameDnsHostname, szApiHostName, &uApiHostNameSize))
    {
        /* Fail in case of error */
        skip("GetComputerNameExA(ComputerNameDnsHostname) failed, error %lu\n", GetLastError());
        return;
    }
    printf("The Hostname from API is '%s'.\n", szApiHostName);

    /* Retrieve the current host name using the 'hostname' command */
    if (!GetHostnameFromCommand(hostbuffer, _countof(hostbuffer)))
    {
        /* Fail in case of error */
        skip("Error while retrieving the host name using the 'hostname' command!\n");
        return;
    }
    printf("The Hostname from command is '%s'.\n", hostbuffer);

    pos = strcmp(szApiHostName, hostbuffer);
    printf("The test results were '%s'.\n", pos==0 ? "good" : "bad");
    ok(pos == 0, "hostbuffer '%s' should have been szApiHostName '%s'.\n", hostbuffer, szApiHostName);

    hKeyHN = OpenRegKey(HKEY_LOCAL_MACHINE, REG_HOSTNAME_KEY, KEY_ALL_ACCESS);
    ok(hKeyHN != NULL, "Error while opening hostname registry key.\n");
    if (hKeyHN == NULL)
        return;

    /* Get Old Hostname */
    szHostNameOld[0] = ANSI_NULL;
    cbData = sizeof(szHostNameOld);
    Error = RegQueryValueExA(hKeyHN, "Hostname", NULL, NULL, (LPBYTE)szHostNameOld, &cbData);

    printf("Hostname from Registry is '%s'.\n", szHostNameOld);

    pos = strcmp(szHostNameOld, szApiHostName);
    printf("The test results were '%s'.\n", pos==0 ? "good" : "bad");
    ok(pos == 0, "szApiHostName '%s' should have been szHostNameOld '%s'.\n", szApiHostName, szHostNameOld);

    /* Change Hostname in the Registry */
    szHostNameNew[0] = ANSI_NULL;
    strcat(szHostNameNew, "ROSHOSTNAME1");
    cbData = lstrlenA(szHostNameNew) + 1;

    Error = RegSetValueExA(hKeyHN, "Hostname", 0, REG_SZ, (LPBYTE)szHostNameNew, cbData);
    ok(Error == ERROR_SUCCESS, "Error setting new registry value (%ld).\n", GetLastError());

    /* Retrieve the current host name using API */
    uApiHostNameSize = _countof(szApiHostName);
    if (!GetComputerNameExA(ComputerNameDnsHostname, szApiHostName, &uApiHostNameSize))
    {
        /* Fail in case of error */
        skip("GetComputerNameExA(ComputerNameDnsHostname) failed, error %lu\n", GetLastError());
        goto Cleanup;
    }
    printf("The Hostname from API is '%s'.\n", szApiHostName);

    /* Retrieve the current host name using the 'hostname' command */
    if (!GetHostnameFromCommand(hostbuffer, _countof(hostbuffer)))
    {
        /* Fail in case of error */
        skip("Error while retrieving the host name using the 'hostname' command!\n");
        goto Cleanup;
    }
    printf("The Hostname from command is '%s'.\n", hostbuffer);

    pos = strcmp(szHostNameNew, szApiHostName);
    printf("The test results were '%s'.\n", pos==0 ? "good" : "bad");
    ok(pos == 0, "szApiHostName '%s' should be szHostNameNew '%s'.\n", szApiHostName, szHostNameNew);

    pos = strcmp(szHostNameNew, hostbuffer);
    printf("The test results were '%s'.\n", pos==0 ? "good" : "bad");
    ok(pos == 0, "hostbuffer '%s' should have been szHostNameNew '%s'.\n", hostbuffer, szHostNameNew);

    /* Reset the original registry entry */
    cbData = lstrlenA(szHostNameOld) + 1;

    Error = RegSetValueExA(hKeyHN, "Hostname", 0, REG_SZ, (LPBYTE)szHostNameOld, cbData);
    ok(Error == ERROR_SUCCESS, "Error resetting new registry value back (%ld).\n", GetLastError());

/*============ Winsock Checks ===============*/

    /* Start up Winsock to use gethostname() */
    iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    ok(iResult == 0, "Error occurred starting Winsock");
    if (iResult != 0)
        goto Cleanup;

    /* Retrieve gethostname() */
    hnError = gethostname(hostbuffer, sizeof(hostbuffer));
    ok(!hnError, "Winsock gethostname() Error is '%d'.\n", WSAGetLastError());

    /* Display results */
    if (!hnError)
        printf("Winsock gethostname() is '%s'.\n", hostbuffer);

    pos = strcmp(szHostNameOld, hostbuffer);
    printf("The test results were '%s'.\n", pos==0 ? "good" : "bad");
    ok(pos == 0, "szHostNameOld '%s' should be hostbuffer '%s'.\n", szHostNameOld, hostbuffer);

    /* Change Hostname in the Registry */
    szHostNameNew[0] = ANSI_NULL;
    strcat(szHostNameNew, "ROSHOSTNAME1");
    cbData = lstrlenA(szHostNameNew) + 1;

    Error = RegSetValueExA(hKeyHN, "Hostname", 0, REG_SZ, (LPBYTE)szHostNameNew, cbData);
    ok(Error == ERROR_SUCCESS, "Error setting new registry value (%ld).\n", GetLastError());

    /* Retrieve gethostname() */
    hnError = gethostname(hostbuffer, sizeof(hostbuffer));
    ok(!hnError, "Winsock gethostname() Error is '%d'.\n", WSAGetLastError());

    /* Display results */
    if (!hnError)
        printf("Winsock gethostname() is '%s'.\n", hostbuffer);

    pos = strcmp(szHostNameNew, hostbuffer);
    printf("The test results were '%s'.\n", pos==0 ? "good" : "bad");
    ok(pos == 0, "szHostNameNew '%s' should be hostbuffer '%s'.\n", szHostNameNew, hostbuffer);

    /* Reset the original registry entry */
    cbData = lstrlenA(szHostNameOld) + 1;

    Error = RegSetValueExA(hKeyHN, "Hostname", 0, REG_SZ, (LPBYTE)szHostNameOld, cbData);
    ok(Error == ERROR_SUCCESS, "Error resetting new registry value back (%ld).\n", GetLastError());

    /* Retrieve gethostname() */
    hnError = gethostname(hostbuffer, sizeof(hostbuffer));
    ok(!hnError, "Winsock gethostname() Error is '%d'.\n", WSAGetLastError());

    /* Display results */
    if (!hnError)
        printf("Winsock gethostname() is '%s'.\n", hostbuffer);

    pos = strcmp(szHostNameOld, hostbuffer);
    printf("The test results were '%s'.\n", pos==0 ? "good" : "bad");
    ok(pos == 0, "szHostNameOld '%s' should be hostbuffer '%s'.\n", szHostNameOld, hostbuffer);

    TestLongName(hKeyHN, szHostNameOld);

Cleanup:
    iResult = WSACleanup();
    ok(iResult == 0, "WSACleanup error occurred ending Winsock");

    Error = RegCloseKey(hKeyHN);
    ok(Error == ERROR_SUCCESS, "Error closing registry key (%ld).\n", GetLastError());

    return;
}
