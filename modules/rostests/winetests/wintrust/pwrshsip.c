/*
 * Test for the PowerShell script subject interface package
 *
 * Copyright 2026 reactos-nitro contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdio.h>
#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mssip.h>

#include "wine/test.h"

#ifndef TRUST_E_BAD_DIGEST
#define TRUST_E_BAD_DIGEST _HRESULT_TYPEDEF_(0x80096010L)
#endif

static const GUID pwsh_subject = {0x603bcc1f, 0x4b59, 0x4e08, {0xb7, 0x24, 0xd2, 0xc6, 0x29, 0x7e, 0xf3, 0x51}};

/* A script signed with a throw-away certificate that chains to no trusted root (its key is gone) */
static const char signed_script[] =
    "Write-Host \"hi\"\r\n"
    "# comment\r\n"
    "\r\n"
    "# SIG # Begin signature block\r\n"
    "# MIIFpwYJKoZIhvcNAQcCoIIFmDCCBZQCAQExDzANBglghkgBZQMEAgEFADB5Bgor\r\n"
    "# BgEEAYI3AgEEoGswaTA0BgorBgEEAYI3AgEeMCYCAwEAAAQQH8w7YFlLCE63JNLG\r\n"
    "# KX7zUQIBAAIBAAIBAAIBAAIBADAxMA0GCWCGSAFlAwQCAQUABCDesbZhWrKd81eZ\r\n"
    "# /ykkiBn2teCKLLodLsbcVVcneZsmZaCCAxYwggMSMIIB+qADAgECAhBwEMVOaZ/A\r\n"
    "# rEX7rKzSORdcMA0GCSqGSIb3DQEBCwUAMCExHzAdBgNVBAMMFnBzc2VjIHRocm93\r\n"
    "# YXdheSBzaWduZXIwHhcNMjYxMDMxMDU0MjI4WhcNMjYxMTAyMDU1MjI4WjAhMR8w\r\n"
    "# HQYDVQQDDBZwc3NlYyB0aHJvd2F3YXkgc2lnbmVyMIIBIjANBgkqhkiG9w0BAQEF\r\n"
    "# AAOCAQ8AMIIBCgKCAQEAui66OwR+BCMWM0/47wsLDJmL0toeS0TXx7HMrURmfTQv\r\n"
    "# bEHLZYmkZzpXkfo44esn8gK9PYIXNfGGwnbGfE7Kiry6oRGn/BszKkXGNBZNoVi/\r\n"
    "# NLvZXCIjt5YHuhwQZHbHbVvmeLeNWvbt5AyDw/vnkhDg0W2lvg8xwO7qZHnMxmKX\r\n"
    "# SXr73i93vFww/2QRQ748gPA+uhD4pAyQ1zMRWiYzQwjIOctGg1xNYKeVNUiZLLs/\r\n"
    "# 6etlAjczkwTDbwyYEZNXJVPjZ8C/ND8yIXsljPMPxzS6zUtKkq0cVNXEu5GIRWPX\r\n"
    "# c/wKeNIzUV+LM4U6pPmQzxWuscids92+5pRAUS2zkQIDAQABo0YwRDAOBgNVHQ8B\r\n"
    "# Af8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwMwHQYDVR0OBBYEFMXRpC9LazOm\r\n"
    "# TGcnDJKDRUZfy8AiMA0GCSqGSIb3DQEBCwUAA4IBAQAPTf2hyyV6WzO9xlSv8iJ/\r\n"
    "# LjU+1butY6lEsCC5aLsp8hUVuTDEHLLya1jwxXLsmgqshynFJzRE7kkgiLBdkwcW\r\n"
    "# 3nd/9ssKUzNp7y77xhF4P6KtrWtVQOBGO9fTIZ9DwR0a8X0dxLsuamFh8o2HW/Nh\r\n"
    "# 3ue2cYLw8MkaTFloVJkIaO5zg2sHO+rxg83Ts7kZaYFhEYvfm/yEQA2eE8SlbwVj\r\n"
    "# tUZ+9Zdej4E1uAa1P0FNdoecUMOdvuHkcKWOWPXiIF2FVsEpQ280S6g/j5SENZ+l\r\n"
    "# 3lIh0nRKQAyGjWmQkX0FaEGCe3gFPZ5gddzY9dVzXsXDij4Tv1CE2lfi4IPEO8uA\r\n"
    "# MYIB5zCCAeMCAQEwNTAhMR8wHQYDVQQDDBZwc3NlYyB0aHJvd2F3YXkgc2lnbmVy\r\n"
    "# AhBwEMVOaZ/ArEX7rKzSORdcMA0GCWCGSAFlAwQCAQUAoIGEMBgGCisGAQQBgjcC\r\n"
    "# AQwxCjAIoAKAAKECgAAwGQYJKoZIhvcNAQkDMQwGCisGAQQBgjcCAQQwHAYKKwYB\r\n"
    "# BAGCNwIBCzEOMAwGCisGAQQBgjcCARUwLwYJKoZIhvcNAQkEMSIEII2dKFxDM5HN\r\n"
    "# xqU8z1NSbtLtiH1iKzSdkeKjcXSwfTVEMA0GCSqGSIb3DQEBAQUABIIBADmK4ps5\r\n"
    "# RkNIwuwZFIj5jr2E+CM3uA1iOVQHbemA7m4hs178UkpMlmch71KaK+kzzN1c9kgR\r\n"
    "# XPZ5b9Gnn8Lsdfn+DR8Z9l9zgwAp6CJDcCGswOdPKxpCHpj0NVP2EbVLNFK7T2Di\r\n"
    "# pVCXozDmyWiD/RPkbg1wIxF5FTErDzVIzjSSsPTz9NFslypNEZJ+6Yapfp/VpQIw\r\n"
    "# yhzDdpfxHNzje2HE4LPZLf8rrqBIQoOPLr4ra3RwVxZ/fDGIc3ZW1LWgDxoDxmvm\r\n"
    "# CFo8QES32kzNhxpegyQEVXxsxrcqCXvibc+UPzC60ZEWW6SAf+/hKjdlTLsPmMVL\r\n"
    "# 3arnVjO35vOGEPU=\r\n"
    "# SIG # End signature block\r\n";

