/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../../../win32ss/user/imm32/precomp.h"
/* These unused functions share names with user32's, but are separate DLL
 * locals in production. Keep the fixture's copies separate as well. */
#define ValidateHandle GuestImmValidateHandle
#define ValidateHandleNoErr GuestImmValidateHandleNoErr
#include "../../../../win32ss/user/imm32/utils.c"

ULONG_PTR GuestStateImmThread(void)
{
    return (ULONG_PTR)Imm32CurrentPti();
}

PVOID GuestStateImmClient(void)
{
    return GetWin32ClientInfo();
}
