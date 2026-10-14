/*
 * File names and files of the browser's downloads
 *
 * Copyright 2026 ReactOS contributors
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

#ifndef _DOWNLOAD_NAME_H_
#define _DOWNLOAD_NAME_H_

/* The server chooses the name, so every name goes through dl_make_leaf, then dl_fit_leaf for the folder it is meant for,
   then dl_unmask_device (dl_create_unique_file does the last two itself). */

/* characters of one file name, as the file system allows */
#define DL_LEAF_MAX 255

/* snprintf that always ends the buffer; returns the length stored */
int dl_snprintf(WCHAR *buf, size_t cap, const WCHAR *fmt, ...);

/* value of the header "name" in a block of response headers; FALSE when there is none or it does not fit */
BOOL dl_find_header(const WCHAR *headers, const WCHAR *name, WCHAR *value, size_t cap);

/* the file name of a Content-Disposition value; FALSE when it names none (the result is not yet safe) */
BOOL dl_name_from_disposition(const WCHAR *value, WCHAR *name, size_t cap);

/* the host (without user name and password) and the last segment of the path of an address, decoded; either may be empty */
void dl_split_url(const WCHAR *url, WCHAR *host, size_t host_cap, WCHAR *name, size_t name_cap);

/* makes a name usable as one file name: no directory part, no stream, no trailing dot or space, never empty, at most
   DL_LEAF_MAX characters; name holds DL_LEAF_MAX + 1 characters */
void dl_make_leaf(WCHAR *name);

/* shortens name (the base, not the extension) so that dir (with its closing backslash), name and reserve more
   characters fit MAX_PATH; FALSE when the folder leaves no room */
BOOL dl_fit_leaf(const WCHAR *dir, WCHAR *name, size_t reserve);

/* a name that would open a DOS device in dir, or in a folder of a local drive (the user can choose another folder), gets
   '_' as the last character of its base name (NUL.txt becomes NU_.txt) */
void dl_unmask_device(const WCHAR *dir, WCHAR *name);

/* the path opens a DOS device (CreateFile would not create a file) */
BOOL dl_path_opens_device(const WCHAR *path);

/* the temporary folder with its closing backslash; FALSE when it is unknown or too long */
BOOL dl_get_temp_dir(WCHAR *dir);

/* creates a new file in dir that was not there: leaf, else "base (1).ext" up to (999); path receives MAX_PATH characters */
HANDLE dl_create_unique_file(const WCHAR *dir, const WCHAR *leaf, WCHAR *path);

BOOL dl_is_executable_ext(const WCHAR *ext);

#endif