static const char script_text[] = "Write-Host \"hi\"\r\n# comment\r\n";
static const BYTE script_sha256[] = {0xde,0xb1,0xb6,0x61,0x5a,0xb2,0x9d,0xf3,0x57,0x99,0xff,0x29,0x24,0x88,0x19,0xf6,0xb5,0xe0,0x8a,0x2c,0xba,0x1d,0x2e,0xc6,0xdc,0x55,0x57,0x27,0x79,0x9b,0x26,0x65};

static void write_file(const WCHAR *path, const void *data, DWORD size)
{
    DWORD written;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    ok(file != INVALID_HANDLE_VALUE, "CreateFile failed %lu\n", GetLastError());
    WriteFile(file, data, size, &written, NULL);
    CloseHandle(file);
}

static LONG verify_file(const WCHAR *path)
{
    WINTRUST_FILE_INFO file_info = {sizeof(file_info)};
    WINTRUST_DATA data = {sizeof(data)};
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG ret;

    file_info.pcwszFilePath = path;
    data.dwUIChoice = WTD_UI_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file_info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    ret = WinVerifyTrust(NULL, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &action, &data);
    return ret;
}

static LONG verify_blob(const char *content, DWORD size)
{
    WINTRUST_BLOB_INFO blob = {sizeof(blob)};
    WINTRUST_DATA data = {sizeof(data)};
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG ret;

    blob.gSubject = pwsh_subject;
    blob.pcwszDisplayName = L"script.ps1";
    blob.cbMemObject = size;
    blob.pbMemObject = (BYTE *)content;
    data.dwUIChoice = WTD_UI_NONE;
    data.dwUnionChoice = WTD_CHOICE_BLOB;
    data.pBlob = &blob;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    ret = WinVerifyTrust(NULL, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &action, &data);
    return ret;
}

