/*
 * Copyright 2007 Juan Lang
 * Copyright 2016 Mark Jansen
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

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wintrust.h"
#include "mssip.h"
#include "softpub.h"
#include "winnls.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wintrust);

HRESULT WINAPI SoftpubDefCertInit(CRYPT_PROVIDER_DATA *data)
{
    HRESULT ret = S_FALSE;

    TRACE("(%p)\n", data);

    if (data->padwTrustStepErrors &&
     !data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_WVTINIT])
        ret = S_OK;
    TRACE("returning %08lx\n", ret);
    return ret;
}

HRESULT WINAPI SoftpubInitialize(CRYPT_PROVIDER_DATA *data)
{
    HRESULT ret = S_FALSE;

    TRACE("(%p)\n", data);

    if (data->padwTrustStepErrors &&
     !data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_WVTINIT])
        ret = S_OK;
    TRACE("returning %08lx\n", ret);
    return ret;
}

HRESULT WINAPI DriverInitializePolicy(CRYPT_PROVIDER_DATA *data)
{
    FIXME("stub\n");
    return S_OK;
}

HRESULT WINAPI DriverCleanupPolicy(CRYPT_PROVIDER_DATA *data)
{
    FIXME("stub\n");
    return S_OK;
}

HRESULT WINAPI DriverFinalPolicy(CRYPT_PROVIDER_DATA *data)
{
    FIXME("stub\n");
    return S_OK;
}

/* Assumes data->pWintrustData->u.pFile exists.  Makes sure a file handle is
 * open for the file.
 */
static DWORD SOFTPUB_OpenFile(CRYPT_PROVIDER_DATA *data)
{
    DWORD err = ERROR_SUCCESS;

    /* PSDK implies that all values should be initialized to NULL, so callers
     * typically have hFile as NULL rather than INVALID_HANDLE_VALUE.  Check
     * for both.
     */
    if (!data->pWintrustData->pFile->hFile ||
     data->pWintrustData->pFile->hFile == INVALID_HANDLE_VALUE)
    {
        data->pWintrustData->pFile->hFile =
            CreateFileW(data->pWintrustData->pFile->pcwszFilePath, GENERIC_READ,
          FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (data->pWintrustData->pFile->hFile != INVALID_HANDLE_VALUE)
            data->fOpenedFile = TRUE;
        else
            err = GetLastError();
    }
    if (!err)
        GetFileTime(data->pWintrustData->pFile->hFile, &data->sftSystemTime,
         NULL, NULL);
    TRACE("returning %ld\n", err);
    return err;
}

/* Assumes data->pWintrustData->pFile exists.  Sets data->pPDSip->gSubject to
 * the file's subject GUID.
 */
static DWORD SOFTPUB_GetFileSubject(CRYPT_PROVIDER_DATA *data)
{
    DWORD err = ERROR_SUCCESS;

    if (!WVT_ISINSTRUCT(WINTRUST_FILE_INFO,
     data->pWintrustData->pFile->cbStruct, pgKnownSubject) ||
     !data->pWintrustData->pFile->pgKnownSubject)
    {
        if (!CryptSIPRetrieveSubjectGuid(
         data->pWintrustData->pFile->pcwszFilePath,
         data->pWintrustData->pFile->hFile,
         &data->pPDSip->gSubject))
        {
            LARGE_INTEGER fileSize;
            DWORD sipError = GetLastError();

            /* Special case for empty files: the error is expected to be
             * TRUST_E_SUBJECT_FORM_UNKNOWN, rather than whatever
             * CryptSIPRetrieveSubjectGuid returns.
             */
            if (GetFileSizeEx(data->pWintrustData->pFile->hFile, &fileSize)
             && !fileSize.QuadPart)
                err = TRUST_E_SUBJECT_FORM_UNKNOWN;
            else
                err = sipError;
        }
    }
    else
        data->pPDSip->gSubject = *data->pWintrustData->pFile->pgKnownSubject;
    TRACE("returning %ld\n", err);
    return err;
}

/* Assumes data->pPDSip exists, and its gSubject member set.
 * Allocates data->pPDSip->pSip and loads it, if possible.
 */
static DWORD SOFTPUB_GetSIP(CRYPT_PROVIDER_DATA *data)
{
    DWORD err = ERROR_SUCCESS;

    data->pPDSip->pSip = data->psPfns->pfnAlloc(sizeof(SIP_DISPATCH_INFO));
    if (data->pPDSip->pSip)
    {
        if (!CryptSIPLoad(&data->pPDSip->gSubject, 0, data->pPDSip->pSip))
            err = GetLastError();
    }
    else
        err = ERROR_OUTOFMEMORY;
    TRACE("returning %ld\n", err);
    return err;
}

/* Assumes data->pPDSip has been loaded, and data->pPDSip->pSip allocated.
 * Calls data->pPDSip->pSip->pfGet to construct data->hMsg.
 */
static DWORD SOFTPUB_GetMessageFromFile(CRYPT_PROVIDER_DATA *data, HANDLE file,
 LPCWSTR filePath)
{
    DWORD err = ERROR_SUCCESS;
    BOOL ret;
    LPBYTE buf = NULL;
    DWORD size = 0;

    data->pPDSip->psSipSubjectInfo =
     data->psPfns->pfnAlloc(sizeof(SIP_SUBJECTINFO));
    if (!data->pPDSip->psSipSubjectInfo)
        return ERROR_OUTOFMEMORY;

    data->pPDSip->psSipSubjectInfo->cbSize = sizeof(SIP_SUBJECTINFO);
    data->pPDSip->psSipSubjectInfo->pgSubjectType = &data->pPDSip->gSubject;
    data->pPDSip->psSipSubjectInfo->hFile = file;
    data->pPDSip->psSipSubjectInfo->pwsFileName = filePath;
    data->pPDSip->psSipSubjectInfo->hProv = data->hProv;
    ret = data->pPDSip->pSip->pfGet(data->pPDSip->psSipSubjectInfo,
     &data->dwEncoding, 0, &size, 0);
    if (!ret)
        return TRUST_E_NOSIGNATURE;

    buf = data->psPfns->pfnAlloc(size);
    if (!buf)
        return ERROR_OUTOFMEMORY;

    ret = data->pPDSip->pSip->pfGet(data->pPDSip->psSipSubjectInfo,
     &data->dwEncoding, 0, &size, buf);
    if (ret)
    {
        data->hMsg = CryptMsgOpenToDecode(data->dwEncoding, 0, 0, data->hProv,
         NULL, NULL);
        if (data->hMsg)
        {
            ret = CryptMsgUpdate(data->hMsg, buf, size, TRUE);
            if (!ret)
                err = GetLastError();
        }
    }
    else
        err = GetLastError();

    data->psPfns->pfnFree(buf);
    TRACE("returning %ld\n", err);
    return err;
}

static BOOL hash_file_data( HANDLE file, DWORD start, DWORD end, HCRYPTHASH hash )
{
    DWORD bytes_read, size = end - start;
    DWORD buffer_size = min( size, 1024*1024 );
    BYTE *buffer = malloc( buffer_size );

    if (!buffer) return FALSE;
    SetFilePointer( file, start, NULL, FILE_BEGIN );
    while (size)
    {
        if (!ReadFile( file, buffer, min( buffer_size, size ), &bytes_read, NULL )) break;
        if (!bytes_read) break;
        if (!CryptHashData( hash, buffer, bytes_read, 0 )) break;
        size -= bytes_read;
    }
    free( buffer );
    return !size;
}

/* See https://www.cs.auckland.ac.nz/~pgut001/pubs/authenticode.txt
 * for details about the hashing.
 */
static BOOL SOFTPUB_HashPEFile(HANDLE file, HCRYPTHASH hash)
{
    DWORD checksum, security_dir;
    IMAGE_DOS_HEADER dos_header;
    union
    {
        IMAGE_NT_HEADERS32 nt32;
        IMAGE_NT_HEADERS64 nt64;
    } nt_header;
    IMAGE_DATA_DIRECTORY secdir;
    LARGE_INTEGER file_size;
    DWORD bytes_read;
    BOOL ret;

    if (!GetFileSizeEx(file, &file_size))
        return FALSE;

    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    ret = ReadFile(file, &dos_header, sizeof(dos_header), &bytes_read, NULL);
    if (!ret || bytes_read != sizeof(dos_header))
        return FALSE;

    if (dos_header.e_magic != IMAGE_DOS_SIGNATURE)
        return FALSE;
    if (dos_header.e_lfanew >= 256 * 1024 * 1024) /* see RtlImageNtHeaderEx */
        return FALSE;
    if (dos_header.e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader.MajorLinkerVersion) > file_size.QuadPart)
        return FALSE;

    SetFilePointer(file, dos_header.e_lfanew, NULL, FILE_BEGIN);
    ret = ReadFile(file, &nt_header, sizeof(nt_header), &bytes_read, NULL);
    if (!ret || bytes_read < FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader.Magic) +
                             sizeof(nt_header.nt32.OptionalHeader.Magic))
        return FALSE;

    if (nt_header.nt32.Signature != IMAGE_NT_SIGNATURE)
        return FALSE;

    if (nt_header.nt32.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        if (bytes_read < sizeof(nt_header.nt32))
            return FALSE;

        checksum     = dos_header.e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader.CheckSum);
        security_dir = dos_header.e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY]);
        secdir       = nt_header.nt32.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    }
    else if (nt_header.nt32.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        if (bytes_read < sizeof(nt_header.nt64))
            return FALSE;

        checksum     = dos_header.e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader.CheckSum);
        security_dir = dos_header.e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY]);
        secdir       = nt_header.nt64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    }
    else
        return FALSE;

    if (secdir.VirtualAddress < security_dir + sizeof(IMAGE_DATA_DIRECTORY))
        return FALSE;
    if (secdir.VirtualAddress > file_size.QuadPart)
        return FALSE;
    if (secdir.VirtualAddress + secdir.Size != file_size.QuadPart)
        return FALSE;

    if (!hash_file_data( file, 0, checksum, hash )) return FALSE;
    if (!hash_file_data( file, checksum + sizeof(DWORD), security_dir, hash )) return FALSE;
    if (!hash_file_data( file, security_dir + sizeof(IMAGE_DATA_DIRECTORY), secdir.VirtualAddress, hash ))
        return FALSE;

    return TRUE;
}

