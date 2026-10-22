/*
 * SetupAPI DiskSpace functions
 *
 * Copyright 2004 CodeWeavers (Aric Stewart)
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

#include "setupapi_private.h"

struct file
{
    WCHAR *path;
    LONGLONG size;
    UINT op;
};

struct disk_space_list
{
    unsigned int flags;
    struct file *files;
    size_t count, capacity;
};

static BOOL array_reserve(void **elements, size_t *capacity, size_t count, size_t size)
{
    size_t new_capacity, max_capacity;
    void *new_elements;

    if (count <= *capacity)
        return TRUE;

    max_capacity = ~(size_t)0 / size;
    if (count > max_capacity)
        return FALSE;

    new_capacity = max(4, *capacity);
    while (new_capacity < count && new_capacity <= max_capacity / 2)
        new_capacity *= 2;
    if (new_capacity < count)
        new_capacity = max_capacity;

    if (!(new_elements = realloc(*elements, new_capacity * size)))
        return FALSE;

    *elements = new_elements;
    *capacity = new_capacity;
    return TRUE;
}

static BOOL ascii_isalpha(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/***********************************************************************
 *		SetupCreateDiskSpaceListW  (SETUPAPI.@)
 */
HDSKSPC WINAPI SetupCreateDiskSpaceListW(PVOID Reserved1, DWORD Reserved2, UINT Flags)
{
    struct disk_space_list *list;

    TRACE("(%p, %u, 0x%08x)\n", Reserved1, Reserved2, Flags);

    if (Reserved1 || Reserved2 || Flags & ~SPDSL_IGNORE_DISK)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    if (!(list = calloc(1, sizeof(*list))))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    list->flags = Flags;
    return list;
}


/***********************************************************************
 *		SetupCreateDiskSpaceListA  (SETUPAPI.@)
 */
HDSKSPC WINAPI SetupCreateDiskSpaceListA(PVOID Reserved1, DWORD Reserved2, UINT Flags)
{
    return SetupCreateDiskSpaceListW( Reserved1, Reserved2, Flags );
}

/***********************************************************************
 *		SetupDuplicateDiskSpaceListW  (SETUPAPI.@)
 */
