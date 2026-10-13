/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Test for the RegGetvalueW API
 * PROGRAMMER:      Jérôme Gardou <jerome.gardou@reactos.org>
 */

#include "precomp.h"
#include <pseh/pseh2.h>

#define GUARD_BYTE  0xCC

/*
 * Enumerates the first value of hKey into a cbData-byte buffer that starts a
 * page whose preceding page is inaccessible and that is followed by guard bytes.
 */
static BOOL
EnumIntoBoundedBuffer(HKEY hKey, BOOL bAnsi, DWORD cbData, LONG *pRet, DWORD *pcbOut, BYTE *pData, BOOL *pFaulted, BOOL *pGuardOk)
{
    SYSTEM_INFO si;
    BYTE *Pages, *Data;
    DWORD i, dwOld, dwType, cchName;
    WCHAR Name[16];
    CHAR NameA[16];

    *pRet = -2;
    *pcbOut = cbData;
    *pFaulted = FALSE;
    *pGuardOk = TRUE;

    GetSystemInfo(&si);
    Pages = VirtualAlloc(NULL, 2 * si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!Pages)
    {
        skip("VirtualAlloc failed with %lu\n", GetLastError());
        return FALSE;
    }
    if (!VirtualProtect(Pages, si.dwPageSize, PAGE_NOACCESS, &dwOld))
    {
        skip("VirtualProtect failed with %lu\n", GetLastError());
        VirtualFree(Pages, 0, MEM_RELEASE);
        return FALSE;
    }
    Data = Pages + si.dwPageSize;
    FillMemory(Data, si.dwPageSize, GUARD_BYTE);

    cchName = _countof(Name);
    _SEH2_TRY
    {
        if (bAnsi)
            *pRet = RegEnumValueA(hKey, 0, NameA, &cchName, NULL, &dwType, Data, pcbOut);
        else
            *pRet = RegEnumValueW(hKey, 0, Name, &cchName, NULL, &dwType, Data, pcbOut);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        *pFaulted = TRUE;
        *pRet = -1;
    }
    _SEH2_END;

    for (i = cbData; i < si.dwPageSize; i++)
    {
        if (Data[i] != GUARD_BYTE)
        {
            *pGuardOk = FALSE;
            break;
        }
    }
    CopyMemory(pData, Data, min(cbData, 8));

    VirtualFree(Pages, 0, MEM_RELEASE);
    return TRUE;
}

static void
TestOddStringData(HKEY hParent)
{
    static const struct
    {
        DWORD cbValue;
        DWORD cbBuffer;
    } Tests[] =
    {
        { 1, 1 }, { 1, 2 }, { 1, 3 }, { 3, 3 }, { 3, 4 }, { 3, 5 }, { 2, 2 }, { 2, 4 }, { 0, 2 },
    };
    static const BYTE Value[] = { 'A', 0, 'B', 0 };
    static const DWORD Types[] = { REG_SZ, REG_EXPAND_SZ, REG_MULTI_SZ };
    HKEY hKey;
    LONG ErrorCode, Ret;
    DWORD i, j, k, dwType, cbStored, cbOut;
    BYTE Data[8];
    BOOL bFaulted, bGuardOk;

    for (k = 0; k < 2; k++)
    for (j = 0; j < _countof(Types); j++)
    {
        for (i = 0; i < _countof(Tests); i++)
        {
            ErrorCode = RegCreateKeyW(hParent, L"Odd", &hKey);
            ok_dec(ErrorCode, ERROR_SUCCESS);
            if (ErrorCode != ERROR_SUCCESS)
                return;

            ErrorCode = RegSetValueExW(hKey, L"V", 0, Types[j], Value, Tests[i].cbValue);
            ok_dec(ErrorCode, ERROR_SUCCESS);
            cbStored = 0;
            ErrorCode = RegQueryValueExW(hKey, L"V", NULL, &dwType, NULL, &cbStored);
            ok(ErrorCode == ERROR_SUCCESS && dwType == Types[j] && cbStored == Tests[i].cbValue,
               "stored: ret %ld type %lu size %lu\n", ErrorCode, dwType, cbStored);

            /* k == 1: the ANSI variant, which converts what RegEnumValueW returns */
            if (EnumIntoBoundedBuffer(hKey, k == 1, Tests[i].cbBuffer, &Ret, &cbOut, Data, &bFaulted, &bGuardOk))
            {
                ok(!bFaulted, "%c type %lu value %lu buffer %lu: faulted\n",
                   k ? 'A' : 'W', Types[j], Tests[i].cbValue, Tests[i].cbBuffer);
                ok(bGuardOk, "%c type %lu value %lu buffer %lu: wrote past the buffer\n",
                   k ? 'A' : 'W', Types[j], Tests[i].cbValue, Tests[i].cbBuffer);
                ok(Ret == ERROR_SUCCESS, "%c type %lu value %lu buffer %lu: ret %ld\n",
                   k ? 'A' : 'W', Types[j], Tests[i].cbValue, Tests[i].cbBuffer, Ret);
                if (k == 0)
                {
                    ok(cbOut == Tests[i].cbValue, "W type %lu value %lu buffer %lu: size %lu\n",
                       Types[j], Tests[i].cbValue, Tests[i].cbBuffer, cbOut);
                    if (Tests[i].cbValue > 0)
                        ok(Data[0] == 'A', "W type %lu value %lu buffer %lu: first byte 0x%02x\n",
                           Types[j], Tests[i].cbValue, Tests[i].cbBuffer, Data[0]);
                    /* After an unterminated string ReactOS adds a terminator when a whole one fits;
                     * Windows Server 2008 R2 and Windows 10 leave those bytes alone */
                    if (Tests[i].cbValue == 2 && Tests[i].cbBuffer == 4)
                        ok((Data[2] == 0 && Data[3] == 0) || (Data[2] == GUARD_BYTE && Data[3] == GUARD_BYTE),
                           "W type %lu: bytes after the data: %02x %02x\n", Types[j], Data[2], Data[3]);
                }
                else
                {
                    /* One ANSI character per whole Unicode character, as on Windows */
                    ok(cbOut == Tests[i].cbValue / sizeof(WCHAR), "A type %lu value %lu buffer %lu: size %lu\n",
                       Types[j], Tests[i].cbValue, Tests[i].cbBuffer, cbOut);
                }
            }

            RegDeleteKeyW(hKey, L"");
            RegCloseKey(hKey);
        }
    }
}