static DWORD SOFTPUB_VerifyImageHash(CRYPT_PROVIDER_DATA *data, HANDLE file)
{
    SPC_INDIRECT_DATA_CONTENT *indirect = (SPC_INDIRECT_DATA_CONTENT *)data->pPDSip->psIndirectData;
    DWORD err, hash_size, length;
    BYTE *hash_data;
    BOOL release_prov = FALSE;
    HCRYPTPROV prov = data->hProv;
    HCRYPTHASH hash = 0;
    ALG_ID algID;

    if (((ULONG_PTR)indirect->Data.pszObjId >> 16) == 0 ||
        strcmp(indirect->Data.pszObjId, SPC_PE_IMAGE_DATA_OBJID))
    {
        FIXME("Cannot verify hash for pszObjId=%s\n", debugstr_a(indirect->Data.pszObjId));
        return ERROR_SUCCESS;
    }

    if (!(algID = CertOIDToAlgId(indirect->DigestAlgorithm.pszObjId)))
        return TRUST_E_SYSTEM_ERROR; /* FIXME */

    if (!prov)
    {
        if (!CryptAcquireContextW(&prov, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
            return GetLastError();
        release_prov = TRUE;
    }

    if (!CryptCreateHash(prov, algID, 0, 0, &hash))
    {
        err = GetLastError();
        goto done;
    }

    if (!SOFTPUB_HashPEFile(file, hash))
    {
        err = TRUST_E_NOSIGNATURE;
        goto done;
    }

    length = sizeof(hash_size);
    if (!CryptGetHashParam(hash, HP_HASHSIZE, (BYTE *)&hash_size, &length, 0))
    {
        err = GetLastError();
        goto done;
    }

    if (!(hash_data = data->psPfns->pfnAlloc(hash_size)))
    {
        err = ERROR_OUTOFMEMORY;
        goto done;
    }

    if (!CryptGetHashParam(hash, HP_HASHVAL, hash_data, &hash_size, 0))
    {
        err = GetLastError();
        data->psPfns->pfnFree(hash_data);
        goto done;
    }

    err = (hash_size == indirect->Digest.cbData &&
           !memcmp(hash_data, indirect->Digest.pbData, hash_size)) ? S_OK : TRUST_E_BAD_DIGEST;
    data->psPfns->pfnFree(hash_data);

done:
    if (hash)
        CryptDestroyHash(hash);
    if (release_prov)
        CryptReleaseContext(prov, 0);
    return err;
}


/* Digest of the PE image (everything except the checksum, the certificate table entry and the
 * certificate table), as the embedded signature records it. Returns a malloc()ed buffer. */
static BYTE *SOFTPUB_ComputePEHash(HANDLE file, ALG_ID alg_id, DWORD *size)
{
    HCRYPTPROV prov;
    HCRYPTHASH hash = 0;
    BYTE *value = NULL;
    DWORD length = sizeof(*size);

    if (!CryptAcquireContextW(&prov, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return NULL;
    if (CryptCreateHash(prov, alg_id, 0, 0, &hash))
    {
        if (SOFTPUB_HashPEFile(file, hash) &&
            CryptGetHashParam(hash, HP_HASHSIZE, (BYTE *)size, &length, 0) &&
            (value = malloc(*size)) &&
            !CryptGetHashParam(hash, HP_HASHVAL, value, size, 0))
        {
            free(value);
            value = NULL;
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return value;
}

/* Digest recorded in the embedded signature of a file whose image still matches it. */
static BYTE *SOFTPUB_GetEmbeddedDigest(LPCWSTR path, DWORD *size, ALG_ID *alg_id)
{
    SIP_DISPATCH_INFO sip;
    SIP_SUBJECTINFO info;
    SPC_INDIRECT_DATA_CONTENT *indirect = NULL;
    HCRYPTMSG msg = NULL;
    HANDLE file;
    GUID subject;
    DWORD encoding = 0, msg_size = 0, oid_size, content_size, indirect_size;
    BYTE *msg_buf = NULL, *content = NULL, *digest = NULL, *actual;
    char *oid = NULL;
    ALG_ID alg;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return NULL;

    memset(&sip, 0, sizeof(sip));
    sip.cbSize = sizeof(sip);
    if (!CryptSIPRetrieveSubjectGuid(path, file, &subject) || !CryptSIPLoad(&subject, 0, &sip))
        goto done;

    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    info.pgSubjectType = &subject;
    info.hFile = file;
    info.pwsFileName = path;
    if (!sip.pfGet(&info, &encoding, 0, &msg_size, NULL) || !(msg_buf = malloc(msg_size)) ||
        !sip.pfGet(&info, &encoding, 0, &msg_size, msg_buf))
        goto done;

    if (!(msg = CryptMsgOpenToDecode(encoding, 0, 0, 0, NULL, NULL)) ||
        !CryptMsgUpdate(msg, msg_buf, msg_size, TRUE))
        goto done;
    if (!CryptMsgGetParam(msg, CMSG_INNER_CONTENT_TYPE_PARAM, 0, NULL, &oid_size) ||
        !(oid = malloc(oid_size)) ||
        !CryptMsgGetParam(msg, CMSG_INNER_CONTENT_TYPE_PARAM, 0, oid, &oid_size))
        goto done;
    if (!CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, NULL, &content_size) ||
        !(content = malloc(content_size)) ||
        !CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, content, &content_size))
        goto done;
    if (!CryptDecodeObject(encoding, oid, content, content_size, 0, NULL, &indirect_size) ||
        !(indirect = malloc(indirect_size)) ||
        !CryptDecodeObject(encoding, oid, content, content_size, 0, indirect, &indirect_size))
        goto done;

    /* The digest must be of this very image; only PE images can be hashed here */
    if (((ULONG_PTR)indirect->Data.pszObjId >> 16) == 0 ||
        strcmp(indirect->Data.pszObjId, SPC_PE_IMAGE_DATA_OBJID))
        goto done;
    if (!(alg = CertOIDToAlgId(indirect->DigestAlgorithm.pszObjId)))
        goto done;
    if (!(actual = SOFTPUB_ComputePEHash(file, alg, size)))
        goto done;
    if (*size == indirect->Digest.cbData && !memcmp(actual, indirect->Digest.pbData, *size))
    {
        digest = actual;
        *alg_id = alg;
    }
    else
        free(actual);

done:
    free(indirect);
    free(content);
    free(oid);
    free(msg_buf);
    if (msg)
        CryptMsgClose(msg);
    CloseHandle(file);
    return digest;
}

/***********************************************************************
 *      WTHelperGetFileHash  (WINTRUST.@)
 *
 * Undocumented. PowerShell's getfilesiginforedist.dll imports it; with the export missing the
 * loader raised its modal "Entry Point Not Found" hard error, which nobody can answer in a
 * console session, so the 32-bit pwsh blocked the first time the Security module was used
 * (Get-Acl, Get-ExecutionPolicy, Import-Module Microsoft.PowerShell.Security).
 *
 * Behaviour of Windows 10 22H2, probed with a 32-bit program: six stdcall arguments; for a file
 * with an embedded signature whose digest matches the image it returns 0 and the signature's
 * digest (SHA-256: 32 bytes, *pAlgId -1); a NULL pbHash only reports the size; a short buffer
 * gives ERROR_MORE_DATA and the needed size; everything else (no embedded signature, catalog
 * signed only, modified image, missing file, NULL name) returns TRUST_E_NOSIGNATURE.
 * Which ALG_ID is reported for MD5/SHA-1 digests was not probed.
 */
HRESULT WINAPI WTHelperGetFileHash(LPCWSTR path, DWORD flags, PVOID reserved,
                                   BYTE *hash, DWORD *size, ALG_ID *alg_id)
{
    ALG_ID alg = 0;
    DWORD digest_size = 0;
    BYTE *digest;
    HRESULT hr;

    TRACE("(%s %#lx %p %p %p %p)\n", debugstr_w(path), flags, reserved, hash, size, alg_id);

    if (!path || !size || !(digest = SOFTPUB_GetEmbeddedDigest(path, &digest_size, &alg)))
    {
        if (size)
            *size = 0;
        return TRUST_E_NOSIGNATURE;
    }

    if (alg_id)
        *alg_id = (alg == CALG_MD5 || alg == CALG_SHA1) ? alg : (ALG_ID)-1;
    if (hash && *size < digest_size)
        hr = ERROR_MORE_DATA;
    else
    {
        if (hash)
            memcpy(hash, digest, digest_size);
        hr = S_OK;
    }
    *size = digest_size;
    free(digest);
    return hr;
}

static DWORD SOFTPUB_CreateStoreFromMessage(CRYPT_PROVIDER_DATA *data)
{
    DWORD err = ERROR_SUCCESS;
    HCERTSTORE store;

    store = CertOpenStore(CERT_STORE_PROV_MSG, data->dwEncoding,
     data->hProv, CERT_STORE_NO_CRYPT_RELEASE_FLAG, data->hMsg);
    if (store)
    {
        if (!data->psPfns->pfnAddStore2Chain(data, store))
            err = GetLastError();
        CertCloseStore(store, 0);
    }
    else
        err = GetLastError();
    TRACE("returning %ld\n", err);
    return err;
}

static DWORD SOFTPUB_DecodeInnerContent(CRYPT_PROVIDER_DATA *data)
{
    BOOL ret;
    DWORD size, err = ERROR_SUCCESS;
    LPSTR oid = NULL;
    LPBYTE buf = NULL;

    ret = CryptMsgGetParam(data->hMsg, CMSG_INNER_CONTENT_TYPE_PARAM, 0, NULL,
     &size);
    if (!ret)
    {
        err = GetLastError();
        goto error;
    }
    oid = data->psPfns->pfnAlloc(size);
    if (!oid)
    {
        err = ERROR_OUTOFMEMORY;
        goto error;
    }
    ret = CryptMsgGetParam(data->hMsg, CMSG_INNER_CONTENT_TYPE_PARAM, 0, oid,
     &size);
    if (!ret)
    {
        err = GetLastError();
        goto error;
    }
    ret = CryptMsgGetParam(data->hMsg, CMSG_CONTENT_PARAM, 0, NULL, &size);
    if (!ret)
    {
        err = GetLastError();
        goto error;
    }
    buf = data->psPfns->pfnAlloc(size);
    if (!buf)
    {
        err = ERROR_OUTOFMEMORY;
        goto error;
    }
    ret = CryptMsgGetParam(data->hMsg, CMSG_CONTENT_PARAM, 0, buf, &size);
    if (!ret)
    {
        err = GetLastError();
        goto error;
    }
    ret = CryptDecodeObject(data->dwEncoding, oid, buf, size, 0, NULL, &size);
    if (!ret)
    {
        err = GetLastError();
        goto error;
    }
    data->pPDSip->psIndirectData = data->psPfns->pfnAlloc(size);
    if (!data->pPDSip->psIndirectData)
    {
        err = ERROR_OUTOFMEMORY;
        goto error;
    }
    ret = CryptDecodeObject(data->dwEncoding, oid, buf, size, 0,
     data->pPDSip->psIndirectData, &size);
    if (!ret)
        err = GetLastError();

error:
    TRACE("returning %ld\n", err);
    data->psPfns->pfnFree(oid);
    data->psPfns->pfnFree(buf);
    return err;
}

static DWORD SOFTPUB_LoadCertMessage(CRYPT_PROVIDER_DATA *data)
{
    DWORD err = ERROR_SUCCESS;

    if (data->pWintrustData->pCert &&
     WVT_IS_CBSTRUCT_GT_MEMBEROFFSET(WINTRUST_CERT_INFO,
     data->pWintrustData->pCert->cbStruct, psCertContext))
    {
        if (data->psPfns)
        {
            CRYPT_PROVIDER_SGNR signer = { sizeof(signer), { 0 } };
            DWORD i;
            BOOL ret;

            /* Add a signer with nothing but the time to verify, so we can
             * add a cert to it
             */
            if (WVT_ISINSTRUCT(WINTRUST_CERT_INFO,
             data->pWintrustData->pCert->cbStruct, psftVerifyAsOf) &&
             data->pWintrustData->pCert->psftVerifyAsOf)
                data->sftSystemTime = signer.sftVerifyAsOf;
            else
            {
                SYSTEMTIME sysTime;

                GetSystemTime(&sysTime);
                SystemTimeToFileTime(&sysTime, &signer.sftVerifyAsOf);
            }
            ret = data->psPfns->pfnAddSgnr2Chain(data, FALSE, 0, &signer);
            if (ret)
            {
                ret = data->psPfns->pfnAddCert2Chain(data, 0, FALSE, 0,
                 data->pWintrustData->pCert->psCertContext);
                if (WVT_ISINSTRUCT(WINTRUST_CERT_INFO,
                 data->pWintrustData->pCert->cbStruct, pahStores))
                        for (i = 0;
                         ret && i < data->pWintrustData->pCert->chStores; i++)
                            ret = data->psPfns->pfnAddStore2Chain(data,
                             data->pWintrustData->pCert->pahStores[i]);
            }
            if (!ret)
                err = GetLastError();
        }
    }
    else
        err = ERROR_INVALID_PARAMETER;
    return err;
}

static DWORD SOFTPUB_LoadFileMessage(CRYPT_PROVIDER_DATA *data)
{
    DWORD err = ERROR_SUCCESS;

    if (!data->pWintrustData->pFile)
    {
        err = ERROR_INVALID_PARAMETER;
        goto error;
    }
    err = SOFTPUB_OpenFile(data);
    if (err)
        goto error;
    err = SOFTPUB_GetFileSubject(data);
    if (err)
        goto error;
    err = SOFTPUB_GetSIP(data);
    if (err)
        goto error;
    err = SOFTPUB_GetMessageFromFile(data, data->pWintrustData->pFile->hFile,
     data->pWintrustData->pFile->pcwszFilePath);
    if (err)
        goto error;
    err = SOFTPUB_CreateStoreFromMessage(data);
    if (err)
        goto error;
    err = SOFTPUB_DecodeInnerContent(data);
    if (err)
        goto error;
    err = SOFTPUB_VerifyImageHash(data, data->pWintrustData->pFile->hFile);

error:
    if (err && data->fOpenedFile && data->pWintrustData->pFile)
    {
        /* The caller won't expect the file to be open on failure, so close it.
         */
        CloseHandle(data->pWintrustData->pFile->hFile);
        data->pWintrustData->pFile->hFile = INVALID_HANDLE_VALUE;
        data->fOpenedFile = FALSE;
    }
    return err;
}

static DWORD SOFTPUB_LoadCatalogMessage(CRYPT_PROVIDER_DATA *data)
{
    DWORD err;
    HANDLE catalog = INVALID_HANDLE_VALUE;

    if (!data->pWintrustData->pCatalog)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    catalog = CreateFileW(data->pWintrustData->pCatalog->pcwszCatalogFilePath,
     GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
     NULL);
    if (catalog == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!CryptSIPRetrieveSubjectGuid(
     data->pWintrustData->pCatalog->pcwszCatalogFilePath, catalog,
     &data->pPDSip->gSubject))
    {
        err = GetLastError();
        goto error;
    }
    err = SOFTPUB_GetSIP(data);
    if (err)
        goto error;
    err = SOFTPUB_GetMessageFromFile(data, catalog,
     data->pWintrustData->pCatalog->pcwszCatalogFilePath);
    if (err)
        goto error;
    err = SOFTPUB_CreateStoreFromMessage(data);
    if (err)
        goto error;
    err = SOFTPUB_DecodeInnerContent(data);
    /* FIXME: this loads the catalog file, but doesn't validate the member. */
error:
    CloseHandle(catalog);
    return err;
}

HRESULT WINAPI SoftpubLoadMessage(CRYPT_PROVIDER_DATA *data)
{
    DWORD err = ERROR_SUCCESS;

    TRACE("(%p)\n", data);

    if (!data->padwTrustStepErrors)
        return S_FALSE;

    switch (data->pWintrustData->dwUnionChoice)
    {
    case WTD_CHOICE_CERT:
        err = SOFTPUB_LoadCertMessage(data);
        break;
    case WTD_CHOICE_FILE:
        err = SOFTPUB_LoadFileMessage(data);
        break;
    case WTD_CHOICE_CATALOG:
        err = SOFTPUB_LoadCatalogMessage(data);
        break;
    default:
        FIXME("unimplemented for %ld\n", data->pWintrustData->dwUnionChoice);
        err = ERROR_INVALID_PARAMETER;
    }

    if (err)
        data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_OBJPROV] = err;
    TRACE("returning %ld (%08lx)\n", !err ? S_OK : S_FALSE,
     data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_OBJPROV]);
    return !err ? S_OK : S_FALSE;
}

static CMSG_SIGNER_INFO *WINTRUST_GetSigner(CRYPT_PROVIDER_DATA *data,
 DWORD signerIdx)
{
    BOOL ret;
    CMSG_SIGNER_INFO *signerInfo = NULL;
    DWORD size;

    ret = CryptMsgGetParam(data->hMsg, CMSG_SIGNER_INFO_PARAM, signerIdx,
     NULL, &size);
    if (ret)
    {
        signerInfo = data->psPfns->pfnAlloc(size);
        if (signerInfo)
        {
            ret = CryptMsgGetParam(data->hMsg, CMSG_SIGNER_INFO_PARAM,
             signerIdx, signerInfo, &size);
            if (!ret)
            {
                data->psPfns->pfnFree(signerInfo);
                signerInfo = NULL;
            }
        }
        else
            SetLastError(ERROR_OUTOFMEMORY);
    }
    return signerInfo;
}

/* Time stamps.  The time stamp of a signature is unauthenticated data: a PKCS #9
 * countersignature (an unauthenticated attribute of type szOID_RSA_counterSign)
 * or an RFC 3161 token (type szOID_RFC3161_counterSign, 1.3.6.1.4.1.311.3.3.1,
 * a CMS SignedData over a TSTInfo structure).  Windows validates the signer's
 * chain as of the time stamp, so a signature that was time stamped while its
 * certificate was valid stays valid after the certificate expires.
 *
 * Because anybody can attach such an attribute, a time stamp is only believed
 * when
 *  - its own signature verifies and covers the data it claims to (for a token
 *    the TSTInfo, for a countersignature the primary signature's value);
 *  - it covers this signature: the message imprint of a token, the message
 *    digest of a countersignature, is the digest of the primary signature's
 *    value;
 *  - the time stamping authority's certificate chains to a trusted root, was
 *    valid at the time stamp and may sign time stamps (extended key usage
 *    id-kp-timeStamping);
 *  - it does not claim a time in the future.
 * Otherwise it is ignored and the signature is validated at the current time,
 * where an expired certificate fails. */
#ifndef szOID_RFC3161_counterSign
#define szOID_RFC3161_counterSign "1.3.6.1.4.1.311.3.3.1"
#endif
#ifndef szOID_TIMESTAMP_TOKEN
#define szOID_TIMESTAMP_TOKEN "1.2.840.113549.1.9.16.1.4"
#endif

/* Reads one DER tag/length header.  On success, content and len describe the
 * contents and the return value points past them. */
static const BYTE *WINTRUST_DerNext(const BYTE *p, const BYTE *end, BYTE *tag,
 const BYTE **content, DWORD *len)
{
    DWORD l, n;

    if (end - p < 2) return NULL;
    *tag = *p++;
    l = *p++;
    if (l & 0x80)
    {
        n = l & 0x7f;
        if (!n || n > 4 || (DWORD)(end - p) < n) return NULL;
        for (l = 0; n; n--) l = (l << 8) | *p++;
    }
    if (l > (DWORD)(end - p)) return NULL;
    *content = p;
    *len = l;
    return p + l;
}

static BOOL WINTRUST_ParseGeneralizedTime(const BYTE *s, DWORD len, FILETIME *time)
{
    SYSTEMTIME st = { 0 };
    DWORD i;

    /* YYYYMMDDHHMMSS[.fff]Z, RFC 3161 section 2.4.2 */
    if (len < 15 || s[len - 1] != 'Z') return FALSE;
    for (i = 0; i < 14; i++)
        if (s[i] < '0' || s[i] > '9') return FALSE;
#define D2(o) ((s[o] - '0') * 10 + (s[(o) + 1] - '0'))
    st.wYear = D2(0) * 100 + D2(2);
    st.wMonth = D2(4);
    st.wDay = D2(6);
    st.wHour = D2(8);
    st.wMinute = D2(10);
    st.wSecond = D2(12);
#undef D2
    if (len > 15)
    {
        DWORD ms = 0, digits = 0;

        if (s[14] != '.' || len == 16) return FALSE;
        for (i = 15; i < len - 1; i++)
        {
            if (s[i] < '0' || s[i] > '9') return FALSE;
            if (digits < 3) { ms = ms * 10 + (s[i] - '0'); digits++; }
        }
        while (digits++ < 3) ms *= 10;
        st.wMilliseconds = ms;
    }
    return SystemTimeToFileTime(&st, time);
}

/* An OBJECT IDENTIFIER's content octets as a dotted string */
static BOOL WINTRUST_OidToString(const BYTE *p, DWORD len, char *out, DWORD outSize)
{
    DWORD i, arc = 0, pos = 0;
    BOOL first = TRUE;
    char digits[11];

    if (!len) return FALSE;
    for (i = 0; i < len; i++)
    {
        if (arc > 0x01ffffff) return FALSE;
        arc = (arc << 7) | (p[i] & 0x7f);
        if (p[i] & 0x80)
        {
            if (i == len - 1) return FALSE;
            continue;
        }
        if (first)
        {
            DWORD top = arc < 40 ? 0 : arc < 80 ? 1 : 2;
            DWORD parts[2] = { top, arc - top * 40 };
            DWORD k;

            for (k = 0; k < 2; k++)
            {
                DWORD n = parts[k], d = 0;

                do { digits[d++] = '0' + n % 10; n /= 10; } while (n);
                if (pos + d + 1 >= outSize) return FALSE;
                while (d) out[pos++] = digits[--d];
                if (!k) out[pos++] = '.';
            }
            first = FALSE;
        }
        else
        {
            DWORD n = arc, d = 0;

            do { digits[d++] = '0' + n % 10; n /= 10; } while (n);
            if (pos + d + 2 >= outSize) return FALSE;
            out[pos++] = '.';
            while (d) out[pos++] = digits[--d];
        }
        arc = 0;
    }
    out[pos] = 0;
    return TRUE;
}

typedef struct _WINTRUST_TSTINFO
{
    FILETIME genTime;
    char hashOid[64];
    const BYTE *imprint;
    DWORD imprintLen;
} WINTRUST_TSTINFO;

/* TSTInfo ::= SEQUENCE { version INTEGER, policy OID,
 *                        messageImprint SEQUENCE { AlgorithmIdentifier, OCTET STRING },
 *                        serialNumber INTEGER, genTime GeneralizedTime, ... }
 * On success, *tstEnd - *tstStart is the DER encoding of the whole TSTInfo, the
 * octets the token's signature covers. */
static BOOL WINTRUST_ParseTSTInfo(const BYTE *p, DWORD len, WINTRUST_TSTINFO *info,
 const BYTE **tstStart, DWORD *tstLen)
{
    const BYTE *end = p + len, *c, *next, *mi, *miEnd, *ai, *aiEnd;
    DWORD l;
    BYTE tag;

    /* the encapsulated content may still be wrapped in its OCTET STRING */
    if (len && p[0] == 0x04)
    {
        next = WINTRUST_DerNext(p, end, &tag, &c, &l);
        if (!next || next != end) return FALSE;
        p = c;
        end = c + l;
    }
    *tstStart = p;
    *tstLen = end - p;
    next = WINTRUST_DerNext(p, end, &tag, &c, &l);
    if (!next || tag != 0x30 || next != end) return FALSE;
    p = c;
    end = c + l;
    /* version, policy */
    p = WINTRUST_DerNext(p, end, &tag, &c, &l);
    if (!p || tag != 0x02) return FALSE;
    p = WINTRUST_DerNext(p, end, &tag, &c, &l);
    if (!p || tag != 0x06) return FALSE;
    /* messageImprint */
    p = WINTRUST_DerNext(p, end, &tag, &mi, &l);
    if (!p || tag != 0x30) return FALSE;
    miEnd = mi + l;
    mi = WINTRUST_DerNext(mi, miEnd, &tag, &ai, &l);   /* AlgorithmIdentifier */
    if (!mi || tag != 0x30) return FALSE;
    aiEnd = ai + l;
    ai = WINTRUST_DerNext(ai, aiEnd, &tag, &c, &l);    /* algorithm OID, parameters ignored */
    if (!ai || tag != 0x06 || !WINTRUST_OidToString(c, l, info->hashOid, sizeof(info->hashOid)))
        return FALSE;
    mi = WINTRUST_DerNext(mi, miEnd, &tag, &info->imprint, &info->imprintLen);
    if (!mi || tag != 0x04 || mi != miEnd) return FALSE;
    /* serialNumber, genTime */
    p = WINTRUST_DerNext(p, end, &tag, &c, &l);
    if (!p || tag != 0x02) return FALSE;
    if (!WINTRUST_DerNext(p, end, &tag, &c, &l) || tag != 0x18) return FALSE;
    return WINTRUST_ParseGeneralizedTime(c, l, &info->genTime);
}

/* The digests the time stamps may use */
/* allowMd5: old Authenticode signatures digest their content with MD5, and
 * the PKCS #9 countersignatures of the time stamping services of the 2000s
 * were made with MD5 too; Windows still accepts both.  RFC 3161 time stamps
 * are only believed with SHA-1 and SHA-2. */
static ALG_ID WINTRUST_TimeStampHashAlg(const char *oid, BOOL allowMd5)
{
    ALG_ID alg;

    if (!oid) return 0;
    alg = CertOIDToAlgId(oid);
    if (alg == CALG_SHA1 || alg == CALG_SHA_256 || alg == CALG_SHA_384 || alg == CALG_SHA_512)
        return alg;
    if (allowMd5 && alg == CALG_MD5)
        return alg;
    return 0;
}

static BOOL WINTRUST_CreateHash(const char *oid, BOOL allowMd5, HCRYPTPROV *prov, HCRYPTHASH *hash)
{
    ALG_ID alg = WINTRUST_TimeStampHashAlg(oid, allowMd5);

    if (!alg || !CryptAcquireContextW(prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return FALSE;
    if (!CryptCreateHash(*prov, alg, 0, 0, hash))
    {
        CryptReleaseContext(*prov, 0);
        return FALSE;
    }
    return TRUE;
}

/* hash must have room for 64 octets */
static BOOL WINTRUST_HashBytes(const char *oid, BOOL allowMd5, const BYTE *data, DWORD len,
 BYTE *hash, DWORD *hashLen)
{
    HCRYPTPROV prov;
    HCRYPTHASH h;
    BOOL ret = FALSE;

    if (!WINTRUST_CreateHash(oid, allowMd5, &prov, &h)) return FALSE;
    *hashLen = 64;
    if (CryptHashData(h, data, len, 0))
        ret = CryptGetHashParam(h, HP_HASHVAL, hash, hashLen, 0);
    CryptDestroyHash(h);
    CryptReleaseContext(prov, 0);
    return ret;
}

/* The signed attributes must carry exactly one message digest, and it must be
 * the digest of data.  Whoever verifies the signature over the attributes
 * would otherwise not know that it covers the content. */
static BOOL WINTRUST_CheckMessageDigest(const CRYPT_ATTRIBUTES *attrs, const char *hashOid,
 BOOL allowMd5, const BYTE *data, DWORD len)
{
    DWORD i, size, found = 0;
    const CRYPT_ATTRIBUTE *digestAttr = NULL;
    CRYPT_DATA_BLOB *digest;
    BYTE hash[64];
    DWORD hashLen;
    BOOL ret = FALSE;

    for (i = 0; i < attrs->cAttr; i++)
    {
        if (!strcmp(attrs->rgAttr[i].pszObjId, szOID_RSA_messageDigest))
        {
            digestAttr = &attrs->rgAttr[i];
            found++;
        }
    }
    if (found != 1 || digestAttr->cValue != 1) return FALSE;
    if (!WINTRUST_HashBytes(hashOid, allowMd5, data, len, hash, &hashLen)) return FALSE;
    if (CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, X509_OCTET_STRING,
     digestAttr->rgValue[0].pbData, digestAttr->rgValue[0].cbData, CRYPT_DECODE_ALLOC_FLAG,
     NULL, &digest, &size))
    {
        ret = digest->cbData == hashLen && !memcmp(digest->pbData, hash, hashLen);
        LocalFree(digest);
    }
    return ret;
}

static BOOL WINTRUST_NotInFuture(const FILETIME *time)
{
    FILETIME now;

    GetSystemTimeAsFileTime(&now);
    return CompareFileTime(time, &now) <= 0;
}

/* A time stamping authority's certificate: it names id-kp-timeStamping, and its
 * chain, built as of the time stamp, ends in a trusted root with no error and
 * allows that usage throughout. */
static BOOL WINTRUST_CheckTimeStampCert(PCCERT_CONTEXT cert, HCERTSTORE store,
 const FILETIME *asOf)
{
    static const DWORD encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    char timeStamping[] = szOID_PKIX_KP_TIMESTAMP_SIGNING;
    LPSTR usages[] = { timeStamping };
    CERT_CHAIN_PARA para = { sizeof(para) };
    PCCERT_CHAIN_CONTEXT chain;
    PCERT_EXTENSION ext;
    CERT_ENHKEY_USAGE *eku;
    DWORD i, size;
    BOOL ret = FALSE;

    ext = CertFindExtension(szOID_ENHANCED_KEY_USAGE, cert->pCertInfo->cExtension,
     cert->pCertInfo->rgExtension);
    if (!ext) return FALSE;
    if (!CryptDecodeObjectEx(encoding, X509_ENHANCED_KEY_USAGE, ext->Value.pbData,
     ext->Value.cbData, CRYPT_DECODE_ALLOC_FLAG, NULL, &eku, &size))
        return FALSE;
    for (i = 0; i < eku->cUsageIdentifier; i++)
        if (!strcmp(eku->rgpszUsageIdentifier[i], szOID_PKIX_KP_TIMESTAMP_SIGNING))
            ret = TRUE;
    LocalFree(eku);
    if (!ret) return FALSE;

    ret = FALSE;
    para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    para.RequestedUsage.Usage.cUsageIdentifier = 1;
    para.RequestedUsage.Usage.rgpszUsageIdentifier = usages;
    if (CertGetCertificateChain(NULL, cert, (FILETIME *)asOf, store, &para, 0, NULL, &chain))
    {
        ret = !chain->TrustStatus.dwErrorStatus;
        CertFreeCertificateChain(chain);
    }
    return ret;
}

static BOOL WINTRUST_GetTimeFromCounterSigner(
 const CMSG_CMS_SIGNER_INFO *counterSignerInfo, FILETIME *time)
{
    DWORD i;
    BOOL foundTimeStamp = FALSE;

    for (i = 0; !foundTimeStamp && i < counterSignerInfo->AuthAttrs.cAttr; i++)
    {
        if (!strcmp(counterSignerInfo->AuthAttrs.rgAttr[i].pszObjId,
         szOID_RSA_signingTime))
        {
            const CRYPT_ATTRIBUTE *attr =
             &counterSignerInfo->AuthAttrs.rgAttr[i];
            DWORD j;

            for (j = 0; !foundTimeStamp && j < attr->cValue; j++)
            {
                static const DWORD encoding = X509_ASN_ENCODING |
                 PKCS_7_ASN_ENCODING;
                DWORD size = sizeof(FILETIME);

                foundTimeStamp = CryptDecodeObjectEx(encoding,
                 X509_CHOICE_OF_TIME,
                 attr->rgValue[j].pbData, attr->rgValue[j].cbData, 0, NULL,
                 time, &size);
            }
        }
    }
    return foundTimeStamp;
}

/* Verifies the signature of a SignerInfo that is not part of a message of its
 * own, over its DER encoded signed attributes. */
static BOOL WINTRUST_VerifySignerInfoSignature(const BYTE *der, DWORD len,
 const CMSG_CMS_SIGNER_INFO *info, PCCERT_CONTEXT cert)
{
    const BYTE *p, *end, *c, *attrs, *attrsEnd;
    BYTE tag, *buf, *sig;
    HCRYPTPROV prov;
    HCRYPTHASH hash;
    HCRYPTKEY key;
    DWORD l, i, sigLen = info->EncryptedHash.cbData;
    BOOL ret = FALSE;

    p = WINTRUST_DerNext(der, der + len, &tag, &c, &l);
    if (!p || tag != 0x30) return FALSE;
    p = c;
    end = c + l;
    /* version, sid, digestAlgorithm, then [0] IMPLICIT signedAttrs */
    for (i = 0; i < 3; i++)
    {
        p = WINTRUST_DerNext(p, end, &tag, &c, &l);
        if (!p) return FALSE;
    }
    attrs = p;
    attrsEnd = WINTRUST_DerNext(p, end, &tag, &c, &l);
    if (!attrsEnd || tag != 0xa0 || !sigLen) return FALSE;

    buf = HeapAlloc(GetProcessHeap(), 0, attrsEnd - attrs);
    sig = HeapAlloc(GetProcessHeap(), 0, sigLen);
    if (buf && sig && WINTRUST_CreateHash(info->HashAlgorithm.pszObjId, TRUE, &prov, &hash))
    {
        /* the signature covers the attributes encoded as a SET OF */
        memcpy(buf, attrs, attrsEnd - attrs);
        buf[0] = 0x31;
        /* CryptoAPI takes signatures in little endian order */
        for (i = 0; i < sigLen; i++)
            sig[i] = info->EncryptedHash.pbData[sigLen - 1 - i];
        if (CryptHashData(hash, buf, attrsEnd - attrs, 0) &&
         CryptImportPublicKeyInfo(prov, X509_ASN_ENCODING, &cert->pCertInfo->SubjectPublicKeyInfo,
         &key))
        {
            ret = CryptVerifySignatureW(hash, sig, sigLen, key, NULL, 0);
            CryptDestroyKey(key);
        }
        CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    HeapFree(GetProcessHeap(), 0, sig);
    return ret;
}

/* A PKCS #9 countersignature: a SignerInfo whose signed attributes hold the
 * signing time and the digest of the primary signature's value, signed by a time
 * stamping authority whose certificate is in the message. */
static BOOL WINTRUST_VerifyCounterSigner(const CRYPT_PROVIDER_DATA *data,
 const CMSG_SIGNER_INFO *signerInfo, const CRYPT_ATTR_BLOB *blob,
 const CMSG_CMS_SIGNER_INFO *counter, FILETIME *time)
{
    CERT_INFO id = { 0 };
    PCCERT_CONTEXT cert;
    BOOL ret = FALSE;

    if (!data->pahStores || !data->chStores ||
     counter->SignerId.dwIdChoice != CERT_ID_ISSUER_SERIAL_NUMBER ||
     !signerInfo->EncryptedHash.cbData)
        return FALSE;
    if (!WINTRUST_CheckMessageDigest(&counter->AuthAttrs, counter->HashAlgorithm.pszObjId, TRUE,
     signerInfo->EncryptedHash.pbData, signerInfo->EncryptedHash.cbData))
        return FALSE;
    if (!WINTRUST_GetTimeFromCounterSigner(counter, time) || !WINTRUST_NotInFuture(time))
        return FALSE;

    id.Issuer = counter->SignerId.IssuerSerialNumber.Issuer;
    id.SerialNumber = counter->SignerId.IssuerSerialNumber.SerialNumber;
    cert = CertGetSubjectCertificateFromStore(data->pahStores[0], data->dwEncoding, &id);
    if (cert)
    {
        ret = WINTRUST_VerifySignerInfoSignature(blob->pbData, blob->cbData, counter, cert) &&
         WINTRUST_CheckTimeStampCert(cert, data->pahStores[0], time);
        CertFreeCertificateContext(cert);
    }
    return ret;
}

/* The certificates a message carries, in a store of their own */
static HCERTSTORE WINTRUST_OpenMessageCertStore(HCRYPTMSG msg)
{
    HCERTSTORE store;
    DWORD count, size, i;
    BOOL ret;

    store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
    if (!store) return NULL;
    size = sizeof(count);
    ret = CryptMsgGetParam(msg, CMSG_CERT_COUNT_PARAM, 0, &count, &size);
    for (i = 0; ret && i < count; i++)
    {
        BYTE *buf;

        ret = CryptMsgGetParam(msg, CMSG_CERT_PARAM, i, NULL, &size);
        if (!ret) break;
        buf = HeapAlloc(GetProcessHeap(), 0, size);
        ret = buf && CryptMsgGetParam(msg, CMSG_CERT_PARAM, i, buf, &size) &&
         CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, buf, size,
         CERT_STORE_ADD_ALWAYS, NULL);
        HeapFree(GetProcessHeap(), 0, buf);
    }
    if (!ret)
    {
        CertCloseStore(store, 0);
        store = NULL;
    }
    return store;
}

/* The token's signed attributes bind its content, its signature binds the
 * attributes, its message imprint binds the primary signature's value. */
static BOOL WINTRUST_VerifyRFC3161Token(const CRYPT_ATTR_BLOB *token,
 const CMSG_SIGNER_INFO *primary, FILETIME *time)
{
    static const DWORD encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    HCRYPTMSG msg;
    BOOL ret = FALSE;
    DWORD size, type, count;
    char oid[64];
    BYTE *content = NULL, hash[64];
    const BYTE *tst;
    DWORD tstLen, hashLen;
    WINTRUST_TSTINFO info;
    CMSG_SIGNER_INFO *signer = NULL;
    CERT_INFO *certInfo = NULL;
    HCERTSTORE store = NULL;
    PCCERT_CONTEXT cert = NULL;

    if (!primary->EncryptedHash.cbData) return FALSE;
    msg = CryptMsgOpenToDecode(encoding, 0, 0, 0, NULL, NULL);
    if (!msg) return FALSE;
    if (!CryptMsgUpdate(msg, token->pbData, token->cbData, TRUE)) goto done;

    size = sizeof(type);
    if (!CryptMsgGetParam(msg, CMSG_TYPE_PARAM, 0, &type, &size) || type != CMSG_SIGNED) goto done;
    size = sizeof(oid);
    if (!CryptMsgGetParam(msg, CMSG_INNER_CONTENT_TYPE_PARAM, 0, oid, &size) ||
     strcmp(oid, szOID_TIMESTAMP_TOKEN))
        goto done;
    size = sizeof(count);
    if (!CryptMsgGetParam(msg, CMSG_SIGNER_COUNT_PARAM, 0, &count, &size) || count != 1) goto done;

    if (!CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, NULL, &size) ||
     !(content = HeapAlloc(GetProcessHeap(), 0, size)) ||
     !CryptMsgGetParam(msg, CMSG_CONTENT_PARAM, 0, content, &size))
        goto done;
    if (!WINTRUST_ParseTSTInfo(content, size, &info, &tst, &tstLen)) goto done;

    /* the message imprint is the digest of the primary signature's value */
    if (!WINTRUST_HashBytes(info.hashOid, FALSE, primary->EncryptedHash.pbData,
     primary->EncryptedHash.cbData, hash, &hashLen) ||
     info.imprintLen != hashLen || memcmp(info.imprint, hash, hashLen))
        goto done;
    if (!WINTRUST_NotInFuture(&info.genTime)) goto done;

    /* the signer: signed attributes that hold the digest of the TSTInfo */
    if (!CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &size) ||
     !(signer = HeapAlloc(GetProcessHeap(), 0, size)) ||
     !CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, signer, &size))
        goto done;
    if (!WINTRUST_CheckMessageDigest(&signer->AuthAttrs, signer->HashAlgorithm.pszObjId, FALSE, tst, tstLen))
        goto done;

    /* its certificate comes with the token; the signature must verify with it */
    if (!CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, NULL, &size) ||
     !(certInfo = HeapAlloc(GetProcessHeap(), 0, size)) ||
     !CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, certInfo, &size))
        goto done;
    store = WINTRUST_OpenMessageCertStore(msg);
    if (!store) goto done;
    cert = CertGetSubjectCertificateFromStore(store, encoding, certInfo);
    if (!cert) goto done;
    {
        CMSG_CTRL_VERIFY_SIGNATURE_EX_PARA para = { sizeof(para), 0, 0,
         CMSG_VERIFY_SIGNER_CERT, (LPVOID)cert };

        if (!CryptMsgControl(msg, 0, CMSG_CTRL_VERIFY_SIGNATURE_EX, &para)) goto done;
    }
    if (!WINTRUST_CheckTimeStampCert(cert, store, &info.genTime)) goto done;

    *time = info.genTime;
    ret = TRUE;
