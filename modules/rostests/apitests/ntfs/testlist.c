/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test list of the NTFS tests
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define STANDALONE
#include <apitest.h>

extern void func_NtfsAccess(void);
extern void func_NtfsBackup(void);
extern void func_NtfsCreate(void);
extern void func_NtfsDelete(void);
extern void func_NtfsDeleteRunning(void);
extern void func_NtfsDirRename(void);
extern void func_NtfsHardLink(void);
extern void func_NtfsInherit(void);
extern void func_NtfsOpenById(void);
extern void func_NtfsPagingFile(void);
extern void func_NtfsRename(void);
extern void func_NtfsSharing(void);
extern void func_NtfsSystemFiles(void);
extern void func_NtfsTraverse(void);

const struct test winetest_testlist[] =
{
    { "NtfsAccess", func_NtfsAccess },
    { "NtfsBackup", func_NtfsBackup },
    { "NtfsCreate", func_NtfsCreate },
    { "NtfsDelete", func_NtfsDelete },
    { "NtfsDeleteRunning", func_NtfsDeleteRunning },
    { "NtfsDirRename", func_NtfsDirRename },
    { "NtfsHardLink", func_NtfsHardLink },
    { "NtfsInherit", func_NtfsInherit },
    { "NtfsOpenById", func_NtfsOpenById },
    { "NtfsPagingFile", func_NtfsPagingFile },
    { "NtfsRename", func_NtfsRename },
    { "NtfsSharing", func_NtfsSharing },
    { "NtfsSystemFiles", func_NtfsSystemFiles },
    { "NtfsTraverse", func_NtfsTraverse },
    { 0, 0 }
};