static void test_verify(const WCHAR *path)
{
    char *tampered;
    LONG ret;
    GUID subject;
    BOOL found;

    write_file(path, signed_script, sizeof(signed_script) - 1);

    found = CryptSIPRetrieveSubjectGuid(path, NULL, &subject);
    ok(found, "CryptSIPRetrieveSubjectGuid failed %lu\n", GetLastError());
    if (found) ok(IsEqualGUID(&subject, &pwsh_subject), "got %s\n", wine_dbgstr_guid(&subject));

    ret = verify_file(path);
    ok(ret == CERT_E_UNTRUSTEDROOT, "got %#lx\n", ret);

    ret = verify_blob(signed_script, sizeof(signed_script) - 1);
    ok(ret == CERT_E_UNTRUSTEDROOT, "got %#lx\n", ret);

    /* change the text */
    tampered = HeapAlloc(GetProcessHeap(), 0, sizeof(signed_script));
    memcpy(tampered, signed_script, sizeof(signed_script));
    memcpy(tampered + 12, "ho", 2);
    ok(!memcmp(tampered, "Write-Host \"ho\"", 15), "wrong offset\n");
    write_file(path, tampered, sizeof(signed_script) - 1);
    ret = verify_file(path);
    ok(ret == TRUST_E_BAD_DIGEST, "got %#lx\n", ret);
    ret = verify_blob(tampered, sizeof(signed_script) - 1);
    ok(ret == TRUST_E_BAD_DIGEST, "got %#lx\n", ret);
    HeapFree(GetProcessHeap(), 0, tampered);

    /* no signature */
    write_file(path, script_text, sizeof(script_text) - 1);
    ret = verify_file(path);
    ok(ret == TRUST_E_NOSIGNATURE, "got %#lx\n", ret);
    ret = verify_blob(script_text, sizeof(script_text) - 1);
    ok(ret == TRUST_E_NOSIGNATURE, "got %#lx\n", ret);
}