done:
    if (cert) CertFreeCertificateContext(cert);
    if (store) CertCloseStore(store, 0);
    HeapFree(GetProcessHeap(), 0, certInfo);
    HeapFree(GetProcessHeap(), 0, signer);
    HeapFree(GetProcessHeap(), 0, content);
    CryptMsgClose(msg);
    return ret;
}

static LPCSTR filetime_to_str(const FILETIME *time)
{
    static char date[80];
    char dateFmt[80]; /* sufficient for all versions of LOCALE_SSHORTDATE */
    SYSTEMTIME sysTime;

    if (!time) return NULL;

    GetLocaleInfoA(LOCALE_SYSTEM_DEFAULT, LOCALE_SSHORTDATE, dateFmt, ARRAY_SIZE(dateFmt));
    FileTimeToSystemTime(time, &sysTime);
    GetDateFormatA(LOCALE_SYSTEM_DEFAULT, 0, &sysTime, dateFmt, date, ARRAY_SIZE(date));
    return date;
}

static FILETIME WINTRUST_GetTimeFromSigner(const CRYPT_PROVIDER_DATA *data,
 const CMSG_SIGNER_INFO *signerInfo)
{
    DWORD i;
    FILETIME time;
    BOOL foundTimeStamp = FALSE;

    for (i = 0; !foundTimeStamp && i < signerInfo->UnauthAttrs.cAttr; i++)
    {
        if (!strcmp(signerInfo->UnauthAttrs.rgAttr[i].pszObjId,
         szOID_RSA_counterSign))
        {
            const CRYPT_ATTRIBUTE *attr = &signerInfo->UnauthAttrs.rgAttr[i];
            DWORD j;

            for (j = 0; j < attr->cValue; j++)
            {
                static const DWORD encoding = X509_ASN_ENCODING |
                 PKCS_7_ASN_ENCODING;
                CMSG_CMS_SIGNER_INFO *counterSignerInfo;
                DWORD size;
                BOOL ret = CryptDecodeObjectEx(encoding, CMS_SIGNER_INFO,
                 attr->rgValue[j].pbData, attr->rgValue[j].cbData,
                 CRYPT_DECODE_ALLOC_FLAG, NULL, &counterSignerInfo, &size);
                if (ret)
                {
                    if (!foundTimeStamp)
                    {
                        foundTimeStamp = WINTRUST_VerifyCounterSigner(data,
                         signerInfo, &attr->rgValue[j], counterSignerInfo,
                         &time);
                        if (!foundTimeStamp)
                            TRACE("countersignature not trusted, ignored\n");
                    }
                    LocalFree(counterSignerInfo);
                }
            }
        }
        else if (!strcmp(signerInfo->UnauthAttrs.rgAttr[i].pszObjId,
         szOID_RFC3161_counterSign))
        {
            const CRYPT_ATTRIBUTE *attr = &signerInfo->UnauthAttrs.rgAttr[i];
            DWORD j;

            for (j = 0; !foundTimeStamp && j < attr->cValue; j++)
            {
                foundTimeStamp = WINTRUST_VerifyRFC3161Token(
                 &attr->rgValue[j], signerInfo, &time);
                if (!foundTimeStamp)
                    TRACE("time stamp token not trusted, ignored\n");
            }
        }
    }
    if (!foundTimeStamp)
    {
        TRACE("returning system time %s\n",
         filetime_to_str(&data->sftSystemTime));
        time = data->sftSystemTime;
    }
    else
        TRACE("returning time from message %s\n", filetime_to_str(&time));
    return time;
}

