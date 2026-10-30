/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * PURPOSE:         Runtime code
 * FILE:            lib/rtl/version.c
 * PROGRAMERS:      Filip Navara
 *                  Hermes Belusca-Maito (hermes.belusca@sfr.fr)
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>
#include <reactos/verifyver.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

NTSTATUS
NTAPI
RtlGetVersion(OUT PRTL_OSVERSIONINFOW lpVersionInformation);

#define VER_CONDITION_MASK_TOUCHED 0x8000000000000000ULL

/* FUNCTIONS ****************************************************************/

/*
* @implemented
*/
NTSTATUS
NTAPI
RtlVerifyVersionInfo(IN PRTL_OSVERSIONINFOEXW VersionInfo,
                     IN ULONG TypeMask,
                     IN ULONGLONG ConditionMask)
{
    RTL_OSVERSIONINFOEXW Version;
    NTSTATUS Status;

    Version.dwOSVersionInfoSize = sizeof(Version);

    Status = RtlGetVersion((PRTL_OSVERSIONINFOW)&Version);
    if (Status != STATUS_SUCCESS)
    {
        return Status;
    }

    return RtlpVerifyVersionInfo(&Version, VersionInfo, TypeMask, ConditionMask);
}

/*
 * @implemented
 */
ULONGLONG
NTAPI
VerSetConditionMask(IN ULONGLONG ConditionMask,
                    IN ULONG TypeMask,
                    IN UCHAR Condition)
{
    ULONGLONG ullCondMask;

    /* Windows returns an empty mask when there is nothing to set */
    if (TypeMask == 0)
        return 0;

    Condition &= VER_CONDITION_MASK;

    ullCondMask = Condition;
    if (TypeMask & VER_PRODUCT_TYPE)
        ConditionMask |= ullCondMask << (7 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_SUITENAME)
        ConditionMask |= ullCondMask << (6 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_SERVICEPACKMAJOR)
        ConditionMask |= ullCondMask << (5 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_SERVICEPACKMINOR)
        ConditionMask |= ullCondMask << (4 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_PLATFORMID)
        ConditionMask |= ullCondMask << (3 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_BUILDNUMBER)
        ConditionMask |= ullCondMask << (2 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_MAJORVERSION)
        ConditionMask |= ullCondMask << (1 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_MINORVERSION)
        ConditionMask |= ullCondMask << (0 * VER_NUM_BITS_PER_CONDITION_MASK);

    /* Windows also flags every mask it has touched, even for an empty condition */
    return ConditionMask | VER_CONDITION_MASK_TOUCHED;
}

/* EOF */
