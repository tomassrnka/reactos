/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test list for comdlg32
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define STANDALONE
#include <apitest.h>

extern void func_PrintDlg(void);
extern void func_SaveDialogDefExt(void);
extern void func_SaveDialogFolder(void);
extern void func_SaveDialogLongName(void);

const struct test winetest_testlist[] =
{
    { "PrintDlg", func_PrintDlg },
    { "SaveDialogDefExt", func_SaveDialogDefExt },
    { "SaveDialogFolder", func_SaveDialogFolder },
    { "SaveDialogLongName", func_SaveDialogLongName },
    { 0, 0 }
};