static DWORD WINTRUST_SaveSigner(CRYPT_PROVIDER_DATA *data, DWORD signerIdx)
{
    DWORD err;
    CMSG_SIGNER_INFO *signerInfo = WINTRUST_GetSigner(data, signerIdx);

    if (signerInfo)
    {
        CRYPT_PROVIDER_SGNR sgnr = { sizeof(sgnr), { 0 } };

        sgnr.psSigner = signerInfo;
        sgnr.sftVerifyAsOf = WINTRUST_GetTimeFromSigner(data, signerInfo);
        if (!data->psPfns->pfnAddSgnr2Chain(data, FALSE, signerIdx, &sgnr))
            err = GetLastError();
        else
            err = ERROR_SUCCESS;
    }
    else
        err = GetLastError();
    return err;
}

static CERT_INFO *WINTRUST_GetSignerCertInfo(CRYPT_PROVIDER_DATA *data,
 DWORD signerIdx)
{
    BOOL ret;
    CERT_INFO *certInfo = NULL;
    DWORD size;

    ret = CryptMsgGetParam(data->hMsg, CMSG_SIGNER_CERT_INFO_PARAM, signerIdx,
     NULL, &size);
    if (ret)
    {
        certInfo = data->psPfns->pfnAlloc(size);
        if (certInfo)
        {
            ret = CryptMsgGetParam(data->hMsg, CMSG_SIGNER_CERT_INFO_PARAM,
             signerIdx, certInfo, &size);
            if (!ret)
            {
                data->psPfns->pfnFree(certInfo);
                certInfo = NULL;
            }
        }
        else
            SetLastError(ERROR_OUTOFMEMORY);
    }
    return certInfo;
}

