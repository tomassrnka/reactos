/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Test for GetComputerNameEx
 * PROGRAMMER:      Thomas Faber <thomas.faber@reactos.org>
 */

#include "precomp.h"

static
VOID
TestGetComputerNameEx(
    _In_ COMPUTER_NAME_FORMAT NameType)
{
    WCHAR Reference[128];
    DWORD ReferenceLen;
    WCHAR BufferW[128];
    CHAR BufferA[128];
    BOOL Ret;
    DWORD Size;
    DWORD Error;
    ULONG i;

    Size = RTL_NUMBER_OF(Reference);
    Ret = GetComputerNameExW(NameType, Reference, &Size);
    ok(Ret == TRUE, "[%d] GetComputerNameExW returned %d\n", NameType, Ret);
    if (!Ret)
    {
        skip("[%d] Failed to get reference string\n", NameType);
        return;
    }
    trace("[%d] Reference is %ls\n", NameType, Reference);
    ReferenceLen = lstrlenW(Reference);
    ok(ReferenceLen < RTL_NUMBER_OF(Reference),
       "[%d] Unexpected ReferenceLen %lu\n", NameType, ReferenceLen);
    if (NameType != ComputerNameDnsDomain && NameType != ComputerNamePhysicalDnsDomain)
    {
        ok(ReferenceLen != 0, "[%d] Unexpected ReferenceLen %lu\n", NameType, ReferenceLen);
    }
    ok(Size == ReferenceLen, "[%d] Size is %lu, expected %lu\n", NameType, Size, ReferenceLen);

    /* NULL buffer, NULL size */
    StartSeh()
        SetLastError(0xdeadbeef);
        Ret = GetComputerNameExW(NameType, NULL, NULL);
        Error = GetLastError();
        ok(Ret == FALSE, "[%d] GetComputerNameExW returned %d\n", NameType, Ret);
        ok(Error == ERROR_INVALID_PARAMETER, "[%d] GetComputerNameExW returned error %lu\n", NameType, Error);
    EndSeh(STATUS_SUCCESS);
    StartSeh()
        SetLastError(0xdeadbeef);
        Ret = GetComputerNameExA(NameType, NULL, NULL);
        Error = GetLastError();
        ok(Ret == FALSE, "[%d] GetComputerNameExA returned %d\n", NameType, Ret);
        ok(Error == ERROR_INVALID_PARAMETER, "[%d] GetComputerNameExA returned error %lu\n", NameType, Error);
    EndSeh(STATUS_SUCCESS);

    /* NULL buffer, nonzero size */
    Size = 0x55555555;
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExW(NameType, NULL, &Size);
    Error = GetLastError();
    ok(Ret == FALSE, "[%d] GetComputerNameExW returned %d\n", NameType, Ret);
    ok(Error == ERROR_INVALID_PARAMETER, "[%d] GetComputerNameExW returned error %lu\n", NameType, Error);
    ok(Size == 0x55555555, "[%d] Got Size %lu\n", NameType, Size);

    Size = 0x55555555;
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExA(NameType, NULL, &Size);
    Error = GetLastError();
    ok(Ret == FALSE, "[%d] GetComputerNameExA returned %d\n", NameType, Ret);
    ok(Error == ERROR_INVALID_PARAMETER, "[%d] GetComputerNameExA returned error %lu\n", NameType, Error);
    ok(Size == 0x55555555, "[%d] Got Size %lu\n", NameType, Size);

    /* non-NULL buffer, NULL size */
    RtlFillMemory(BufferW, sizeof(BufferW), 0x55);
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExW(NameType, BufferW, NULL);
    Error = GetLastError();
    ok(Ret == FALSE, "[%d] GetComputerNameExW returned %d\n", NameType, Ret);
    ok(Error == ERROR_INVALID_PARAMETER, "[%d] GetComputerNameExW returned error %lu\n", NameType, Error);
    ok(BufferW[0] == 0x5555, "[%d] BufferW[0] = 0x%x\n", NameType, BufferW[0]);

    RtlFillMemory(BufferA, sizeof(BufferA), 0x55);
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExA(NameType, BufferA, NULL);
    Error = GetLastError();
    ok(Ret == FALSE, "[%d] GetComputerNameExA returned %d\n", NameType, Ret);
    ok(Error == ERROR_INVALID_PARAMETER, "[%d] GetComputerNameExA returned error %lu\n", NameType, Error);
    ok(BufferA[0] == 0x55, "[%d] BufferA[0] = 0x%x\n", NameType, BufferA[0]);

    /* NULL buffer, zero size */
    Size = 0;
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExW(NameType, NULL, &Size);
    Error = GetLastError();
    ok(Ret == FALSE, "[%d] GetComputerNameExW returned %d\n", NameType, Ret);
    ok(Error == ERROR_MORE_DATA, "[%d] GetComputerNameExW returned error %lu\n", NameType, Error);
    ok(Size == ReferenceLen + 1, "[%d] Got Size %lu, expected %lu\n", NameType, Size, ReferenceLen + 1);

    Size = 0;
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExA(NameType, NULL, &Size);
    Error = GetLastError();
    ok(Ret == FALSE, "[%d] GetComputerNameExA returned %d\n", NameType, Ret);
    ok(Error == ERROR_MORE_DATA, "[%d] GetComputerNameExA returned error %lu\n", NameType, Error);
    ok(Size == ReferenceLen + 1, "[%d] Got Size %lu, expected %lu\n", NameType, Size, ReferenceLen + 1);

    /* non-NULL buffer, zero size */
    RtlFillMemory(BufferW, sizeof(BufferW), 0x55);
    Size = 0;
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExW(NameType, BufferW, &Size);
    Error = GetLastError();
    ok(Ret == FALSE, "[%d] GetComputerNameExW returned %d\n", NameType, Ret);
    ok(Error == ERROR_MORE_DATA, "[%d] GetComputerNameExW returned error %lu\n", NameType, Error);
    ok(Size == ReferenceLen + 1, "[%d] Got Size %lu, expected %lu\n", NameType, Size, ReferenceLen + 1);
    ok(BufferW[0] == 0x5555, "[%d] BufferW[0] = 0x%x\n", NameType, BufferW[0]);

    RtlFillMemory(BufferA, sizeof(BufferA), 0x55);
    Size = 0;
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExA(NameType, BufferA, &Size);
    Error = GetLastError();
    ok(Ret == FALSE, "[%d] GetComputerNameExA returned %d\n", NameType, Ret);
    ok(Error == ERROR_MORE_DATA, "[%d] GetComputerNameExA returned error %lu\n", NameType, Error);
    ok(Size == ReferenceLen + 1, "[%d] Got Size %lu, expected %lu\n", NameType, Size, ReferenceLen + 1);
    ok(BufferA[0] == 0x55, "[%d] BufferA[0] = 0x%x\n", NameType, BufferA[0]);

    /* non-NULL buffer, too small size */
    RtlFillMemory(BufferW, sizeof(BufferW), 0x55);
    Size = ReferenceLen;
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExW(NameType, BufferW, &Size);
    Error = GetLastError();
    ok(Ret == FALSE, "[%d] GetComputerNameExW returned %d\n", NameType, Ret);
    ok(Error == ERROR_MORE_DATA, "[%d] GetComputerNameExW returned error %lu\n", NameType, Error);
    ok(Size == ReferenceLen + 1, "[%d] Got Size %lu, expected %lu\n", NameType, Size, ReferenceLen + 1);
    if (NameType != ComputerNameNetBIOS && NameType != ComputerNamePhysicalNetBIOS)
    {
        if (ReferenceLen == 0)
        {
            ok(BufferW[0] == 0x5555, "[%d] BufferW[0] = 0x%x\n",
               NameType, BufferW[0]);
        }
        else
        {
            ok(BufferW[0] == 0, "[%d] BufferW[0] = 0x%x\n",
               NameType, BufferW[0]);
        }
    }
    ok(BufferW[1] == 0x5555, "[%d] BufferW[1] = 0x%x\n", NameType, BufferW[1]);

    RtlFillMemory(BufferA, sizeof(BufferA), 0x55);
    Size = ReferenceLen;
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExA(NameType, BufferA, &Size);
    Error = GetLastError();
    ok(Ret == FALSE, "[%d] GetComputerNameExA returned %d\n", NameType, Ret);
    ok(Error == ERROR_MORE_DATA, "[%d] GetComputerNameExA returned error %lu\n", NameType, Error);
    ok(Size == ReferenceLen + 1, "[%d] Got Size %lu, expected %lu\n", NameType, Size, ReferenceLen + 1);
    ok(BufferA[0] == 0x55, "[%d] BufferA[0] = 0x%x\n", NameType, BufferA[0]);

    /* non-NULL buffer, exact size */
    RtlFillMemory(BufferW, sizeof(BufferW), 0x55);
    Size = ReferenceLen + 1;
    Ret = GetComputerNameExW(NameType, BufferW, &Size);
    ok(Ret == TRUE, "[%d] GetComputerNameExW returned %d\n", NameType, Ret);
    ok(Size == ReferenceLen, "[%d] Got Size %lu, expected %lu\n", NameType, Size, ReferenceLen + 1);
    ok(BufferW[ReferenceLen] == 0, "[%d] BufferW[ReferenceLen] = 0x%x\n", NameType, BufferW[ReferenceLen]);
    ok(BufferW[ReferenceLen + 1] == 0x5555, "[%d] BufferW[ReferenceLen + 1] = 0x%x\n", NameType, BufferW[ReferenceLen + 1]);
    ok(!wcscmp(BufferW, Reference), "[%d] '%ls' != '%ls'\n", NameType, BufferW, Reference);

    RtlFillMemory(BufferA, sizeof(BufferA), 0x55);
    Size = ReferenceLen + 1;
    Ret = GetComputerNameExA(NameType, BufferA, &Size);
    ok(Ret == TRUE, "[%d] GetComputerNameExA returned %d\n", NameType, Ret);
    ok(Size == ReferenceLen, "[%d] Got Size %lu, expected %lu\n", NameType, Size, ReferenceLen + 1);
    ok(BufferA[ReferenceLen] == 0, "[%d] BufferA[ReferenceLen] = 0x%x\n", NameType, BufferA[ReferenceLen]);
    ok(BufferA[ReferenceLen + 1] == 0x55, "[%d] BufferA[ReferenceLen + 1] = 0x%x\n", NameType, BufferA[ReferenceLen + 1]);
    for (i = 0; i < ReferenceLen; i++)
    {
        if (BufferA[i] != Reference[i])
        {
            ok(0, "[%d] BufferA[%lu] = 0x%x, expected 0x%x\n", NameType, i, BufferA[i], Reference[i]);
        }
    }
}