static void test_sip(const WCHAR *path)
{
    static const BYTE message[] = {0x30, 0x03, 0x02, 0x01, 0x01, 0x30, 0x82, 0x01, 0x00, 0xde, 0xad, 0xbe, 0xef};
    SIP_DISPATCH_INFO dispatch = {sizeof(dispatch)};
    SIP_SUBJECTINFO info = {sizeof(info)};
    SIP_INDIRECT_DATA *indirect;
    BYTE buffer[256], *buf;
    DWORD size, encoding, index, read;
    char file[sizeof(signed_script) + 256];
    HANDLE handle;
    GUID subject = pwsh_subject;
    HCRYPTPROV prov;
    BOOL ret;

    ret = CryptSIPLoad(&subject, 0, &dispatch);
    ok(ret, "CryptSIPLoad failed %lu\n", GetLastError());
    if (!ret) return;

    ret = CryptAcquireContextW(&prov, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    ok(ret, "CryptAcquireContext failed %lu\n", GetLastError());

    write_file(path, script_text, sizeof(script_text) - 1);
    info.pgSubjectType = &subject;
    info.pwsFileName = path;
    info.DigestAlgorithm.pszObjId = (char *)"2.16.840.1.101.3.4.2.1";
    info.hFile = INVALID_HANDLE_VALUE;
    info.hProv = prov;

    /* the digest algorithm is required */
    size = 0;
    info.DigestAlgorithm.pszObjId = NULL;
    SetLastError(0xdeadbeef);
    ret = dispatch.pfCreate(&info, &size, NULL);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    info.DigestAlgorithm.pszObjId = (char *)"unknown";
    SetLastError(0xdeadbeef);
    ret = dispatch.pfCreate(&info, &size, NULL);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    info.DigestAlgorithm.pszObjId = (char *)"2.16.840.1.101.3.4.2.1";

    size = 0;
    SetLastError(0xdeadbeef);
    ret = dispatch.pfCreate(&info, &size, NULL);
    ok(ret, "pfCreate failed %lu\n", GetLastError());
    ok(size > sizeof(*indirect), "got %lu\n", size);
    indirect = HeapAlloc(GetProcessHeap(), 0, size);
    ret = dispatch.pfCreate(&info, &size, indirect);
    ok(ret, "pfCreate failed %lu\n", GetLastError());
    ok(!strcmp(indirect->Data.pszObjId, "1.3.6.1.4.1.311.2.1.30"), "got %s\n", indirect->Data.pszObjId);
    ok(!strcmp(indirect->DigestAlgorithm.pszObjId, "2.16.840.1.101.3.4.2.1"), "got %s\n",
       indirect->DigestAlgorithm.pszObjId);
    ok(indirect->Digest.cbData == sizeof(script_sha256) &&
       !memcmp(indirect->Digest.pbData, script_sha256, sizeof(script_sha256)), "wrong digest\n");
    ok(indirect->Data.Value.cbData == 38 + 2 && !memcmp(indirect->Data.Value.pbData + 9, &pwsh_subject, 16),
       "wrong SIP info\n");

    SetLastError(0xdeadbeef);
    ret = dispatch.pfVerify(&info, indirect);
    ok(ret, "pfVerify failed %lu\n", GetLastError());

    /* sign: the message becomes a comment block, the script itself does not change */
    index = 0xdeadbeef;
    ret = dispatch.pfPut(&info, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, &index, sizeof(message), (BYTE *)message);
    ok(ret, "pfPut failed %lu\n", GetLastError());
    ok(!index, "got %lu\n", index);

    handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ReadFile(handle, file, sizeof(file) - 1, &read, NULL);
    CloseHandle(handle);
    file[read] = 0;
    ok(!strncmp(file, script_text, sizeof(script_text) - 1), "got %s\n", wine_dbgstr_an(file, read));
    ok(strstr(file, "\r\n# SIG # Begin signature block\r\n# MAMCAQEwggEA3q2+7w==\r\n# SIG # End signature block\r\n") != NULL,
       "got %s\n", wine_dbgstr_an(file, read));

    /* the digest is still that of the script */
    ret = dispatch.pfVerify(&info, indirect);
    ok(ret, "pfVerify failed %lu\n", GetLastError());

    size = 0;
    encoding = 0;
    ret = dispatch.pfGet(&info, &encoding, 0, &size, NULL);
    ok(ret, "pfGet failed %lu\n", GetLastError());
    ok(size == sizeof(message), "got %lu\n", size);
    ok(encoding == (X509_ASN_ENCODING | PKCS_7_ASN_ENCODING), "got %#lx\n", encoding);
    buf = HeapAlloc(GetProcessHeap(), 0, size);
    ret = dispatch.pfGet(&info, &encoding, 0, &size, buf);
    ok(ret, "pfGet failed %lu\n", GetLastError());
    ok(size == sizeof(message) && !memcmp(buf, message, sizeof(message)), "wrong message\n");
    HeapFree(GetProcessHeap(), 0, buf);

    size = 1;
    SetLastError(0xdeadbeef);
    ret = dispatch.pfGet(&info, &encoding, 0, &size, buffer);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "got error %lu\n", GetLastError());
    ok(size == sizeof(message), "got %lu\n", size);

    SetLastError(0xdeadbeef);
    ret = dispatch.pfGet(&info, &encoding, 1, &size, buffer);
    ok(!ret, "expected failure\n");

    /* changed text */
    handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    WriteFile(handle, "Write-Host \"ho\"", 15, &read, NULL);
    CloseHandle(handle);
    SetLastError(0xdeadbeef);
    ret = dispatch.pfVerify(&info, indirect);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == TRUST_E_BAD_DIGEST, "got error %#lx\n", GetLastError());
    handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    WriteFile(handle, script_text, 15, &read, NULL);
    CloseHandle(handle);

    /* unsign */
    ret = dispatch.pfRemove(&info, 0);
    ok(ret, "pfRemove failed %lu\n", GetLastError());
    handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ReadFile(handle, file, sizeof(file) - 1, &read, NULL);
    CloseHandle(handle);
    ok(read == sizeof(script_text) - 1 && !memcmp(file, script_text, read), "got %s\n", wine_dbgstr_an(file, read));

    /* an unsigned file has no message, and that is not an error */
    size = 0x1234;
    encoding = 0x5678;
    ret = dispatch.pfGet(&info, &encoding, 0, &size, NULL);
    ok(ret, "pfGet failed %lu\n", GetLastError());
    ok(!size && !encoding, "got size %lu, encoding %#lx\n", size, encoding);
    size = 0x1234;
    ret = dispatch.pfGet(&info, &encoding, 0, &size, buffer);
    ok(ret, "pfGet failed %lu\n", GetLastError());
    ok(!size, "got size %lu\n", size);

    HeapFree(GetProcessHeap(), 0, indirect);
    CryptReleaseContext(prov, 0);
}


static const char *find_str(const char *s, DWORD len, const char *needle)
{
    DWORD n = strlen(needle), i;

    for (i = 0; i + n <= len; i++) if (!memcmp(s + i, needle, n)) return s + i;
    return NULL;
}