/* The contents octets of the (BER or DER) value at the start of content:
 * definite or indefinite length, and nothing after it. */
static BOOL WINTRUST_ContentOctets(const BYTE *content, DWORD size,
 const BYTE **octets, DWORD *len)
{
    const BYTE *p = content, *end = content + size;
    BYTE tag;

    if (size < 2) return FALSE;
    if ((content[0] & 0x1f) == 0x1f) return FALSE;      /* no high tag numbers here */
    if (content[1] == 0x80)
    {
        /* indefinite length, constructed only: up to the end-of-contents octets */
        if (!(content[0] & 0x20) || size < 4 || end[-1] || end[-2]) return FALSE;
        *octets = content + 2;
        *len = size - 4;
        return TRUE;
    }
    return (p = WINTRUST_DerNext(content, end, &tag, octets, len)) == end;
}

/* The signature of a signer with signed attributes covers those attributes
 * only: the content is bound to it by the attributes' message digest, which has
 * to be compared with the digest of the content.  That digest is taken over the
 * contents octets of the encapsulated content: for id-data (which crypt32
 * returns already decoded) the data itself; for any other type, such as
 * Authenticode's SpcIndirectDataContent or a catalog's certificate trust list,
 * the value without its tag and length.  A digest of the whole encoding binds
 * the content as well, and is accepted too: some signing tools produce it. */
