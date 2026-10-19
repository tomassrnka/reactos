/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/file/mntpointansi.c
 * PURPOSE:         Volume mount point functions (ANSI)
 */

/* INCLUDES *****************************************************************/

#include <k32.h>

/* FUNCTIONS ****************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
GetVolumeNameForVolumeMountPointA(IN LPCSTR lpszVolumeMountPoint,
                                  IN LPSTR lpszVolumeName,
                                  IN DWORD cchBufferLength)
{
    BOOL Ret;
    ANSI_STRING VolumeName;
    UNICODE_STRING VolumeNameU;
    PUNICODE_STRING VolumeMountPointU;

    /* Convert mount point to unicode */
    VolumeMountPointU = Basep8BitStringToStaticUnicodeString(lpszVolumeMountPoint);
    if (VolumeMountPointU == NULL)
    {
        return FALSE;
    }

    /* Initialize the strings we'll use for convention */
    VolumeName.Buffer = lpszVolumeName;
    VolumeName.Length = 0;
    VolumeName.MaximumLength = cchBufferLength - 1;

    VolumeNameU.Length = 0;
    VolumeNameU.MaximumLength = (cchBufferLength - 1) * sizeof(WCHAR) + sizeof(UNICODE_NULL);
    /* Allocate a buffer big enough to contain the returned name */
    VolumeNameU.Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, VolumeNameU.MaximumLength);
    if (VolumeNameU.Buffer == NULL)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    /* Query -W */
    Ret = GetVolumeNameForVolumeMountPointW(VolumeMountPointU->Buffer, VolumeNameU.Buffer, cchBufferLength);
    /* If it succeed, perform -A conversion */
    if (Ret)
    {
        NTSTATUS Status;

        /* Reinit our string for length */
        RtlInitUnicodeString(&VolumeNameU, VolumeNameU.Buffer);
        /* Convert to ANSI */
        Status = RtlUnicodeStringToAnsiString(&VolumeName, &VolumeNameU, FALSE);
        /* If conversion failed, force failure, otherwise, just null terminate */
        if (!NT_SUCCESS(Status))
        {
            Ret = FALSE;
            BaseSetLastNTError(Status);
        }
        else
        {
            VolumeName.Buffer[VolumeName.Length] = ANSI_NULL;
        }
    }

    /* Internal buffer no longer needed */
    RtlFreeHeap(RtlGetProcessHeap(), 0, VolumeNameU.Buffer);

    return Ret;
}

/* EOF */

/*
 * @implemented
 */
BOOL
WINAPI
SetVolumeMountPointA(IN LPCSTR lpszVolumeMountPoint,
                     IN LPCSTR lpszVolumeName)
{
    BOOL Ret = FALSE;
    UNICODE_STRING MountPointU, VolumeNameU;

    if (lpszVolumeMountPoint == NULL || lpszVolumeName == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* the conversion follows SetFileApisToOEM, as in the other file functions */
    if (!Basep8BitStringToDynamicUnicodeString(&MountPointU, lpszVolumeMountPoint))
        return FALSE;
    if (Basep8BitStringToDynamicUnicodeString(&VolumeNameU, lpszVolumeName))
    {
        Ret = SetVolumeMountPointW(MountPointU.Buffer, VolumeNameU.Buffer);
        RtlFreeUnicodeString(&VolumeNameU);
    }
    RtlFreeUnicodeString(&MountPointU);
    return Ret;
}

/*
 * @implemented
 */
BOOL
WINAPI
DeleteVolumeMountPointA(IN LPCSTR lpszVolumeMountPoint)
{
    BOOL Ret;
    UNICODE_STRING MountPointU;

    /* the ANSI form names the missing path an invalid name, the wide form an invalid parameter */
    if (lpszVolumeMountPoint == NULL)
    {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }

    if (!Basep8BitStringToDynamicUnicodeString(&MountPointU, lpszVolumeMountPoint))
        return FALSE;
    Ret = DeleteVolumeMountPointW(MountPointU.Buffer);
    RtlFreeUnicodeString(&MountPointU);
    return Ret;
}