static
VOID
TestFullyQualified(VOID)
{
    WCHAR Host[128], Domain[128], Expected[260], BufferW[260];
    CHAR BufferA[260];
    DWORD HostLen, DomainLen, ExpectedLen, Needed, Size, Error;
    BOOL Ret;

    Size = RTL_NUMBER_OF(Host);
    Ret = GetComputerNameExW(ComputerNameDnsHostname, Host, &Size);
    ok(Ret == TRUE, "GetComputerNameExW(ComputerNameDnsHostname) returned %d\n", Ret);
    Size = RTL_NUMBER_OF(Domain);
    Ret = Ret && GetComputerNameExW(ComputerNameDnsDomain, Domain, &Size);
    if (!Ret)
    {
        skip("Failed to get the host and domain names\n");
        return;
    }
    HostLen = lstrlenW(Host);
    DomainLen = lstrlenW(Domain);

    /* host.domain, or the host name alone (no trailing dot) when there is no domain */
    lstrcpyW(Expected, Host);
    if (DomainLen)
    {
        lstrcatW(Expected, L".");
        lstrcatW(Expected, Domain);
    }
    ExpectedLen = lstrlenW(Expected);

    /* Windows asks for room for the dot and the domain even when there is none */
    Needed = HostLen + 1 + DomainLen + 1;

    Size = 0;
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExW(ComputerNameDnsFullyQualified, NULL, &Size);
    Error = GetLastError();
    ok(Ret == FALSE, "GetComputerNameExW returned %d\n", Ret);
    ok(Error == ERROR_MORE_DATA, "GetComputerNameExW returned error %lu\n", Error);
    ok(Size == Needed, "Got Size %lu, expected %lu\n", Size, Needed);

    RtlFillMemory(BufferW, sizeof(BufferW), 0x55);
    Size = Needed - 1;
    SetLastError(0xdeadbeef);
    Ret = GetComputerNameExW(ComputerNameDnsFullyQualified, BufferW, &Size);
    Error = GetLastError();
    ok(Ret == FALSE, "GetComputerNameExW returned %d\n", Ret);
    ok(Error == ERROR_MORE_DATA, "GetComputerNameExW returned error %lu\n", Error);
    ok(Size == Needed, "Got Size %lu, expected %lu\n", Size, Needed);

    RtlFillMemory(BufferW, sizeof(BufferW), 0x55);
    Size = Needed;
    Ret = GetComputerNameExW(ComputerNameDnsFullyQualified, BufferW, &Size);
    ok(Ret == TRUE, "GetComputerNameExW returned %d\n", Ret);
    ok(Size == ExpectedLen, "Got Size %lu, expected %lu\n", Size, ExpectedLen);
    ok(!lstrcmpW(BufferW, Expected), "'%ls' != '%ls'\n", BufferW, Expected);
    ok(BufferW[ExpectedLen] == 0, "BufferW[ExpectedLen] = 0x%x\n", BufferW[ExpectedLen]);
    ok(ExpectedLen == 0 || BufferW[ExpectedLen - 1] != L'.', "Trailing dot in '%ls'\n", BufferW);

    RtlFillMemory(BufferA, sizeof(BufferA), 0x55);
    Size = Needed;
    Ret = GetComputerNameExA(ComputerNameDnsFullyQualified, BufferA, &Size);
    ok(Ret == TRUE, "GetComputerNameExA returned %d\n", Ret);
    ok(Size == ExpectedLen, "Got Size %lu, expected %lu\n", Size, ExpectedLen);
    ok(BufferA[ExpectedLen] == 0, "BufferA[ExpectedLen] = 0x%x\n", BufferA[ExpectedLen]);
    ok(ExpectedLen == 0 || BufferA[ExpectedLen - 1] != '.', "Trailing dot in '%s'\n", BufferA);
}