START_TEST(RegEnumValueW)
{
    LONG ErrorCode;
    HKEY TestKey;
    ULONG Data;
    DWORD NameLength, DataLength;
    DWORD DataType;
    WCHAR NameBuffer[7];

    /* Create our Test Key */
    ErrorCode = RegCreateKeyW( HKEY_CURRENT_USER, L"Software\\ReactOS_apitest", &TestKey );
    ok_dec(ErrorCode, ERROR_SUCCESS);

    /* All NULL is invalid */
    ErrorCode = RegEnumValueW(TestKey, 0, NULL, NULL, NULL, NULL, NULL, NULL);
    ok_dec(ErrorCode, ERROR_INVALID_PARAMETER);

    /* Asking for the buffer length is not enough. */
    NameLength = 8;
    ErrorCode = RegEnumValueW(TestKey, 0, NULL, &NameLength, NULL, NULL, NULL, NULL);
    ok_dec(ErrorCode, ERROR_INVALID_PARAMETER);

    /* Or maybe it is, you just have to nicely ask */
    NameLength = 0;
    ErrorCode = RegEnumValueW(TestKey, 0, NULL, &NameLength, NULL, NULL, NULL, NULL);
    ok_dec(ErrorCode, ERROR_INVALID_PARAMETER);

    /* Name buffer alone is also prohibited */
    ErrorCode = RegEnumValueW(TestKey, 0, NameBuffer, NULL, NULL, NULL, NULL, NULL);
    ok_dec(ErrorCode, ERROR_INVALID_PARAMETER);

    /* You need to ask for both a minima */
    ErrorCode = RegEnumValueW(TestKey, 0, NameBuffer, &NameLength, NULL, NULL, NULL, NULL);
    ok_dec(ErrorCode, ERROR_NO_MORE_ITEMS);

    /* What if I only want to know the value type ? */
    ErrorCode = RegEnumValueW(TestKey, 0, NULL, NULL, &DataType, NULL, NULL, NULL);
    ok_dec(ErrorCode, ERROR_INVALID_PARAMETER);

    /* Set a value */
    Data = 0xF00DF00D;
    ErrorCode = RegSetValueExW(TestKey, L"Value1", 0, REG_BINARY, (LPBYTE)&Data, sizeof(Data));
    ok_dec(ErrorCode, ERROR_SUCCESS);

    /* Try various combinations of Arguments */
    NameLength = 7;
    ErrorCode = RegEnumValueW(TestKey, 0, NULL, &NameLength, NULL, NULL, NULL, NULL);
    ok_dec(ErrorCode, ERROR_INVALID_PARAMETER);
    /* Provide a buffer this time */
    ErrorCode = RegEnumValueW(TestKey, 0, NameBuffer, &NameLength, NULL, NULL, NULL, NULL);
    ok_dec(ErrorCode, ERROR_SUCCESS);
    /* This doesn't include the NULL terminator */
    ok_dec(NameLength, 6);
    /* But the string is NULL terminated */
    ok_hex(NameBuffer[6], L'\0');
    ok(wcscmp(NameBuffer, L"Value1") == 0, "%S\n", NameBuffer);

    /* See if skipping the NULL value is a problem */
    NameLength = 6;
    memset(NameBuffer, 0xBA, sizeof(NameBuffer));
    ErrorCode = RegEnumValueW(TestKey, 0, NameBuffer, &NameLength, NULL, NULL, NULL, NULL);
    ok_dec(ErrorCode, ERROR_MORE_DATA);
    /* In fact, this is unchanged */
    ok_dec(NameLength, 6);
    /* And the string is untouched */
    ok_hex(NameBuffer[6], 0xBABA);
    ok_hex(NameBuffer[5], 0xBABA);
    ok_hex(NameBuffer[4], 0xBABA);
    ok_hex(NameBuffer[3], 0xBABA);
    ok_hex(NameBuffer[2], 0xBABA);
    ok_hex(NameBuffer[1], 0xBABA);
    ok_hex(NameBuffer[0], 0xBABA);

    /* Of course, anything smaller is an outrage */
    NameLength = 5;
    memset(NameBuffer, 0xBA, sizeof(NameBuffer));
    ErrorCode = RegEnumValueW(TestKey, 0, NameBuffer, &NameLength, NULL, NULL, NULL, NULL);
    ok_dec(ErrorCode, ERROR_MORE_DATA);
    /* Untouched... */
    ok_dec(NameLength, 5);
    /* And the string is untouched */
    ok_hex(NameBuffer[6], 0xBABA);
    ok_hex(NameBuffer[5], 0xBABA);
    ok_hex(NameBuffer[4], 0xBABA);
    ok_hex(NameBuffer[3], 0xBABA);
    ok_hex(NameBuffer[2], 0xBABA);
    ok_hex(NameBuffer[1], 0xBABA);
    ok_hex(NameBuffer[0], 0xBABA);

    /* Of course, asking for data without caring for the name would be strange... */
    DataLength = 0;
    ErrorCode = RegEnumValueW(TestKey, 0, NULL, NULL, NULL, NULL, NULL, &DataLength);
    ok_dec(ErrorCode, ERROR_INVALID_PARAMETER);

    DataLength = 0;
    NameLength = 7;
    ErrorCode = RegEnumValueW(TestKey, 0, NameBuffer, &NameLength, NULL, &DataType, NULL, &DataLength);
    ok_dec(ErrorCode, ERROR_SUCCESS);
    ok_dec(DataLength, sizeof(Data));
    ok_hex(DataType, REG_BINARY);

    /* Same, but this time with NULL data buffer and non-zero data size */
    DataLength = 2;
    NameLength = 7;
    DataType = 0x01234567;
    ErrorCode = RegEnumValueW(TestKey, 0, NameBuffer, &NameLength, NULL, &DataType, NULL, &DataLength);
    ok_dec(ErrorCode, ERROR_SUCCESS);
    ok_dec(DataLength, sizeof(Data));
    ok_hex(DataType, REG_BINARY);

    /* Same, but this time with data buffer and shrunk data size */
    DataLength = 2;
    Data = 0;
    DataType = 0x01234567;
    NameLength = 7;
    ErrorCode = RegEnumValueW(TestKey, 0, NameBuffer, &NameLength, NULL, &DataType, (LPBYTE)&Data, &DataLength);
    ok_dec(ErrorCode, ERROR_MORE_DATA);
    ok_dec(DataLength, sizeof(Data));
    ok_hex(Data, 0);
    ok_hex(DataType, REG_BINARY);

    /* Put the right parameters */
    DataLength = sizeof(Data);
    NameLength = 7;
    Data = 0;
    DataType = 0x01234567;
    ErrorCode = RegEnumValueW(TestKey, 0, NameBuffer, &NameLength, NULL, &DataType, (LPBYTE)&Data, &DataLength);
    ok_dec(ErrorCode, ERROR_SUCCESS);
    ok_dec(DataLength, sizeof(Data));
    ok_hex(Data, 0xF00DF00D);
    ok_hex(DataType, REG_BINARY);

    TestOddStringData(TestKey);

    /* Delete the key */
    ErrorCode = RegDeleteKeyW(TestKey, L"");
    ok_dec(ErrorCode, ERROR_SUCCESS);
    RegCloseKey(TestKey);
}