static BOOL WINTRUST_CheckSignerContentDigest(CRYPT_PROVIDER_DATA *data,
 DWORD signerIdx)
{
    const CMSG_SIGNER_INFO *signerInfo = data->pasSigners[signerIdx].psSigner;
    const char *hashOid = signerInfo->HashAlgorithm.pszObjId;
    DWORD size, typeSize = 0, len;
    BYTE *content;
    char *type = NULL;
    const BYTE *octets;
    BOOL ret = FALSE;

    if (!signerInfo->AuthAttrs.cAttr) return TRUE;
    if (CryptMsgGetParam(data->hMsg, CMSG_INNER_CONTENT_TYPE_PARAM, 0, NULL, &typeSize) &&
     (type = HeapAlloc(GetProcessHeap(), 0, typeSize + 1)))
    {
        if (CryptMsgGetParam(data->hMsg, CMSG_INNER_CONTENT_TYPE_PARAM, 0, type, &typeSize))
            type[typeSize] = 0;
        else
            type[0] = 0;
    }
    if (!CryptMsgGetParam(data->hMsg, CMSG_CONTENT_PARAM, 0, NULL, &size) ||
     !(content = HeapAlloc(GetProcessHeap(), 0, size ? size : 1)))
    {
        HeapFree(GetProcessHeap(), 0, type);
        return FALSE;
    }
    if (CryptMsgGetParam(data->hMsg, CMSG_CONTENT_PARAM, 0, content, &size))
    {
        if (type && !strcmp(type, szOID_RSA_data))
            ret = WINTRUST_CheckMessageDigest(&signerInfo->AuthAttrs, hashOid, TRUE,
             content, size);
        else
        {
            if (WINTRUST_ContentOctets(content, size, &octets, &len))
                ret = WINTRUST_CheckMessageDigest(&signerInfo->AuthAttrs, hashOid,
                 TRUE, octets, len);
            if (!ret)
                ret = WINTRUST_CheckMessageDigest(&signerInfo->AuthAttrs, hashOid,
                 TRUE, content, size);
        }
    }
    if (!ret)
        TRACE("signer %ld: the message digest does not match the content (type %s, %ld bytes)\n",
         signerIdx, debugstr_a(type), size);
    HeapFree(GetProcessHeap(), 0, content);
    HeapFree(GetProcessHeap(), 0, type);
    return ret;
}

static DWORD WINTRUST_VerifySigner(CRYPT_PROVIDER_DATA *data, DWORD signerIdx)
{
    DWORD err;
    CERT_INFO *certInfo = WINTRUST_GetSignerCertInfo(data, signerIdx);

    if (certInfo)
    {
        PCCERT_CONTEXT subject = CertGetSubjectCertificateFromStore(
         data->pahStores[0], data->dwEncoding, certInfo);

        if (subject)
        {
            CMSG_CTRL_VERIFY_SIGNATURE_EX_PARA para = { sizeof(para), 0,
             signerIdx, CMSG_VERIFY_SIGNER_CERT, (LPVOID)subject };

            if (!CryptMsgControl(data->hMsg, 0, CMSG_CTRL_VERIFY_SIGNATURE_EX,
             &para))
                err = TRUST_E_CERT_SIGNATURE;
            else if (!WINTRUST_CheckSignerContentDigest(data, signerIdx))
                err = TRUST_E_NOSIGNATURE;
            else
            {
                data->psPfns->pfnAddCert2Chain(data, signerIdx, FALSE, 0,
                 subject);
                err = ERROR_SUCCESS;
            }
            CertFreeCertificateContext(subject);
        }
        else
            err = TRUST_E_NO_SIGNER_CERT;
        data->psPfns->pfnFree(certInfo);
    }
    else
        err = GetLastError();
    return err;
}

static void load_secondary_signatures(CRYPT_PROVIDER_DATA *data, HCRYPTMSG msg)
{
    CRYPT_PROVIDER_SIGSTATE *s = data->pSigState;
    CRYPT_ATTRIBUTES *attrs;
    unsigned int i, j;
    DWORD size;

    if (!CryptMsgGetParam(msg, CMSG_SIGNER_UNAUTH_ATTR_PARAM, 0, NULL, &size))
        return;

    if (!(attrs = data->psPfns->pfnAlloc(size)))
    {
        ERR("No memory.\n");
        return;
    }
    if (!CryptMsgGetParam(msg, CMSG_SIGNER_UNAUTH_ATTR_PARAM, 0, attrs, &size))
        goto done;

    for (i = 0; i < attrs->cAttr; ++i)
    {
        if (strcmp(attrs->rgAttr[i].pszObjId, szOID_NESTED_SIGNATURE))
            continue;

        if (!(s->rhSecondarySigs = data->psPfns->pfnAlloc(attrs->rgAttr[i].cValue * sizeof(*s->rhSecondarySigs))))
        {
            ERR("No memory.\n");
            goto done;
        }
        s->cSecondarySigs = 0;
        for (j = 0; j < attrs->rgAttr[i].cValue; ++j)
        {
            if (!(msg = CryptMsgOpenToDecode(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, 0, 0, NULL, NULL)))
            {
                ERR("Could not create crypt message.\n");
                goto done;
            }
            if (!CryptMsgUpdate(msg, attrs->rgAttr[i].rgValue[j].pbData, attrs->rgAttr[i].rgValue[j].cbData, TRUE))
            {
                ERR("Could not update crypt message, err %lu.\n", GetLastError());
                CryptMsgClose(msg);
                goto done;
            }
            s->rhSecondarySigs[j] = msg;
            ++s->cSecondarySigs;
        }
        break;
    }
done:
    data->psPfns->pfnFree(attrs);
}

HRESULT WINAPI SoftpubLoadSignature(CRYPT_PROVIDER_DATA *data)
{
    DWORD err = ERROR_SUCCESS;

    TRACE("(%p)\n", data);

    if (!data->padwTrustStepErrors)
        return S_FALSE;

    if (data->pSigState)
    {
        /* We did not initialize this, probably an unsupported usage. */
        FIXME("pSigState %p already initialized.\n", data->pSigState);
    }
    if (!(data->pSigState = data->psPfns->pfnAlloc(sizeof(*data->pSigState))))
    {
        err = ERROR_OUTOFMEMORY;
    }
    else
    {
        data->pSigState->cbStruct = sizeof(*data->pSigState);
        data->pSigState->fSupportMultiSig = TRUE;
        data->pSigState->dwCryptoPolicySupport = WSS_SIGTRUST_SUPPORT | WSS_OBJTRUST_SUPPORT | WSS_CERTTRUST_SUPPORT;
        if (data->hMsg)
        {
            data->pSigState->hPrimarySig = CryptMsgDuplicate(data->hMsg);
            load_secondary_signatures(data, data->pSigState->hPrimarySig);
        }
        if (data->pSigSettings)
        {
            if (data->pSigSettings->dwFlags & WSS_GET_SECONDARY_SIG_COUNT)
                data->pSigSettings->cSecondarySigs = data->pSigState->cSecondarySigs;
        }
    }

    if (!err && data->hMsg)
    {
        DWORD signerCount, size;

        size = sizeof(signerCount);
        if (CryptMsgGetParam(data->hMsg, CMSG_SIGNER_COUNT_PARAM, 0,
         &signerCount, &size))
        {
            DWORD i;

            err = ERROR_SUCCESS;
            for (i = 0; !err && i < signerCount; i++)
            {
                if (!(err = WINTRUST_SaveSigner(data, i)))
                    err = WINTRUST_VerifySigner(data, i);
            }
        }
        else
            err = TRUST_E_NOSIGNATURE;
    }

    if (err)
        data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_SIGPROV] = err;
    return !err ? S_OK : S_FALSE;
}

static DWORD WINTRUST_TrustStatusToConfidence(DWORD errorStatus)
{
    DWORD confidence = 0;

    confidence = 0;
    if (!(errorStatus & CERT_TRUST_IS_NOT_SIGNATURE_VALID))
        confidence |= CERT_CONFIDENCE_SIG;
    if (!(errorStatus & CERT_TRUST_IS_NOT_TIME_VALID))
        confidence |= CERT_CONFIDENCE_TIME;
    if (!(errorStatus & CERT_TRUST_IS_NOT_TIME_NESTED))
        confidence |= CERT_CONFIDENCE_TIMENEST;
    return confidence;
}

BOOL WINAPI SoftpubCheckCert(CRYPT_PROVIDER_DATA *data, DWORD idxSigner,
 BOOL fCounterSignerChain, DWORD idxCounterSigner)
{
    BOOL ret;

    TRACE("(%p, %ld, %d, %ld)\n", data, idxSigner, fCounterSignerChain,
     idxCounterSigner);

    if (fCounterSignerChain)
    {
        FIXME("unimplemented for counter signers\n");
        ret = FALSE;
    }
    else
    {
        PCERT_SIMPLE_CHAIN simpleChain =
         data->pasSigners[idxSigner].pChainContext->rgpChain[0];
        DWORD i;

        ret = TRUE;
        for (i = 0; i < simpleChain->cElement; i++)
        {
            /* Set confidence */
            data->pasSigners[idxSigner].pasCertChain[i].dwConfidence =
             WINTRUST_TrustStatusToConfidence(
             simpleChain->rgpElement[i]->TrustStatus.dwErrorStatus);
            /* Set additional flags */
            if (!(simpleChain->rgpElement[i]->TrustStatus.dwErrorStatus &
             CERT_TRUST_IS_UNTRUSTED_ROOT))
                data->pasSigners[idxSigner].pasCertChain[i].fTrustedRoot = TRUE;
            if (simpleChain->rgpElement[i]->TrustStatus.dwInfoStatus &
             CERT_TRUST_IS_SELF_SIGNED)
                data->pasSigners[idxSigner].pasCertChain[i].fSelfSigned = TRUE;
            if (simpleChain->rgpElement[i]->TrustStatus.dwErrorStatus &
             CERT_TRUST_IS_CYCLIC)
                data->pasSigners[idxSigner].pasCertChain[i].fIsCyclic = TRUE;
        }
    }
    return ret;
}

