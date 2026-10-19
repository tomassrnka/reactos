/*
 * Volume management functions
 *
 * Copyright 1993 Erik Bos
 * Copyright 1996, 2004 Alexandre Julliard
 * Copyright 1999 Petr Tomasek
 * Copyright 2000 Andreas Mohr
 * Copyright 2003 Eric Pouech
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

/* Adapted from Wine kernelbase/volume.c. */
#include <k32.h>

BOOL WINAPI GetVolumeInformationByHandleW( HANDLE handle, WCHAR *label, DWORD label_len,
                                           DWORD *serial, DWORD *filename_len, DWORD *flags,
                                           WCHAR *fsname, DWORD fsname_len )
{
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    if (label || serial)
    {
        char buffer[sizeof(FILE_FS_VOLUME_INFORMATION) + MAX_PATH * sizeof(WCHAR)];
        FILE_FS_VOLUME_INFORMATION *info = (FILE_FS_VOLUME_INFORMATION *)buffer;
        ULONG size = label ? sizeof(*info) + min(label_len, MAX_PATH) * sizeof(WCHAR) : sizeof(buffer);

        status = NtQueryVolumeInformationFile( handle, &io, info, size, FileFsVolumeInformation );
        if (!NT_SUCCESS(status))
        {
            BaseSetLastNTError(status);
            return FALSE;
        }

        if (label)
        {
            if (label_len < info->VolumeLabelLength / sizeof(WCHAR) + 1)
            {
                SetLastError( ERROR_BAD_LENGTH );
                return FALSE;
            }
            memcpy( label, info->VolumeLabel, info->VolumeLabelLength );
            label[info->VolumeLabelLength / sizeof(WCHAR)] = 0;
        }
        if (serial) *serial = info->VolumeSerialNumber;
    }

    if (filename_len || flags || fsname)
    {
        char buffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + MAX_PATH * sizeof(WCHAR)];
        FILE_FS_ATTRIBUTE_INFORMATION *info = (FILE_FS_ATTRIBUTE_INFORMATION *)buffer;
        ULONG size = fsname ? sizeof(*info) + min(fsname_len, MAX_PATH) * sizeof(WCHAR) : sizeof(buffer);

        status = NtQueryVolumeInformationFile( handle, &io, info, size, FileFsAttributeInformation );
        if (!NT_SUCCESS(status))
        {
            BaseSetLastNTError(status);
            return FALSE;
        }

        if (fsname)
        {
            if (fsname_len < info->FileSystemNameLength / sizeof(WCHAR) + 1)
            {
                SetLastError( ERROR_BAD_LENGTH );
                return FALSE;
            }
            memcpy( fsname, info->FileSystemName, info->FileSystemNameLength );
            fsname[info->FileSystemNameLength / sizeof(WCHAR)] = 0;
        }
        if (filename_len) *filename_len = info->MaximumComponentNameLength;
        if (flags) *flags = info->FileSystemAttributes;
    }

    return TRUE;
}