static LONG verify_blob_with_msg(const GUID *subject, const BYTE *content, DWORD size, const BYTE *msg, DWORD msg_size)
{
    WINTRUST_BLOB_INFO blob = {sizeof(blob)};
    WINTRUST_DATA data = {sizeof(data)};
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG ret;

    blob.gSubject = *subject;
    blob.pcwszDisplayName = L"script.ps1";
    blob.cbMemObject = size;
    blob.pbMemObject = (BYTE *)content;
    blob.cbMemSignedMsg = msg_size;
    blob.pbMemSignedMsg = (BYTE *)msg;
    data.dwUIChoice = WTD_UI_NONE;
    data.dwUnionChoice = WTD_CHOICE_BLOB;
    data.pBlob = &blob;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    ret = WinVerifyTrust(NULL, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &action, &data);
    return ret;
}

static void test_malformed(const WCHAR *path)
{
    static const char trailing[] = "Write-Host evil\r\n";
    static const struct
    {
        const char *name;
        LONG expected;
    } names[] = {
        {"trailing text", TRUST_E_NOSIGNATURE}, {"trailing comment", TRUST_E_NOSIGNATURE},
        {"two blocks", TRUST_E_NOSIGNATURE}, {"text between blocks", TRUST_E_NOSIGNATURE},
        {"junk in block", TRUST_E_NOSIGNATURE}, {"begin marker in the text", TRUST_E_BAD_DIGEST},
        {"line feeds", TRUST_E_NOSIGNATURE}, {"no end", TRUST_E_NOSIGNATURE}, {"bad base64", TRUST_E_NOSIGNATURE},
        {"only the block", TRUST_E_NOSIGNATURE},
    };
    const DWORD size = sizeof(signed_script) - 1;
    const char *begin = find_str(signed_script, size, "# SIG # Begin signature block");
    const char *end = find_str(signed_script, size, "# SIG # End signature block");
    char *buf = HeapAlloc(GetProcessHeap(), 0, 4 * size + 256), *p;
    DWORD len, block = size - (begin - signed_script), i;
    LONG ret, ret_blob;

    for (i = 0; i < ARRAYSIZE(names); i++)
    {
        p = buf;
        switch (i)
        {
        case 0: memcpy(p, signed_script, size); p += size; memcpy(p, trailing, sizeof(trailing) - 1); p += sizeof(trailing) - 1; break;
        case 1: memcpy(p, signed_script, size); p += size; memcpy(p, "# harmless\r\n", 12); p += 12; break;
        case 2: memcpy(p, signed_script, size); p += size; memcpy(p, begin, block); p += block; break;
        case 3: memcpy(p, signed_script, size); p += size; memcpy(p, trailing, sizeof(trailing) - 1); p += sizeof(trailing) - 1;
                memcpy(p, begin, block); p += block; break;
        case 4: memcpy(p, signed_script, end - signed_script); p += end - signed_script;
                memcpy(p, "Write-Host evil\r\n", 17); p += 17; memcpy(p, end, signed_script + size - end); p += signed_script + size - end; break;
        case 5: memcpy(p, "# SIG # Begin signature block\r\nWrite-Host evil\r\n", 47); p += 47; memcpy(p, signed_script, size); p += size; break;
        case 6: { DWORD k; for (k = 0; k < size; k++) if (!(signed_script[k] == '\r' && signed_script[k + 1] == '\n')) *p++ = signed_script[k]; } break;
        case 7: memcpy(p, signed_script, end - signed_script); p += end - signed_script; break;
        case 8: memcpy(p, signed_script, size); memcpy((char *)find_str(p, size, "# MIIFpwYJ") + 2, "!!!!!!!!", 8); p += size; break;
        case 9: memcpy(p, begin, block); p += block; break;
        }
        len = p - buf;
        write_file(path, buf, len);
        ret = verify_file(path);
        ok(ret == names[i].expected, "%s: file got %#lx\n", names[i].name, ret);
        ret_blob = verify_blob(buf, len);
        ok(ret_blob == names[i].expected, "%s: blob got %#lx\n", names[i].name, ret_blob);
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

static void test_blob_builtin(const WCHAR *path)
{
    static const GUID subjects[] = {
        {0xc689aab8, 0x8e78, 0x11d0, {0x8c, 0x47, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}},
        {0xc689aaba, 0x8e78, 0x11d0, {0x8c, 0x47, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}},
        {0xde351a42, 0x8e59, 0x11d0, {0x8c, 0x47, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}},
        {0x000c10f1, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}},
        {0xde351a43, 0x8e59, 0x11d0, {0x8c, 0x47, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}},
    };
    SIP_DISPATCH_INFO dispatch = {sizeof(dispatch)};
    SIP_SUBJECTINFO info = {sizeof(info)};
    GUID subject = pwsh_subject;
    DWORD size = 0, encoding, i;
    BYTE *msg;
    LONG ret;
    BOOL ok_;

    write_file(path, signed_script, sizeof(signed_script) - 1);
    ok_ = CryptSIPLoad(&subject, 0, &dispatch);
    ok(ok_, "CryptSIPLoad failed %lu\n", GetLastError());
    if (!ok_) return;
    info.pgSubjectType = &subject;
    info.pwsFileName = path;
    info.hFile = INVALID_HANDLE_VALUE;
    ok_ = dispatch.pfGet(&info, &encoding, 0, &size, NULL);
    ok(ok_ && size, "pfGet failed %lu\n", GetLastError());
    msg = HeapAlloc(GetProcessHeap(), 0, size);
    ok_ = dispatch.pfGet(&info, &encoding, 0, &size, msg);
    ok(ok_, "pfGet failed %lu\n", GetLastError());

    /* a valid signature for other content never vouches for a memory object of a SIP that hashes files */
    for (i = 0; i < ARRAYSIZE(subjects); i++)
    {
        LONG expected = i == 4 ? (LONG)0x8009200d : TRUST_E_NOSIGNATURE;

        ret = verify_blob_with_msg(&subjects[i], (const BYTE *)"not the signed bytes", 20, msg, size);
        ok(ret == expected, "subject %lu with message: got %#lx\n", i, ret);
        ret = verify_blob_with_msg(&subjects[i], (const BYTE *)"not the signed bytes", 20, NULL, 0);
        ok(ret == expected, "subject %lu: got %#lx\n", i, ret);
    }

    /* a message supplied with a script is ignored too: the signature is the one in the text */
    ret = verify_blob_with_msg(&pwsh_subject, (const BYTE *)script_text, sizeof(script_text) - 1, msg, size);
    ok(ret == TRUST_E_NOSIGNATURE, "got %#lx\n", ret);
    ret = verify_blob_with_msg(&pwsh_subject, (const BYTE *)signed_script, sizeof(signed_script) - 1, msg, size);
    ok(ret == CERT_E_UNTRUSTEDROOT, "got %#lx\n", ret);
    HeapFree(GetProcessHeap(), 0, msg);
}

static void test_names(void)
{
    static const WCHAR *supported[] = {L"a.ps1", L"a.PSM1", L"a.psd1", L"a.ps1xml", L"a.psc1", L"a.cdxml", L"a.mof"};
    static const WCHAR *other[] = {L"a.txt", L"a.exe", L"a.ps2", L"a.ps1x"};
    GUID subject;
    unsigned int i;
    WCHAR path[MAX_PATH];
    BOOL ret;

    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"pwrshsip-name-test.");
    for (i = 0; i < ARRAYSIZE(supported); i++)
    {
        WCHAR file[MAX_PATH];

        lstrcpyW(file, path);
        lstrcatW(file, wcsrchr(supported[i], '.') + 1);
        write_file(file, script_text, sizeof(script_text) - 1);
        ret = CryptSIPRetrieveSubjectGuid(file, NULL, &subject);
        ok(ret && IsEqualGUID(&subject, &pwsh_subject), "%s: got %d, %s\n", wine_dbgstr_w(supported[i]), ret,
           wine_dbgstr_guid(&subject));
        DeleteFileW(file);
    }
    for (i = 0; i < ARRAYSIZE(other); i++)
    {
        WCHAR file[MAX_PATH];

        lstrcpyW(file, path);
        lstrcatW(file, wcsrchr(other[i], '.') + 1);
        write_file(file, script_text, sizeof(script_text) - 1);
        ret = CryptSIPRetrieveSubjectGuid(file, NULL, &subject);
        ok(!ret || !IsEqualGUID(&subject, &pwsh_subject), "%s: unexpected success\n", wine_dbgstr_w(other[i]));
        DeleteFileW(file);
    }
}

START_TEST(pwrshsip)
{
    WCHAR path[MAX_PATH];
    GUID subject = pwsh_subject;
    SIP_DISPATCH_INFO dispatch = {sizeof(dispatch)};

    if (!CryptSIPLoad(&subject, 0, &dispatch))
    {
        win_skip("the PowerShell SIP is not registered\n");
        return;
    }

    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"pwrshsip-test.ps1");

    test_verify(path);
    test_sip(path);
    test_malformed(path);
    test_blob_builtin(path);
    test_names();
    DeleteFileW(path);
}