static DWORD WINTRUST_TrustStatusToError(DWORD errorStatus)
{
    DWORD error;

    if (errorStatus & CERT_TRUST_IS_NOT_SIGNATURE_VALID)
        error = TRUST_E_CERT_SIGNATURE;
    else if (errorStatus & CERT_TRUST_IS_UNTRUSTED_ROOT)
        error = CERT_E_UNTRUSTEDROOT;
    else if (errorStatus & CERT_TRUST_IS_NOT_TIME_VALID)
        error = CERT_E_EXPIRED;
    else if (errorStatus & CERT_TRUST_IS_NOT_TIME_NESTED)
        error = CERT_E_VALIDITYPERIODNESTING;
    else if (errorStatus & CERT_TRUST_IS_REVOKED)
        error = CERT_E_REVOKED;
    else if (errorStatus & CERT_TRUST_IS_OFFLINE_REVOCATION ||
     errorStatus & CERT_TRUST_REVOCATION_STATUS_UNKNOWN)
        error = CERT_E_REVOCATION_FAILURE;
    else if (errorStatus & CERT_TRUST_IS_NOT_VALID_FOR_USAGE)
        error = CERT_E_WRONG_USAGE;
    else if (errorStatus & CERT_TRUST_IS_CYCLIC)
        error = CERT_E_CHAINING;
    else if (errorStatus & CERT_TRUST_INVALID_EXTENSION)
        error = CERT_E_CRITICAL;
    else if (errorStatus & CERT_TRUST_INVALID_POLICY_CONSTRAINTS)
        error = CERT_E_INVALID_POLICY;
    else if (errorStatus & CERT_TRUST_INVALID_BASIC_CONSTRAINTS)
        error = TRUST_E_BASIC_CONSTRAINTS;
    else if (errorStatus & CERT_TRUST_INVALID_NAME_CONSTRAINTS ||
     errorStatus & CERT_TRUST_HAS_NOT_SUPPORTED_NAME_CONSTRAINT ||
     errorStatus & CERT_TRUST_HAS_NOT_DEFINED_NAME_CONSTRAINT ||
     errorStatus & CERT_TRUST_HAS_NOT_PERMITTED_NAME_CONSTRAINT ||
     errorStatus & CERT_TRUST_HAS_EXCLUDED_NAME_CONSTRAINT)
        error = CERT_E_INVALID_NAME;
    else if (errorStatus & CERT_TRUST_NO_ISSUANCE_CHAIN_POLICY)
        error = CERT_E_INVALID_POLICY;
    else if (errorStatus)
    {
        FIXME("unknown error status %08lx\n", errorStatus);
        error = TRUST_E_SYSTEM_ERROR;
    }
    else
        error = S_OK;
    return error;
}

static DWORD WINTRUST_CopyChain(CRYPT_PROVIDER_DATA *data, DWORD signerIdx)
{
    DWORD err, i;
    PCERT_SIMPLE_CHAIN simpleChain =
     data->pasSigners[signerIdx].pChainContext->rgpChain[0];

    data->pasSigners[signerIdx].pasCertChain[0].dwConfidence =
     WINTRUST_TrustStatusToConfidence(
     simpleChain->rgpElement[0]->TrustStatus.dwErrorStatus);
    data->pasSigners[signerIdx].pasCertChain[0].pChainElement =
     simpleChain->rgpElement[0];
    err = ERROR_SUCCESS;
    for (i = 1; !err && i < simpleChain->cElement; i++)
    {
        if (data->psPfns->pfnAddCert2Chain(data, signerIdx, FALSE, 0,
         simpleChain->rgpElement[i]->pCertContext))
        {
            data->pasSigners[signerIdx].pasCertChain[i].pChainElement =
             simpleChain->rgpElement[i];
            data->pasSigners[signerIdx].pasCertChain[i].dwConfidence =
             WINTRUST_TrustStatusToConfidence(
             simpleChain->rgpElement[i]->TrustStatus.dwErrorStatus);
        }
        else
            err = GetLastError();
    }
    data->pasSigners[signerIdx].pasCertChain[simpleChain->cElement - 1].dwError
     = WINTRUST_TrustStatusToError(
     simpleChain->rgpElement[simpleChain->cElement - 1]->
     TrustStatus.dwErrorStatus);
    return err;
}

static void WINTRUST_CreateChainPolicyCreateInfo(
 const CRYPT_PROVIDER_DATA *data, PWTD_GENERIC_CHAIN_POLICY_CREATE_INFO info,
 PCERT_CHAIN_PARA chainPara)
{
    chainPara->cbSize = sizeof(CERT_CHAIN_PARA);
    if (data->pRequestUsage)
        chainPara->RequestedUsage = *data->pRequestUsage;
    else
    {
        chainPara->RequestedUsage.dwType = 0;
        chainPara->RequestedUsage.Usage.cUsageIdentifier = 0;
    }
    info->cbSize = sizeof(WTD_GENERIC_CHAIN_POLICY_CREATE_INFO);
    info->hChainEngine = NULL;
    info->pChainPara = chainPara;
    if (data->dwProvFlags & CPD_REVOCATION_CHECK_END_CERT)
        info->dwFlags = CERT_CHAIN_REVOCATION_CHECK_END_CERT;
    else if (data->dwProvFlags & CPD_REVOCATION_CHECK_CHAIN)
        info->dwFlags = CERT_CHAIN_REVOCATION_CHECK_CHAIN;
    else if (data->dwProvFlags & CPD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT)
        info->dwFlags = CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    else
        info->dwFlags = 0;
    info->pvReserved = NULL;
}

static DWORD WINTRUST_CreateChainForSigner(CRYPT_PROVIDER_DATA *data,
 DWORD signer, PWTD_GENERIC_CHAIN_POLICY_CREATE_INFO createInfo,
 PCERT_CHAIN_PARA chainPara)
{
    DWORD err = ERROR_SUCCESS;
    HCERTSTORE store = NULL;

    if (data->chStores)
    {
        store = CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0,
         CERT_STORE_CREATE_NEW_FLAG, NULL);
        if (store)
        {
            DWORD i;

            for (i = 0; i < data->chStores; i++)
                CertAddStoreToCollection(store, data->pahStores[i], 0, 0);
        }
        else
            err = GetLastError();
    }
    if (!err)
    {
        /* Expect the end certificate for each signer to be the only cert in
         * the chain:
         */
        if (data->pasSigners[signer].csCertChain)
        {
            BOOL ret;

            /* Create a certificate chain for each signer */
            ret = CertGetCertificateChain(createInfo->hChainEngine,
             data->pasSigners[signer].pasCertChain[0].pCert,
             &data->pasSigners[signer].sftVerifyAsOf, store,
             chainPara, createInfo->dwFlags, createInfo->pvReserved,
             &data->pasSigners[signer].pChainContext);
            if (ret)
            {
                if (data->pasSigners[signer].pChainContext->cChain != 1)
                {
                    FIXME("unimplemented for more than 1 simple chain\n");
                    err = E_NOTIMPL;
                }
                else
                {
                    if (!(err = WINTRUST_CopyChain(data, signer)))
                    {
                        if (data->psPfns->pfnCertCheckPolicy)
                        {
                            ret = data->psPfns->pfnCertCheckPolicy(data, signer,
                             FALSE, 0);
                            if (!ret)
                                err = GetLastError();
                        }
                        else
                            TRACE(
                             "no cert check policy, skipping policy check\n");
                    }
                }
            }
            else
                err = GetLastError();
        }
        CertCloseStore(store, 0);
    }
    return err;
}

HRESULT WINAPI WintrustCertificateTrust(CRYPT_PROVIDER_DATA *data)
{
    DWORD err;

    TRACE("(%p)\n", data);

    if (!data->csSigners)
        err = TRUST_E_NOSIGNATURE;
    else
    {
        DWORD i;
        WTD_GENERIC_CHAIN_POLICY_CREATE_INFO createInfo;
        CERT_CHAIN_PARA chainPara;

        WINTRUST_CreateChainPolicyCreateInfo(data, &createInfo, &chainPara);
        err = ERROR_SUCCESS;
        for (i = 0; !err && i < data->csSigners; i++)
            err = WINTRUST_CreateChainForSigner(data, i, &createInfo,
             &chainPara);
    }
    if (err)
        data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_CERTPROV] = err;
    TRACE("returning %ld (%08lx)\n", !err ? S_OK : S_FALSE,
     data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_CERTPROV]);
    return !err ? S_OK : S_FALSE;
}

HRESULT WINAPI GenericChainCertificateTrust(CRYPT_PROVIDER_DATA *data)
{
    DWORD err;
    WTD_GENERIC_CHAIN_POLICY_DATA *policyData =
     data->pWintrustData->pPolicyCallbackData;

    TRACE("(%p)\n", data);

    if (policyData && policyData->cbSize !=
     sizeof(WTD_GENERIC_CHAIN_POLICY_CREATE_INFO))
    {
        err = ERROR_INVALID_PARAMETER;
        goto end;
    }
    if (!data->csSigners)
        err = TRUST_E_NOSIGNATURE;
    else
    {
        DWORD i;
        WTD_GENERIC_CHAIN_POLICY_CREATE_INFO createInfo, *pCreateInfo;
        CERT_CHAIN_PARA chainPara, *pChainPara;

        if (policyData)
        {
            pCreateInfo = policyData->pSignerChainInfo;
            pChainPara = pCreateInfo->pChainPara;
        }
        else
        {
            WINTRUST_CreateChainPolicyCreateInfo(data, &createInfo, &chainPara);
            pChainPara = &chainPara;
            pCreateInfo = &createInfo;
        }
        err = ERROR_SUCCESS;
        for (i = 0; !err && i < data->csSigners; i++)
            err = WINTRUST_CreateChainForSigner(data, i, pCreateInfo,
             pChainPara);
    }

end:
    if (err)
        data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_CERTPROV] = err;
    TRACE("returning %ld (%08lx)\n", !err ? S_OK : S_FALSE,
     data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_CERTPROV]);
    return !err ? S_OK : S_FALSE;
}

/* Whether the certificate is in the store of disallowed certificates.  Looked
 * up by its signature hash when it has one (crypt32 cannot compute it for
 * every signature algorithm, e.g. ECDSA) and by its SHA-1 thumbprint, which
 * every certificate has: a certificate must not escape the store because one
 * hash is unavailable. */