static
LSTATUS
ReadRegistryValue(PCHAR ValueName, PCHAR Value)
{
    INT ErrorCode;
    HKEY ParametersKey;
    DWORD RegType;
    DWORD RegSize = 0;

    /* Open the database path key */
    ErrorCode = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                              "System\\CurrentControlSet\\Services\\Tcpip\\Parameters",
                              0,
                              KEY_READ,
                              &ParametersKey);
    if (ErrorCode == NO_ERROR)
    {
        /* Read the actual path */
        ErrorCode = RegQueryValueExA(ParametersKey,
                                     ValueName,
                                     NULL,
                                     &RegType,
                                     NULL,
                                     &RegSize);
        if (RegSize)
        {
            /* Read the actual path */
            ErrorCode = RegQueryValueExA(ParametersKey,
                                         ValueName,
                                         NULL,
                                         &RegType,
                                         (LPBYTE)Value,
                                         &RegSize);
        }

        /* Close the key */
        RegCloseKey(ParametersKey);
    }
    return ErrorCode;
}

static
LSTATUS
ReadRegistryComputerNameValue(PCHAR ValueName, PCHAR Value)
{
    INT ErrorCode;
    HKEY ParametersKey;
    DWORD RegType;
    DWORD RegSize = 0;

    /* Open the database path key */
    ErrorCode = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                              "System\\CurrentControlSet\\Control\\ComputerName\\ActiveComputerName",
                              0,
                              KEY_READ,
                              &ParametersKey);
    if (ErrorCode == NO_ERROR)
    {
        /* Read the actual path */
        ErrorCode = RegQueryValueExA(ParametersKey,
                                     ValueName,
                                     NULL,
                                     &RegType,
                                     NULL,
                                     &RegSize);
        if (RegSize)
        {
            /* Read the actual path */
            ErrorCode = RegQueryValueExA(ParametersKey,
                                         ValueName,
                                         NULL,
                                         &RegType,
                                         (LPBYTE)Value,
                                         &RegSize);
        }

        /* Close the key */
        RegCloseKey(ParametersKey);
    }
    return ErrorCode;
}