HDSKSPC WINAPI SetupDuplicateDiskSpaceListW(HDSKSPC handle, PVOID Reserved1, DWORD Reserved2, UINT Flags)
{
    struct disk_space_list *copy, *list = handle;

    if (Reserved1 || Reserved2 || Flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    if (!handle)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }

    if (!(copy = malloc(sizeof(*copy))))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    copy->flags = list->flags;
    copy->count = list->count;
    copy->capacity = 0;
    copy->files = NULL;
    if (!array_reserve((void **)&copy->files, &copy->capacity, copy->count, sizeof(*copy->files)))
    {
        free(copy);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    for (size_t i = 0; i < list->count; ++i)
    {
        copy->files[i].path = wcsdup(list->files[i].path);
        copy->files[i].op = list->files[i].op;
        copy->files[i].size = list->files[i].size;
        if (!copy->files[i].path)
        {
            while (i--)
                free(copy->files[i].path);
            free(copy->files);
            free(copy);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
    }

    return copy;
}

/***********************************************************************
 *		SetupDuplicateDiskSpaceListA  (SETUPAPI.@)
 */
HDSKSPC WINAPI SetupDuplicateDiskSpaceListA(HDSKSPC DiskSpace, PVOID Reserved1, DWORD Reserved2, UINT Flags)
{
    return SetupDuplicateDiskSpaceListW(DiskSpace, Reserved1, Reserved2, Flags);
}

static BOOL query_drives(HDSKSPC handle, void *buffer, BOOL wide, DWORD size, DWORD *ret_size)
{
    struct disk_space_list *list = handle;
    BOOL used[26] = {FALSE};
    DWORD count = 0, fit, pos = 0;

    if (!handle)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    for (size_t i = 0; i < list->count; ++i)
    {
        WCHAR letter = towlower(list->files[i].path[0]);

        if (!used[letter - 'a'])
        {
            used[letter - 'a'] = TRUE;
            ++count;
        }
    }

    /* Each drive takes three characters ("c:" and a terminator); one more ends the list. */
    if (!buffer)
    {
        if (ret_size)
            *ret_size = count * 3 + 1;
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    fit = min(count, size / 3);

    for (unsigned int letter = 0; letter < 26 && pos < fit * 3; ++letter)
    {
        if (!used[letter])
            continue;
        if (wide)
        {
            ((WCHAR *)buffer)[pos++] = 'a' + letter;
            ((WCHAR *)buffer)[pos++] = ':';
            ((WCHAR *)buffer)[pos++] = 0;
        }
        else
        {
            ((char *)buffer)[pos++] = 'a' + letter;
            ((char *)buffer)[pos++] = ':';
            ((char *)buffer)[pos++] = 0;
        }
    }

    if (count * 3 + 1 > size)
    {
        if (ret_size)
            *ret_size = (fit < count ? fit + 1 : fit) * 3 + 1;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    if (wide)
        ((WCHAR *)buffer)[pos] = 0;
    else
        ((char *)buffer)[pos] = 0;
    if (ret_size)
        *ret_size = count * 3 + 1;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

/***********************************************************************
 *		SetupQueryDrivesInDiskSpaceListW  (SETUPAPI.@)
 */
BOOL WINAPI SetupQueryDrivesInDiskSpaceListW(HDSKSPC handle, WCHAR *buffer, DWORD size, DWORD *ret_size)
{
    TRACE("handle %p, buffer %p, size %u, ret_size %p.\n", handle, buffer, size, ret_size);

    return query_drives(handle, buffer, TRUE, size, ret_size);
}

/***********************************************************************
 *		SetupQueryDrivesInDiskSpaceListA  (SETUPAPI.@)
 */
BOOL WINAPI SetupQueryDrivesInDiskSpaceListA(HDSKSPC handle, char *buffer, DWORD size, DWORD *ret_size)
{
    TRACE("handle %p, buffer %p, size %u, ret_size %p.\n", handle, buffer, size, ret_size);

    return query_drives(handle, buffer, FALSE, size, ret_size);
}

/***********************************************************************
*		SetupQuerySpaceRequiredOnDriveW  (SETUPAPI.@)
*/
BOOL WINAPI SetupQuerySpaceRequiredOnDriveW(HDSKSPC handle,
        const WCHAR *drive, LONGLONG *ret_size, void *reserved1, UINT reserved2)
{
    struct disk_space_list *list = handle;
    BOOL has_files = FALSE;
    LONGLONG size = 0;

    TRACE("handle %p, drive %s, ret_size %p, reserved1 %p, reserved2 %#x.\n",
            handle, debugstr_w(drive), ret_size, reserved1, reserved2);

    if (!handle)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (!drive)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!ascii_isalpha(drive[0]) || drive[1] != ':' || drive[2])
    {
        SetLastError(ERROR_INVALID_DRIVE);
        return FALSE;
    }

    for (size_t i = 0; i < list->count; ++i)
    {
        if (towlower(drive[0]) == towlower(list->files[i].path[0]))
        {
            has_files = TRUE;
            size += list->files[i].size;
        }
    }

    if (!has_files)
    {
        SetLastError(ERROR_INVALID_DRIVE);
        return FALSE;
    }

    *ret_size = size;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

/***********************************************************************
*		SetupQuerySpaceRequiredOnDriveA  (SETUPAPI.@)
*/
BOOL WINAPI SetupQuerySpaceRequiredOnDriveA(HDSKSPC DiskSpace,
                        LPCSTR DriveSpec, LONGLONG *SpaceRequired,
                        PVOID Reserved1, UINT Reserved2)
{
    DWORD len;
    LPWSTR DriveSpecW;
    BOOL ret;

    /* The parameter validation checks are in a different order from the
     * Unicode variant of SetupQuerySpaceRequiredOnDrive. */
    if (!DriveSpec)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!DiskSpace)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    len = MultiByteToWideChar(CP_ACP, 0, DriveSpec, -1, NULL, 0);
    if (!len)
        return FALSE;

    DriveSpecW = malloc(len * sizeof(WCHAR));
    if (!DriveSpecW)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    if (!MultiByteToWideChar(CP_ACP, 0, DriveSpec, -1, DriveSpecW, len))
    {
        free(DriveSpecW);
        return FALSE;
    }

    ret = SetupQuerySpaceRequiredOnDriveW(DiskSpace, DriveSpecW, SpaceRequired,
                                          Reserved1, Reserved2);

    free(DriveSpecW);

    return ret;
}

static BOOL get_source_size(HINF inf, const WCHAR *file, LONGLONG *size)
{
    WCHAR actual[MAX_PATH], text[32], *end;
    INFCONTEXT ctx;
    ULONGLONG value = 0;

    if ((!SetupDiGetActualSectionToInstallW(inf, L"SourceDisksFiles", actual, ARRAYSIZE(actual), NULL, NULL)
            || !SetupFindFirstLineW(inf, actual, file, &ctx))
            && !SetupFindFirstLineW(inf, L"SourceDisksFiles", file, &ctx))
    {
        SetLastError(ERROR_LINE_NOT_FOUND);
        return FALSE;
    }

    /* the size is a 32-bit value on Windows */
    if (SetupGetFieldCount(&ctx) >= 3)
    {
        if (!SetupGetStringFieldW(&ctx, 3, text, ARRAYSIZE(text), NULL))
            return FALSE;
        value = _wcstoui64(text, &end, 0);
        if (end == text)
            value = 0;
        else if (value > MAXDWORD)
            value = MAXDWORD;
    }

    *size = value;
    return TRUE;
}

static BOOL add_file_list(HDSKSPC handle, HINF inf, HINF layout, const WCHAR *dir_section,
        const WCHAR *name, const WCHAR *source, UINT op)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH * 2];
    LONGLONG size = 0;
    size_t len;

    if (op == FILEOP_COPY && !get_source_size(layout, source, &size))
        return FALSE;

    if (!SetupGetTargetPathW(inf, NULL, dir_section, dir, ARRAYSIZE(dir), NULL))
        return FALSE;

    len = wcslen(dir);
    if (len && dir[len - 1] == '\\')
        dir[--len] = 0;
    _snwprintf(path, ARRAYSIZE(path), L"%s\\%s", dir, name);
    return SetupAddToDiskSpaceListW(handle, path, size, op, NULL, 0);
}

static BOOL add_section(HDSKSPC handle, HINF inf, HINF layout, const WCHAR *section, UINT op)
{
    WCHAR name[MAX_PATH], source[MAX_PATH];
    INFCONTEXT line;
    BOOL more;

    if (SetupGetLineCountW(inf, section) < 0)
        return FALSE;

    for (more = SetupFindFirstLineW(inf, section, NULL, &line); more; more = SetupFindNextLine(&line, &line))
    {
        DWORD count = SetupGetFieldCount(&line);

        if (!count)
            continue;
        if (!SetupGetStringFieldW(&line, 1, name, ARRAYSIZE(name), NULL))
            return FALSE;
        if (!name[0])
            continue;
        source[0] = 0;
        if (count >= 2 && !SetupGetStringFieldW(&line, 2, source, ARRAYSIZE(source), NULL))
            return FALSE;
        if (!source[0])
            wcscpy(source, name);
        if (!add_file_list(handle, inf, layout, section, name, source, op))
            return FALSE;
    }
    return TRUE;
}

/***********************************************************************
 *		SetupAddSectionToDiskSpaceListW  (SETUPAPI.@)
 */
BOOL WINAPI SetupAddSectionToDiskSpaceListW(HDSKSPC handle, HINF inf, HINF layout,
        const WCHAR *section, UINT op, void *reserved1, UINT reserved2)
{
    TRACE("handle %p, inf %p, layout %p, section %s, op %#x, reserved1 %p, reserved2 %#x.\n",
            handle, inf, layout, debugstr_w(section), op, reserved1, reserved2);

    if (!handle)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (!section)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!layout || layout == INVALID_HANDLE_VALUE)
        layout = inf;

    if (!add_section(handle, inf, layout, section, op))
        return FALSE;

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

/***********************************************************************
 *		SetupAddSectionToDiskSpaceListA  (SETUPAPI.@)
 */
BOOL WINAPI SetupAddSectionToDiskSpaceListA(HDSKSPC handle, HINF inf, HINF layout,
        const char *section, UINT op, void *reserved1, UINT reserved2)
{
    WCHAR *sectionW = NULL;
    BOOL ret;

    if (section && !(sectionW = strdupAtoW(section)))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    ret = SetupAddSectionToDiskSpaceListW(handle, inf, layout, sectionW, op, reserved1, reserved2);
    HeapFree(GetProcessHeap(), 0, sectionW);
    return ret;
}

static BOOL add_copy_files(HDSKSPC handle, HINF inf, HINF layout, const WCHAR *section,
        const WCHAR *key, UINT op)
{
    INFCONTEXT line;
    BOOL more;

    for (more = SetupFindFirstLineW(inf, section, key, &line); more;
            more = SetupFindNextMatchLineW(&line, key, &line))
    {
        DWORD count = SetupGetFieldCount(&line);

        for (DWORD i = 1; i <= count; ++i)
        {
            WCHAR list[MAX_PATH];

            if (!SetupGetStringFieldW(&line, i, list, ARRAYSIZE(list), NULL))
                return FALSE;
            if (!list[0])
                continue;

            if (list[0] == '@')
            {
                if (!add_file_list(handle, inf, layout, L"DefaultDestDir", list + 1, list + 1, op))
                    return FALSE;
            }
            else if (SetupGetLineCountW(inf, list) >= 0 && !add_section(handle, inf, layout, list, op))
                return FALSE;
        }
    }
    return TRUE;
}

/***********************************************************************
 *		SetupAddInstallSectionToDiskSpaceListW  (SETUPAPI.@)
 */
BOOL WINAPI SetupAddInstallSectionToDiskSpaceListW(HDSKSPC handle, HINF inf, HINF layout,
        const WCHAR *section, void *reserved1, UINT reserved2)
{
    TRACE("handle %p, inf %p, layout %p, section %s, reserved1 %p, reserved2 %#x.\n",
            handle, inf, layout, debugstr_w(section), reserved1, reserved2);

    if (!handle)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (!section)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!inf || inf == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    if (!layout || layout == INVALID_HANDLE_VALUE)
        layout = inf;

    if (!add_copy_files(handle, inf, layout, section, L"CopyFiles", FILEOP_COPY)
            || !add_copy_files(handle, inf, layout, section, L"DelFiles", FILEOP_DELETE))
        return FALSE;

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

/***********************************************************************
 *		SetupAddInstallSectionToDiskSpaceListA  (SETUPAPI.@)
 */
BOOL WINAPI SetupAddInstallSectionToDiskSpaceListA(HDSKSPC handle, HINF inf, HINF layout,
        const char *section, void *reserved1, UINT reserved2)
{
    WCHAR *sectionW = NULL;
    BOOL ret;

    if (section && !(sectionW = strdupAtoW(section)))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    ret = SetupAddInstallSectionToDiskSpaceListW(handle, inf, layout, sectionW, reserved1, reserved2);
    HeapFree(GetProcessHeap(), 0, sectionW);
    return ret;
}

/***********************************************************************
*		SetupDestroyDiskSpaceList  (SETUPAPI.@)
*/
BOOL WINAPI SetupDestroyDiskSpaceList(HDSKSPC handle)
{
    struct disk_space_list *list = handle;

    if (!list)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    for (size_t i = 0; i < list->count; ++i)
        free(list->files[i].path);
    free(list->files);
    free(list);
    return TRUE;
}

static LONGLONG get_aligned_size(LONGLONG size)
{
    return (size + 4095) & ~4095;
}

/***********************************************************************
*		SetupAddToDiskSpaceListA  (SETUPAPI.@)
*/
BOOL WINAPI SetupAddToDiskSpaceListA(HDSKSPC handle, const char *file,
        LONGLONG size, UINT op, void *reserved1, UINT reserved2)
{
    WCHAR *fileW;
    DWORD len;
    BOOL ret;

    if (!file)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    len = MultiByteToWideChar(CP_ACP, 0, file, -1, NULL, 0);
    if (!len)
        return FALSE;
    if (!(fileW = malloc(len * sizeof(WCHAR))))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (!MultiByteToWideChar(CP_ACP, 0, file, -1, fileW, len))
    {
        free(fileW);
        return FALSE;
    }

    ret = SetupAddToDiskSpaceListW(handle, fileW, size, op, reserved1, reserved2);
    free(fileW);
    return ret;
}

/***********************************************************************
*		SetupAddToDiskSpaceListW  (SETUPAPI.@)
*/
BOOL WINAPI SetupAddToDiskSpaceListW(HDSKSPC handle, const WCHAR *file,
        LONGLONG size, UINT op, void *reserved1, UINT reserved2)
{
    struct disk_space_list *list = handle;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    WCHAR *full_path;
    DWORD len, size_full;

    TRACE("handle %p, file %s, size %I64d, op %#x, reserved1 %p, reserved2 %#x.\n",
            handle, debugstr_w(file), size, op, reserved1, reserved2);

    if (!handle)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    size = get_aligned_size(size);

    if (op != FILEOP_COPY && op != FILEOP_DELETE)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (op == FILEOP_DELETE)
        size = 0;

    if (!(len = GetFullPathNameW(file, 0, NULL, NULL)))
    {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    if (!(full_path = malloc(len * sizeof(WCHAR))))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    size_full = len;
    if (!(len = GetFullPathNameW(file, size_full, full_path, NULL)) || len >= size_full)
    {
        free(full_path);
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }

    if (!ascii_isalpha(full_path[0]) || full_path[1] != ':' || full_path[len - 1] == '\\')
    {
        free(full_path);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!(list->flags & SPDSL_IGNORE_DISK)
            && GetFileAttributesExW(full_path, GetFileExInfoStandard, &attr)
            && !(attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        size -= get_aligned_size(((LONGLONG)attr.nFileSizeHigh << 32) | attr.nFileSizeLow);

    for (size_t i = 0; i < list->count; ++i)
    {
        if (!wcscmp(full_path, list->files[i].path))
        {
            if (!(op == FILEOP_DELETE && list->files[i].op == FILEOP_COPY))
            {
                list->files[i].op = op;
                list->files[i].size = size;
            }
            free(full_path);
            SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
    }

    if (!array_reserve((void **)&list->files, &list->capacity, list->count + 1, sizeof(*list->files)))
    {
        free(full_path);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    list->files[list->count].path = full_path;
    list->files[list->count].op = op;
    list->files[list->count].size = size;
    ++list->count;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}
