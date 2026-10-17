/*
 * Registry management
 *
 * Copyright (C) 1999 Alexandre Julliard
 * Copyright (C) 2017 Dmitry Timoshkov
 *
 * Based on misc/registry.c code
 * Copyright (C) 1996 Marcus Meissner
 * Copyright (C) 1998 Matthew Becker
 * Copyright (C) 1999 Sylvain St-Germain
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

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winerror.h>

LSTATUS WINAPI RegCreateKeyTransactedW( HKEY hkey, LPCWSTR name, DWORD reserved, LPWSTR class,
                                        DWORD options, REGSAM access, SECURITY_ATTRIBUTES *sa,
                                        PHKEY retkey, LPDWORD dispos, HANDLE transaction, PVOID reserved2 )
{
    return ERROR_CALL_NOT_IMPLEMENTED;
}


/******************************************************************************
 * RegCreateKeyTransactedA   [ADVAPI32.@]
 */
LSTATUS WINAPI RegCreateKeyTransactedA( HKEY hkey, LPCSTR name, DWORD reserved, LPSTR class,
                                        DWORD options, REGSAM access, SECURITY_ATTRIBUTES *sa,
                                        PHKEY retkey, LPDWORD dispos, HANDLE transaction, PVOID reserved2 )
{
    return ERROR_CALL_NOT_IMPLEMENTED;
}