static
LSTATUS
WriteRegistryValue(PCHAR ValueName, PCHAR Value)
{
    INT ErrorCode;
    HKEY ParametersKey;

    /* Open the database path key */
    ErrorCode = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                              "System\\CurrentControlSet\\Services\\Tcpip\\Parameters",
                              0,
                              KEY_WRITE,
                              &ParametersKey);
    if (ErrorCode == NO_ERROR)
    {
        /* Read the actual path */
        ErrorCode = RegSetValueExA(ParametersKey,
                                   ValueName,
                                   0,
                                   REG_SZ,
                                   (LPBYTE)Value,
                                   lstrlenA(Value) + 1);

        /* Close the key */
        RegCloseKey(ParametersKey);
    }
    return ErrorCode;
}

static
LSTATUS
DeleteRegistryValue(PCHAR ValueName)
{
    INT ErrorCode;
    HKEY ParametersKey;

    /* Open the database path key */
    ErrorCode = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                              "System\\CurrentControlSet\\Services\\Tcpip\\Parameters",
                              0,
                              KEY_WRITE,
                              &ParametersKey);
    if (ErrorCode == NO_ERROR)
    {
        /* Read the actual path */
        ErrorCode = RegDeleteValueA(ParametersKey, ValueName);

        /* Close the key */
        RegCloseKey(ParametersKey);
    }
    return ErrorCode;
}

