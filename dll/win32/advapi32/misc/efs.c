/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dlls/win32/advapi32/misc/efs.c
 * PURPOSE:         Encrypted File System support
 * PROGRAMMER:      Christoph_vW
 */

#include <advapi32.h>

#include <winefs.h>

WINE_DEFAULT_DEBUG_CHANNEL(advapi);

/*
 * @unimplemented
 */
DWORD WINAPI
AddUsersToEncryptedFile(LPCWSTR lpcwstr,
                        PENCRYPTION_CERTIFICATE_LIST pencryption_certificate_list)
{
    FIXME("%s() not implemented!\n", __FUNCTION__);
    return ERROR_CALL_NOT_IMPLEMENTED;
}


/*
 * @implemented
 */
BOOL WINAPI
DecryptFileA(LPCSTR lpFileName,
             DWORD dwReserved)
{
    UNICODE_STRING FileName;
    BOOL ret;

    if (!RtlCreateUnicodeStringFromAsciiz(&FileName, lpFileName))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    ret = DecryptFileW(FileName.Buffer, dwReserved);

    if (FileName.Buffer != NULL)
        RtlFreeUnicodeString(&FileName);
    return ret;
}


/*
 * There is no Encrypted File System. As on a file system without encryption
 * support on Windows, nothing gets encrypted, and a call that would have to
 * encrypt or decrypt fails instead of reporting success. The error codes are
 * not all the ones Windows gives (see EncryptFileW and FileEncryptionStatusW).
 */
static BOOL
EfsGetAttributes(LPCWSTR lpFileName,
                 LPDWORD lpAttributes)
{
    *lpAttributes = GetFileAttributesW(lpFileName);
    return (*lpAttributes != INVALID_FILE_ATTRIBUTES);
}


/*
 * @unimplemented
 */
BOOL WINAPI
DecryptFileW(LPCWSTR lpFileName,
             DWORD dwReserved)
{
    DWORD Attributes;

    FIXME("%s(%S) not implemented!\n", __FUNCTION__, lpFileName);

    if (!EfsGetAttributes(lpFileName, &Attributes))
        return FALSE;

    /* A file encrypted elsewhere cannot be decrypted here */
    if (Attributes & FILE_ATTRIBUTE_ENCRYPTED)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    /* Not encrypted: nothing to do */
    return TRUE;
}


/*
 * @implemented
 */
BOOL WINAPI
EncryptFileA(LPCSTR lpFileName)
{
    UNICODE_STRING FileName;
    BOOL ret;

    if (!RtlCreateUnicodeStringFromAsciiz(&FileName, lpFileName))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    ret = EncryptFileW(FileName.Buffer);

    if (FileName.Buffer != NULL)
        RtlFreeUnicodeString(&FileName);
    return ret;
}


/*
 * @unimplemented
 */
BOOL WINAPI
EncryptFileW(LPCWSTR lpFileName)
{
    DWORD Attributes;

    FIXME("%s(%S) not implemented!\n", __FUNCTION__, lpFileName);

    if (!EfsGetAttributes(lpFileName, &Attributes))
        return FALSE;

    /* Windows checks this first, on file systems with and without encryption */
    if (Attributes & FILE_ATTRIBUTE_READONLY)
    {
        SetLastError(ERROR_FILE_READ_ONLY);
        return FALSE;
    }

    /* Windows without encryption support gives ERROR_ACCESS_DENIED or
     * ERROR_INVALID_PARAMETER, which point at the caller; this one does not */
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}


/*
 * @unimplemented
 */
BOOL WINAPI
EncryptionDisable(LPCWSTR DirPath,
                  BOOL Disable)
{
    FIXME("%s() not implemented!\n", __FUNCTION__);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}


/*
 * @implemented
 */
BOOL WINAPI
FileEncryptionStatusA(LPCSTR lpFileName,
                      LPDWORD lpStatus)
{
    UNICODE_STRING FileName;
    BOOL ret = FALSE;

    TRACE("(%s, %p)\n", lpFileName, lpStatus);

    FileName.Buffer = NULL;

    if (!RtlCreateUnicodeStringFromAsciiz(&FileName, lpFileName))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        goto cleanup;
    }

    ret = FileEncryptionStatusW(FileName.Buffer, lpStatus);

cleanup:
    if (FileName.Buffer != NULL)
        RtlFreeUnicodeString(&FileName);

    return ret;
}

/*
 * @unimplemented
 */
BOOL WINAPI
FileEncryptionStatusW(LPCWSTR lpFileName,
                      LPDWORD lpStatus)
{
    DWORD Attributes;

    FIXME("%s(%S) not implemented!\n", __FUNCTION__, lpFileName);

    if (!lpStatus)
        return FALSE;

    if (!EfsGetAttributes(lpFileName, &Attributes))
        return FALSE;

    /* Windows reports FILE_ENCRYPTABLE even where EncryptFile then fails */
    *lpStatus = (Attributes & FILE_ATTRIBUTE_ENCRYPTED) ? FILE_IS_ENCRYPTED : FILE_SYSTEM_NOT_SUPPORT;

    return TRUE;
}


/*
 * @unimplemented
 */
VOID WINAPI
FreeEncryptionCertificateHashList(PENCRYPTION_CERTIFICATE_HASH_LIST pencryption_certificate_hash_list)
{
    FIXME("%s() not implemented!\n", __FUNCTION__);
    return;
}


/*
 * @unimplemented
 */
DWORD WINAPI
QueryRecoveryAgentsOnEncryptedFile(LPCWSTR lpctstr,
                                   PENCRYPTION_CERTIFICATE_HASH_LIST* pencryption_certificate_hash_list)
{
    FIXME("%s() not implemented!\n", __FUNCTION__);
    return ERROR_CALL_NOT_IMPLEMENTED;
}


/*
 * @unimplemented
 */
DWORD WINAPI
QueryUsersOnEncryptedFile(LPCWSTR lpctstr,
                          PENCRYPTION_CERTIFICATE_HASH_LIST* pencryption_certificate_hash_list)
{
    FIXME("%s() not implemented!\n", __FUNCTION__);
    return ERROR_CALL_NOT_IMPLEMENTED;
}


/*
 * @unimplemented
 */
DWORD WINAPI
RemoveUsersFromEncryptedFile(LPCWSTR lpcwstr,
                             PENCRYPTION_CERTIFICATE_HASH_LIST pencryption_certificate_hash_list)
{
    FIXME("%s() not implemented!\n", __FUNCTION__);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/* EOF */