static BOOL WINTRUST_IsCertDisallowed(PCCERT_CONTEXT cert)
{
    static const WCHAR disallowedW[] =
     { 'D','i','s','a','l','l','o','w','e','d',0 };
    HCERTSTORE disallowed;
    PCCERT_CONTEXT found = NULL;
    BYTE hash[64];
    DWORD size;

    disallowed = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, X509_ASN_ENCODING, 0,
     CERT_SYSTEM_STORE_CURRENT_USER, disallowedW);
    if (!disallowed)
        return FALSE;
    /* The signature hash is as long as the certificate's signature
     * algorithm's digest: 32 octets for SHA-256, up to 64. */
    size = sizeof(hash);
    if (CertGetCertificateContextProperty(cert, CERT_SIGNATURE_HASH_PROP_ID,
     hash, &size))
    {
        CRYPT_HASH_BLOB blob = { size, hash };

        found = CertFindCertificateInStore(disallowed, X509_ASN_ENCODING, 0,
         CERT_FIND_SIGNATURE_HASH, &blob, NULL);
    }
    size = sizeof(hash);
    if (!found && CertGetCertificateContextProperty(cert, CERT_HASH_PROP_ID,
     hash, &size))
    {
        CRYPT_HASH_BLOB blob = { size, hash };

        found = CertFindCertificateInStore(disallowed, X509_ASN_ENCODING, 0,
         CERT_FIND_SHA1_HASH, &blob, NULL);
    }
    if (!found)
        found = CertFindCertificateInStore(disallowed, X509_ASN_ENCODING, 0,
         CERT_FIND_EXISTING, cert, NULL);
    if (found)
        CertFreeCertificateContext(found);
    CertCloseStore(disallowed, 0);
    return found != NULL;
}

HRESULT WINAPI SoftpubAuthenticode(CRYPT_PROVIDER_DATA *data)
{
    BOOL ret;
    CERT_CHAIN_POLICY_STATUS policyStatus = { sizeof(policyStatus), 0 };

    TRACE("(%p)\n", data);

    if (data->pWintrustData->dwUIChoice != WTD_UI_NONE)
        FIXME("unimplemented for UI choice %ld\n",
         data->pWintrustData->dwUIChoice);
    if (!data->csSigners)
    {
        ret = FALSE;
        policyStatus.dwError = TRUST_E_NOSIGNATURE;
    }
    else
    {
        DWORD i;

        ret = TRUE;
        for (i = 0; ret && i < data->csSigners; i++)
        {
            /* First make sure cert isn't disallowed. */
            if (WINTRUST_IsCertDisallowed(
             data->pasSigners[i].pasCertChain[0].pCert))
            {
                /* Disallowed!  Can't verify it. */
                policyStatus.dwError = TRUST_E_SUBJECT_NOT_TRUSTED;
                ret = FALSE;
            }
            if (ret)
            {
                CERT_CHAIN_POLICY_PARA policyPara = { sizeof(policyPara), 0 };

                if (data->dwRegPolicySettings & WTPF_TRUSTTEST)
                    policyPara.dwFlags |= CERT_CHAIN_POLICY_TRUST_TESTROOT_FLAG;
                if (data->dwRegPolicySettings & WTPF_TESTCANBEVALID)
                    policyPara.dwFlags |= CERT_CHAIN_POLICY_ALLOW_TESTROOT_FLAG;
                if (data->dwRegPolicySettings & WTPF_IGNOREEXPIRATION)
                    policyPara.dwFlags |=
                     CERT_CHAIN_POLICY_IGNORE_NOT_TIME_VALID_FLAG |
                     CERT_CHAIN_POLICY_IGNORE_CTL_NOT_TIME_VALID_FLAG |
                     CERT_CHAIN_POLICY_IGNORE_NOT_TIME_NESTED_FLAG;
                if (data->dwRegPolicySettings & WTPF_IGNOREREVOKATION)
                    policyPara.dwFlags |=
                     CERT_CHAIN_POLICY_IGNORE_END_REV_UNKNOWN_FLAG |
                     CERT_CHAIN_POLICY_IGNORE_CTL_SIGNER_REV_UNKNOWN_FLAG |
                     CERT_CHAIN_POLICY_IGNORE_CA_REV_UNKNOWN_FLAG |
                     CERT_CHAIN_POLICY_IGNORE_ROOT_REV_UNKNOWN_FLAG;
                CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_AUTHENTICODE,
                 data->pasSigners[i].pChainContext, &policyPara, &policyStatus);
                if (policyStatus.dwError != NO_ERROR)
                    ret = FALSE;
            }
        }
    }
    if (!ret)
        data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_POLICYPROV] =
         policyStatus.dwError;
    TRACE("returning %ld (%08lx)\n", ret ? S_OK : S_FALSE,
     data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_POLICYPROV]);
    return ret ? S_OK : S_FALSE;
}

static HRESULT WINAPI WINTRUST_DefaultPolicy(CRYPT_PROVIDER_DATA *pProvData,
 DWORD dwStepError, DWORD dwRegPolicySettings, DWORD cSigner,
 PWTD_GENERIC_CHAIN_POLICY_SIGNER_INFO rgpSigner, void *pvPolicyArg)
{
    DWORD i;
    CERT_CHAIN_POLICY_STATUS policyStatus = { sizeof(policyStatus), 0 };

    for (i = 0; !policyStatus.dwError && i < cSigner; i++)
    {
        CERT_CHAIN_POLICY_PARA policyPara = { sizeof(policyPara), 0 };

        if (dwRegPolicySettings & WTPF_IGNOREEXPIRATION)
            policyPara.dwFlags |=
             CERT_CHAIN_POLICY_IGNORE_NOT_TIME_VALID_FLAG |
             CERT_CHAIN_POLICY_IGNORE_CTL_NOT_TIME_VALID_FLAG |
             CERT_CHAIN_POLICY_IGNORE_NOT_TIME_NESTED_FLAG;
        if (dwRegPolicySettings & WTPF_IGNOREREVOKATION)
            policyPara.dwFlags |=
             CERT_CHAIN_POLICY_IGNORE_END_REV_UNKNOWN_FLAG |
             CERT_CHAIN_POLICY_IGNORE_CTL_SIGNER_REV_UNKNOWN_FLAG |
             CERT_CHAIN_POLICY_IGNORE_CA_REV_UNKNOWN_FLAG |
             CERT_CHAIN_POLICY_IGNORE_ROOT_REV_UNKNOWN_FLAG;
        CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_BASE,
         rgpSigner[i].pChainContext, &policyPara, &policyStatus);
    }
    return policyStatus.dwError;
}

HRESULT WINAPI GenericChainFinalProv(CRYPT_PROVIDER_DATA *data)
{
    HRESULT err = NO_ERROR; /* not a typo, MS confused the types */
    WTD_GENERIC_CHAIN_POLICY_DATA *policyData =
     data->pWintrustData->pPolicyCallbackData;

    TRACE("(%p)\n", data);

    if (data->pWintrustData->dwUIChoice != WTD_UI_NONE)
        FIXME("unimplemented for UI choice %ld\n",
         data->pWintrustData->dwUIChoice);
    if (!data->csSigners)
        err = TRUST_E_NOSIGNATURE;
    else
    {
        PFN_WTD_GENERIC_CHAIN_POLICY_CALLBACK policyCallback;
        void *policyArg;
        WTD_GENERIC_CHAIN_POLICY_SIGNER_INFO *signers = NULL;

        if (policyData)
        {
            policyCallback = policyData->pfnPolicyCallback;
            policyArg = policyData->pvPolicyArg;
        }
        else
        {
            policyCallback = WINTRUST_DefaultPolicy;
            policyArg = NULL;
        }
        if (data->csSigners)
        {
            DWORD i;

            signers = data->psPfns->pfnAlloc(
             data->csSigners * sizeof(WTD_GENERIC_CHAIN_POLICY_SIGNER_INFO));
            if (signers)
            {
                for (i = 0; i < data->csSigners; i++)
                {
                    signers[i].cbSize =
                     sizeof(WTD_GENERIC_CHAIN_POLICY_SIGNER_INFO);
                    signers[i].pChainContext =
                     data->pasSigners[i].pChainContext;
                    signers[i].dwSignerType = data->pasSigners[i].dwSignerType;
                    signers[i].pMsgSignerInfo = data->pasSigners[i].psSigner;
                    signers[i].dwError = data->pasSigners[i].dwError;
                    if (data->pasSigners[i].csCounterSigners)
                        FIXME("unimplemented for counter signers\n");
                    signers[i].cCounterSigner = 0;
                    signers[i].rgpCounterSigner = NULL;
                }
            }
            else
                err = ERROR_OUTOFMEMORY;
        }
        if (err == NO_ERROR)
            err = policyCallback(data, TRUSTERROR_STEP_FINAL_POLICYPROV,
             data->dwRegPolicySettings, data->csSigners, signers, policyArg);
        data->psPfns->pfnFree(signers);
    }
    if (err != NO_ERROR)
        data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_POLICYPROV] = err;
    TRACE("returning %ld (%08lx)\n", err == NO_ERROR ? S_OK : S_FALSE,
     data->padwTrustStepErrors[TRUSTERROR_STEP_FINAL_POLICYPROV]);
    return err == NO_ERROR ? S_OK : S_FALSE;
}

HRESULT WINAPI SoftpubCleanup(CRYPT_PROVIDER_DATA *data)
{
    DWORD i, j;

    for (i = 0; i < data->csSigners; i++)
    {
        for (j = 0; j < data->pasSigners[i].csCertChain; j++)
            CertFreeCertificateContext(data->pasSigners[i].pasCertChain[j].pCert);
        data->psPfns->pfnFree(data->pasSigners[i].pasCertChain);
        data->psPfns->pfnFree(data->pasSigners[i].psSigner);
        CertFreeCertificateChain(data->pasSigners[i].pChainContext);
    }
    data->psPfns->pfnFree(data->pasSigners);

    for (i = 0; i < data->chStores; i++)
        CertCloseStore(data->pahStores[i], 0);
    data->psPfns->pfnFree(data->pahStores);

    if (data->pPDSip)
    {
        data->psPfns->pfnFree(data->pPDSip->pSip);
        data->psPfns->pfnFree(data->pPDSip->pCATSip);
        data->psPfns->pfnFree(data->pPDSip->psSipSubjectInfo);
        data->psPfns->pfnFree(data->pPDSip->psSipCATSubjectInfo);
        data->psPfns->pfnFree(data->pPDSip->psIndirectData);
    }

    if (WVT_ISINSTRUCT(CRYPT_PROVIDER_DATA, data->cbStruct, pSigState) && data->pSigState)
    {
        CryptMsgClose(data->pSigState->hPrimarySig);
        for (i = 0; i < data->pSigState->cSecondarySigs; ++i)
            CryptMsgClose(data->pSigState->rhSecondarySigs[i]);
        data->psPfns->pfnFree(data->pSigState);
    }
    CryptMsgClose(data->hMsg);

    if (data->fOpenedFile &&
     data->pWintrustData->dwUnionChoice == WTD_CHOICE_FILE &&
     data->pWintrustData->pFile)
    {
        CloseHandle(data->pWintrustData->pFile->hFile);
        data->pWintrustData->pFile->hFile = INVALID_HANDLE_VALUE;
        data->fOpenedFile = FALSE;
    }

    return S_OK;
}

HRESULT WINAPI HTTPSCertificateTrust(CRYPT_PROVIDER_DATA *data)
{
    FIXME("(%p)\n", data);
    return S_OK;
}

HRESULT WINAPI HTTPSFinalProv(CRYPT_PROVIDER_DATA *data)
{
    FIXME("(%p)\n", data);
    return S_OK;
}
