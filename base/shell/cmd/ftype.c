/*
 * PROJECT:     ReactOS Command Shell
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ftype internal command: show or change the open command of a file type
 */

#include "precomp.h"

#ifdef INCLUDE_CMD_FTYPE

#define CLASSES_KEY     _T("SOFTWARE\\Classes")
#define OPEN_COMMAND    _T("\\shell\\open\\command")

/*
 * Reads the default value of <FileType>\shell\open\command.
 * Returns ERROR_FILE_NOT_FOUND when the command key does not exist, and
 * ERROR_SUCCESS with *ppszCommand NULL when the key exists but holds no command.
 */
static LONG
QueryOpenCommand(
    IN HKEY hKeyClasses,
    IN PCTSTR pszFileType,
    OUT PTSTR *ppszCommand)
{
    LONG lRet;
    HKEY hKey;
    PTSTR pszPath;
    DWORD dwType, dwSize = 0;
    PTSTR pszCommand;

    *ppszCommand = NULL;

    pszPath = cmd_alloc((_tcslen(pszFileType) + _tcslen(OPEN_COMMAND) + 1) * sizeof(TCHAR));
    if (!pszPath)
        return ERROR_NOT_ENOUGH_MEMORY;
    _tcscpy(pszPath, pszFileType);
    _tcscat(pszPath, OPEN_COMMAND);

    lRet = RegOpenKeyEx(hKeyClasses, pszPath, 0, KEY_QUERY_VALUE, &hKey);
    cmd_free(pszPath);
    if (lRet != ERROR_SUCCESS)
        return lRet;

    lRet = RegQueryValueEx(hKey, NULL, NULL, &dwType, NULL, &dwSize);
    if (lRet != ERROR_SUCCESS || (dwType != REG_SZ && dwType != REG_EXPAND_SZ) || dwSize < sizeof(TCHAR))
    {
        RegCloseKey(hKey);
        return (lRet == ERROR_FILE_NOT_FOUND || lRet == ERROR_SUCCESS) ? ERROR_SUCCESS : lRet;
    }

    pszCommand = cmd_alloc(dwSize + sizeof(TCHAR));
    if (!pszCommand)
    {
        RegCloseKey(hKey);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    lRet = RegQueryValueEx(hKey, NULL, NULL, &dwType, (LPBYTE)pszCommand, &dwSize);
    RegCloseKey(hKey);
    if (lRet != ERROR_SUCCESS)
    {
        cmd_free(pszCommand);
        return lRet;
    }
    pszCommand[dwSize / sizeof(TCHAR)] = 0;

    if (!*pszCommand)
    {
        cmd_free(pszCommand);
        return ERROR_SUCCESS;
    }

    *ppszCommand = pszCommand;
    return ERROR_SUCCESS;
}

/* The command text can be longer than the buffer of the printf functions, so it is written as is */
static VOID
PrintTypeAndCommand(
    IN PCTSTR pszFileType,
    IN PCTSTR pszCommand)
{
    ConOutPuts(pszFileType);
    ConOutPuts(_T("="));
    ConOutPuts(pszCommand);
    ConOutPuts(_T("\n"));
}

/* Prints "type=command"; a type without an open command gets the error text instead */
static LONG
PrintFileType(
    IN PCTSTR pszFileType)
{
    LONG lRet;
    HKEY hKeyClasses;
    PTSTR pszCommand;

    lRet = RegOpenKeyEx(HKEY_LOCAL_MACHINE, CLASSES_KEY, 0, KEY_QUERY_VALUE, &hKeyClasses);
    if (lRet != ERROR_SUCCESS)
    {
        ErrorMessage(lRet, NULL);
        return lRet;
    }

    lRet = QueryOpenCommand(hKeyClasses, pszFileType, &pszCommand);
    RegCloseKey(hKeyClasses);

    if (lRet == ERROR_FILE_NOT_FOUND || lRet == ERROR_PATH_NOT_FOUND)
    {
        ConErrResPrintf(STRING_FTYPE_ERROR, pszFileType);
        return 2;
    }
    if (lRet != ERROR_SUCCESS)
    {
        ErrorMessage(lRet, NULL);
        return lRet;
    }

    if (!pszCommand)
    {
        ConErrResPrintf(STRING_FTYPE_ERROR, pszFileType);
        return ERROR_SUCCESS;
    }

    PrintTypeAndCommand(pszFileType, pszCommand);
    cmd_free(pszCommand);
    return ERROR_SUCCESS;
}

static LONG
PrintAllFileTypes(VOID)
{
    LONG lRet;
    HKEY hKeyClasses;
    DWORD dwKeyCtr, dwNumKeys = 0, dwNameLen = 0;
    PTSTR pszName;

    lRet = RegOpenKeyEx(HKEY_LOCAL_MACHINE, CLASSES_KEY, 0,
                        KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &hKeyClasses);
    if (lRet != ERROR_SUCCESS)
    {
        ErrorMessage(lRet, NULL);
        return lRet;
    }

    lRet = RegQueryInfoKey(hKeyClasses, NULL, NULL, NULL, &dwNumKeys, &dwNameLen,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    if (lRet != ERROR_SUCCESS)
    {
        ErrorMessage(lRet, NULL);
        RegCloseKey(hKeyClasses);
        return lRet;
    }

    ++dwNameLen;
    pszName = cmd_alloc(dwNameLen * sizeof(TCHAR));
    if (!pszName)
    {
        RegCloseKey(hKeyClasses);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    for (dwKeyCtr = 0; dwKeyCtr < dwNumKeys; ++dwKeyCtr)
    {
        DWORD dwBufSize = dwNameLen;
        PTSTR pszCommand;

        lRet = RegEnumKeyEx(hKeyClasses, dwKeyCtr, pszName, &dwBufSize, NULL, NULL, NULL, NULL);
        if (lRet != ERROR_SUCCESS)
        {
            if (lRet != ERROR_NO_MORE_ITEMS)
                ErrorMessage(lRet, NULL);
            break;
        }

        if (QueryOpenCommand(hKeyClasses, pszName, &pszCommand) == ERROR_SUCCESS && pszCommand)
        {
            PrintTypeAndCommand(pszName, pszCommand);
            cmd_free(pszCommand);
        }
    }

    cmd_free(pszName);
    RegCloseKey(hKeyClasses);
    return (lRet == ERROR_NO_MORE_ITEMS) ? ERROR_SUCCESS : lRet;
}

/* Creates <FileType>\shell\open\command if needed and stores the command as its default value */
static LONG
SetOpenCommand(
    IN PCTSTR pszFileType,
    IN PCTSTR pszCommand)
{
    LONG lRet;
    HKEY hKeyClasses, hKey;
    PTSTR pszPath;

    lRet = RegOpenKeyEx(HKEY_LOCAL_MACHINE, CLASSES_KEY, 0, KEY_CREATE_SUB_KEY, &hKeyClasses);
    if (lRet != ERROR_SUCCESS)
    {
        ErrorMessage(lRet, NULL);
        return lRet;
    }

    pszPath = cmd_alloc((_tcslen(pszFileType) + _tcslen(OPEN_COMMAND) + 1) * sizeof(TCHAR));
    if (!pszPath)
    {
        RegCloseKey(hKeyClasses);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    _tcscpy(pszPath, pszFileType);
    _tcscat(pszPath, OPEN_COMMAND);

    lRet = RegCreateKeyEx(hKeyClasses, pszPath, 0, NULL, REG_OPTION_NON_VOLATILE,
                          KEY_SET_VALUE, NULL, &hKey, NULL);
    cmd_free(pszPath);
    RegCloseKey(hKeyClasses);
    if (lRet != ERROR_SUCCESS)
    {
        ErrorMessage(lRet, NULL);
        return lRet;
    }

    lRet = RegSetValueEx(hKey, NULL, 0, REG_EXPAND_SZ,
                         (LPBYTE)pszCommand, (DWORD)(_tcslen(pszCommand) + 1) * sizeof(TCHAR));
    RegCloseKey(hKey);
    if (lRet != ERROR_SUCCESS)
        ErrorMessage(lRet, NULL);
    return lRet;
}

/* Removes the open command value only, the keys stay */
static LONG
RemoveOpenCommand(
    IN PCTSTR pszFileType)
{
    LONG lRet;
    HKEY hKeyClasses, hKey;
    PTSTR pszPath;

    lRet = RegOpenKeyEx(HKEY_LOCAL_MACHINE, CLASSES_KEY, 0, KEY_QUERY_VALUE, &hKeyClasses);
    if (lRet != ERROR_SUCCESS)
    {
        ErrorMessage(lRet, NULL);
        return lRet;
    }

    pszPath = cmd_alloc((_tcslen(pszFileType) + _tcslen(OPEN_COMMAND) + 1) * sizeof(TCHAR));
    if (!pszPath)
    {
        RegCloseKey(hKeyClasses);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    _tcscpy(pszPath, pszFileType);
    _tcscat(pszPath, OPEN_COMMAND);

    lRet = RegOpenKeyEx(hKeyClasses, pszPath, 0, KEY_SET_VALUE, &hKey);
    cmd_free(pszPath);
    RegCloseKey(hKeyClasses);
    if (lRet == ERROR_FILE_NOT_FOUND || lRet == ERROR_PATH_NOT_FOUND)
    {
        ConErrResPrintf(STRING_FTYPE_ERROR, pszFileType);
        return 2;
    }
    if (lRet != ERROR_SUCCESS)
    {
        ErrorMessage(lRet, NULL);
        return lRet;
    }

    lRet = RegDeleteValue(hKey, NULL);
    RegCloseKey(hKey);
    if (lRet == ERROR_FILE_NOT_FOUND)
        return ERROR_SUCCESS;
    if (lRet != ERROR_SUCCESS)
        ErrorMessage(lRet, NULL);
    return lRet;
}

INT CommandFtype(LPTSTR param)
{
    INT retval = 0;
    PTCHAR pEqualSign;

    if (!_tcsncmp(param, _T("/?"), 2))
    {
        ConOutResPaging(TRUE, STRING_FTYPE_HELP);
        return 0;
    }

    if (!*param)
    {
        retval = PrintAllFileTypes();
        goto Quit;
    }

    pEqualSign = _tcschr(param, _T('='));
    if (pEqualSign)
    {
        PTSTR pszCommand = pEqualSign + 1;

        *pEqualSign = 0;

        /* A registry key name has at most 255 characters; this also keeps the name inside the printf buffers */
        if (!*param || _tcslen(param) > 255 || _tcschr(pszCommand, _T('=')))
        {
            ConErrResPuts(STRING_SYNTAX_COMMAND_INCORRECT);
        }
        else if (!*pszCommand)
        {
            retval = RemoveOpenCommand(param);
        }
        else
        {
            retval = SetOpenCommand(param, pszCommand);
            if (retval == ERROR_SUCCESS)
                PrintFileType(param);
        }
    }
    else
    {
        /* Windows ignores the last word when the text has several: "ftype a b" asks for a */
        PTCHAR pLastSpace = _tcsrchr(param, _T(' '));
        if (pLastSpace)
            *pLastSpace = 0;
        if (_tcslen(param) > 255)
            ConErrResPuts(STRING_SYNTAX_COMMAND_INCORRECT);
        else
            retval = PrintFileType(param);
    }

Quit:
    if (BatType != CMD_TYPE)
    {
        if (retval != 0)
            nErrorLevel = retval;
    }
    else
    {
        nErrorLevel = retval;
    }

    return retval;
}

#endif /* INCLUDE_CMD_FTYPE */