/* If this test crashes it might end up with wrong host and/or domain name in registry! */
static
VOID
TestReturnValues()
{
    CHAR OrigNetBIOS[128];
    CHAR OrigHostname[128];
    CHAR OrigDomainName[128];
    CHAR OrigDhcpHostname[128];
    CHAR OrigDhcpDomainName[128];
    BOOL OrigNetBIOSExists;
    BOOL OrigHostnameExists;
    BOOL OrigDomainNameExists;
    BOOL OrigDhcpHostnameExists;
    BOOL OrigDhcpDomainNameExists;
    CHAR ComputerName[128];
    DWORD ComputerNameSize = 0;
    INT ErrorCode;
    BOOL Ret;

    memset(OrigNetBIOS, 0, sizeof(OrigNetBIOS));
    memset(OrigHostname, 0, sizeof(OrigHostname));
    memset(OrigDomainName, 0, sizeof(OrigDomainName));
    memset(OrigDhcpHostname, 0, sizeof(OrigDhcpHostname));
    memset(OrigDhcpDomainName, 0, sizeof(OrigDhcpDomainName));
    /* read current registry values */
    ErrorCode = ReadRegistryComputerNameValue("ComputerName", OrigNetBIOS);
    ok(ErrorCode == ERROR_SUCCESS, "Failed to read registry key ComputerName %d\n", ErrorCode);
    OrigNetBIOSExists = ErrorCode == STATUS_SUCCESS;
    ErrorCode = ReadRegistryValue("Hostname", OrigHostname);
    ok(ErrorCode == ERROR_SUCCESS, "Failed to read registry key Hostname %d\n", ErrorCode);
    OrigHostnameExists = ErrorCode == STATUS_SUCCESS;
    ErrorCode = ReadRegistryValue("Domain", OrigDomainName);
    ok(ErrorCode == ERROR_SUCCESS || ErrorCode == ERROR_FILE_NOT_FOUND, "Failed to read registry key DomainName %d\n", ErrorCode);
    OrigDomainNameExists = ErrorCode == STATUS_SUCCESS;
    ErrorCode = ReadRegistryValue("DhcpHostname", OrigDhcpHostname);
    ok(ErrorCode == ERROR_SUCCESS || ErrorCode == ERROR_FILE_NOT_FOUND, "Failed to read registry key DhcpHostname %d\n", ErrorCode);
    OrigDhcpHostnameExists = ErrorCode == STATUS_SUCCESS;
    ErrorCode = ReadRegistryValue("DhcpDomain", OrigDhcpDomainName);
    ok(ErrorCode == ERROR_SUCCESS || ErrorCode == ERROR_FILE_NOT_FOUND, "Failed to read registry key DhcpDomainName %d\n", ErrorCode);
    OrigDhcpDomainNameExists = ErrorCode == STATUS_SUCCESS;

    trace("Starting values:\n");
    trace("NetBIOS: %s, exists %s\n", OrigNetBIOS, OrigNetBIOSExists ? "yes" : "no");
    trace("Hostname: %s, exists %s\n", OrigHostname, OrigHostnameExists ? "yes" : "no");
    trace("Domain: %s, exists %s\n", OrigDomainName, OrigDomainNameExists ? "yes" : "no");
    trace("DhcpHostname: %s, exists %s\n", OrigDhcpHostnameExists ? OrigDhcpHostname : "", OrigDhcpHostnameExists ? "yes" : "no");
    trace("DhcpDomain: %s, exists %s\n", OrigDhcpDomainNameExists ? OrigDhcpDomainName : "", OrigDhcpDomainNameExists ? "yes" : "no");

    /* ComputerNamePhysicalNetBIOS */
    ComputerNameSize = 0;
    StartSeh()
        GetComputerNameExA(ComputerNamePhysicalNetBIOS, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNamePhysicalNetBIOS, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNamePhysicalNetBIOS) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigNetBIOS) == 0, "ComputerNamePhysicalNetBIOS doesn't match registry value '%s' != '%s'\n", ComputerName, OrigNetBIOS);
    EndSeh(STATUS_SUCCESS);

    /* ComputerNamePhysicalDnsHostname */
    ComputerNameSize = 0;
    StartSeh()
        GetComputerNameExA(ComputerNamePhysicalDnsHostname, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNamePhysicalDnsHostname, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNamePhysicalDnsHostname) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigHostname) == 0, "ComputerNamePhysicalDnsHostname doesn't match registry value '%s' != '%s'\n", ComputerName, OrigHostname);
    EndSeh(STATUS_SUCCESS);

    /* ComputerNamePhysicalDnsDomain */
    ComputerNameSize = 0;
    StartSeh()
        GetComputerNameExA(ComputerNamePhysicalDnsDomain, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNamePhysicalDnsDomain, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNamePhysicalDnsDomain) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigDomainName) == 0, "ComputerNamePhysicalDnsDomain doesn't match registry value '%s' != '%s'\n", ComputerName, OrigDomainName);
    EndSeh(STATUS_SUCCESS);
    ComputerNameSize = 0;

    /* ComputerNameNetBIOS */
    StartSeh()
        GetComputerNameExA(ComputerNameNetBIOS, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNameNetBIOS, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNameNetBIOS) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigNetBIOS) == 0, "ComputerNameNetBIOS doesn't match registry value '%s' != '%s'\n", ComputerName, OrigNetBIOS);
    EndSeh(STATUS_SUCCESS);

    /* ComputerNameDnsHostname */
    ComputerNameSize = 0;
    StartSeh()
        GetComputerNameExA(ComputerNameDnsHostname, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNameDnsHostname, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNameDnsHostname) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigHostname) == 0, "ComputerNameDnsHostname doesn't match registry value '%s' != '%s'\n", ComputerName, OrigHostname);
    EndSeh(STATUS_SUCCESS);

    /* ComputerNameDnsDomain */
    ComputerNameSize = 0;
    StartSeh()
        GetComputerNameExA(ComputerNameDnsDomain, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNameDnsDomain, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNameDnsDomain) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigDomainName) == 0, "ComputerNameDnsDomain doesn't match registry value '%s' != '%s'\n", ComputerName, OrigDomainName);
    EndSeh(STATUS_SUCCESS);

    ErrorCode = WriteRegistryValue("DhcpHostname", "testdhcproshost");
    ok(ErrorCode == ERROR_SUCCESS, "Failed to write registry key DhcpHostname %d\n", ErrorCode);
    ErrorCode = WriteRegistryValue("DhcpDomain", "testrosdomain");
    ok(ErrorCode == ERROR_SUCCESS, "Failed to write registry key DhcpDomainName %d\n", ErrorCode);

    /* ComputerNamePhysicalNetBIOS */
    ComputerNameSize = 0;
    StartSeh()
        GetComputerNameExA(ComputerNamePhysicalNetBIOS, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNamePhysicalNetBIOS, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNamePhysicalNetBIOS) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigNetBIOS) == 0, "ComputerNamePhysicalNetBIOS doesn't match registry value '%s' != '%s'\n", ComputerName, OrigNetBIOS);
    EndSeh(STATUS_SUCCESS);

    /* ComputerNamePhysicalDnsHostname */
    ComputerNameSize = 0;
    StartSeh()
        GetComputerNameExA(ComputerNamePhysicalDnsHostname, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNamePhysicalDnsHostname, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNamePhysicalDnsHostname) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigHostname) == 0, "ComputerNamePhysicalDnsHostname doesn't match registry value '%s' != '%s'\n", ComputerName, OrigHostname);
    EndSeh(STATUS_SUCCESS);

    /* ComputerNamePhysicalDnsDomain */
    ComputerNameSize = 0;
    StartSeh()
        GetComputerNameExA(ComputerNamePhysicalDnsDomain, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNamePhysicalDnsDomain, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNamePhysicalDnsDomain) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigDomainName) == 0, "ComputerNamePhysicalDnsDomain doesn't match registry value '%s' != '%s'\n", ComputerName, OrigDomainName);
    EndSeh(STATUS_SUCCESS);
    ComputerNameSize = 0;

    /* ComputerNameNetBIOS */
    StartSeh()
        GetComputerNameExA(ComputerNameNetBIOS, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNameNetBIOS, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNameNetBIOS) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigNetBIOS) == 0, "ComputerNameNetBIOS doesn't match registry value '%s' != '%s'\n", ComputerName, OrigNetBIOS);
    EndSeh(STATUS_SUCCESS);

    /* ComputerNameDnsHostname */
    ComputerNameSize = 0;
    StartSeh()
        GetComputerNameExA(ComputerNameDnsHostname, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNameDnsHostname, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNameDnsHostname) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigHostname) == 0, "ComputerNameDnsHostname doesn't match registry value '%s' != '%s'\n", ComputerName, OrigHostname);
    EndSeh(STATUS_SUCCESS);

    /* ComputerNameDnsDomain */
    ComputerNameSize = 0;
    StartSeh()
        GetComputerNameExA(ComputerNameDnsDomain, ComputerName, &ComputerNameSize);
        if (ComputerNameSize)
        {
            Ret = GetComputerNameExA(ComputerNameDnsDomain, ComputerName, &ComputerNameSize);
            ok(Ret, "GetComputerNameExA(ComputerNameDnsDomain) failed with %ld\n", GetLastError());
        }
        else
        {
            memset(ComputerName, 0, sizeof(ComputerName));
        }
        ok(strcmp(ComputerName, OrigDomainName) == 0, "ComputerNameDnsDomain doesn't match registry value '%s' != '%s'\n", ComputerName, OrigDomainName);
    EndSeh(STATUS_SUCCESS);

    /* restore registry values */
    if (OrigDhcpHostnameExists)
        ErrorCode = WriteRegistryValue("DhcpHostname", OrigDhcpHostname);
    else
        ErrorCode = DeleteRegistryValue("DhcpHostname");
    ok(ErrorCode == ERROR_SUCCESS, "Failed to restore registry key DhcpHostname %d\n", ErrorCode);
    if (OrigDhcpDomainNameExists)
        ErrorCode = WriteRegistryValue("DhcpDomain", OrigDhcpDomainName);
    else
        ErrorCode = DeleteRegistryValue("DhcpDomain");
    ok(ErrorCode == ERROR_SUCCESS, "Failed to restore registry key DhcpDomainName %d\n", ErrorCode);
}

START_TEST(GetComputerNameEx)
{
    TestGetComputerNameEx(ComputerNameNetBIOS);
    TestGetComputerNameEx(ComputerNameDnsHostname);
    TestGetComputerNameEx(ComputerNameDnsDomain);
    /* The generic test does not apply: Windows sizes the buffer for a trailing dot it does not return */
    TestFullyQualified();
    TestGetComputerNameEx(ComputerNamePhysicalNetBIOS);
    TestGetComputerNameEx(ComputerNamePhysicalDnsHostname);
    TestGetComputerNameEx(ComputerNamePhysicalDnsDomain);
    //TestGetComputerNameEx(ComputerNamePhysicalDnsFullyQualified);
    TestReturnValues();
}
