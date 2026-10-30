/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The comparison behind RtlVerifyVersionInfo, for any version to compare with
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * RtlVerifyVersionInfo compares with the version of the running system. The version lie shims answer the
 * same question for the version they pretend to be, and share this code so that both give the answers
 * of Windows.
 *
 * Needs the NT status codes and RTL_OSVERSIONINFOEXW (ntndk.h, or rtl.h).
 */

#ifndef _REACTOS_VERIFYVER_H
#define _REACTOS_VERIFYVER_H

/* The condition the mask holds for a field: the type bit says which one */
static inline UCHAR
RtlpVerGetCondition(IN ULONGLONG ConditionMask,
                    IN ULONG TypeMask)
{
    UCHAR Condition = 0;

    if (TypeMask & VER_PRODUCT_TYPE)
        Condition |= ConditionMask >> (7 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_SUITENAME)
        Condition |= ConditionMask >> (6 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_PLATFORMID)
        Condition |= ConditionMask >> (3 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_BUILDNUMBER)
        Condition |= ConditionMask >> (2 * VER_NUM_BITS_PER_CONDITION_MASK);
    /*
     * We choose here the lexicographical order on the 4D space
     * {(Major ; Minor ; SP Major ; SP Minor)} to select the
     * appropriate comparison operator.
     * Therefore the following 'else if' instructions must be in this order.
     */
    else if (TypeMask & VER_MAJORVERSION)
        Condition |= ConditionMask >> (1 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_MINORVERSION)
        Condition |= ConditionMask >> (0 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_SERVICEPACKMAJOR)
        Condition |= ConditionMask >> (5 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_SERVICEPACKMINOR)
        Condition |= ConditionMask >> (4 * VER_NUM_BITS_PER_CONDITION_MASK);

    Condition &= VER_CONDITION_MASK;

    return Condition;
}

/* Windows compares the version numbers as signed values */
static inline BOOLEAN
RtlpVerCompare(LONG left, LONG right, UCHAR Condition)
{
    switch (Condition)
    {
        case VER_EQUAL:
            return (left == right);
        case VER_GREATER:
            return (left > right);
        case VER_GREATER_EQUAL:
            return (left >= right);
        case VER_LESS:
            return (left < right);
        case VER_LESS_EQUAL:
            return (left <= right);
        default:
            break;
    }
    return FALSE;
}

/*
 * Does 'Version' satisfy what VersionInfo asks, as RtlVerifyVersionInfo answers it?
 * Returns STATUS_SUCCESS, STATUS_REVISION_MISMATCH or STATUS_INVALID_PARAMETER.
 */
static inline NTSTATUS
RtlpVerifyVersionInfo(IN const RTL_OSVERSIONINFOEXW* Version,
                      IN const RTL_OSVERSIONINFOEXW* VersionInfo,
                      IN ULONG TypeMask,
                      IN ULONGLONG ConditionMask)
{
    BOOLEAN Comparison;
    BOOLEAN DoNextCheck;
    UCHAR Condition;

    /* FIXME:
        - Check the following special case on Windows (various versions):
          o lp->wSuiteMask == 0 and ver.wSuiteMask != 0 and VER_AND/VER_OR
          o lp->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXW)
        - MSDN talks about some tests being impossible. Check what really happens.
     */

    /* An empty condition mask is valid: the missing conditions never match */
    if (!TypeMask)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (TypeMask & VER_PRODUCT_TYPE)
    {
        Comparison = RtlpVerCompare(Version->wProductType,
                                    VersionInfo->wProductType,
                                    RtlpVerGetCondition(ConditionMask, VER_PRODUCT_TYPE));
        if (!Comparison)
        {
            return STATUS_REVISION_MISMATCH;
        }
    }

    if (TypeMask & VER_SUITENAME)
    {
        switch (RtlpVerGetCondition(ConditionMask, VER_SUITENAME))
        {
            case VER_AND:
            {
                if ((VersionInfo->wSuiteMask & Version->wSuiteMask) != VersionInfo->wSuiteMask)
                {
                    return STATUS_REVISION_MISMATCH;
                }
            }
            break;

            case VER_OR:
            {
                if (!(VersionInfo->wSuiteMask & Version->wSuiteMask) && VersionInfo->wSuiteMask)
                {
                    return STATUS_REVISION_MISMATCH;
                }
                break;
            }

            default:
            {
                return STATUS_INVALID_PARAMETER;
            }
        }
    }

    if (TypeMask & VER_PLATFORMID)
    {
        Comparison = RtlpVerCompare(Version->dwPlatformId,
                                    VersionInfo->dwPlatformId,
                                    RtlpVerGetCondition(ConditionMask, VER_PLATFORMID));
        if (!Comparison)
        {
            return STATUS_REVISION_MISMATCH;
        }
    }

    if (TypeMask & VER_BUILDNUMBER)
    {
        Comparison = RtlpVerCompare(Version->dwBuildNumber,
                                    VersionInfo->dwBuildNumber,
                                    RtlpVerGetCondition(ConditionMask, VER_BUILDNUMBER));
        if (!Comparison)
        {
            return STATUS_REVISION_MISMATCH;
        }
    }

    DoNextCheck = TRUE;
    Condition = VER_EQUAL;

    if (TypeMask & VER_MAJORVERSION)
    {
        Condition = RtlpVerGetCondition(ConditionMask, VER_MAJORVERSION);
        DoNextCheck = (VersionInfo->dwMajorVersion == Version->dwMajorVersion);
        Comparison = RtlpVerCompare(Version->dwMajorVersion,
                                    VersionInfo->dwMajorVersion,
                                    Condition);

        if (!Comparison && !DoNextCheck)
        {
            return STATUS_REVISION_MISMATCH;
        }
    }

    if (DoNextCheck)
    {
        if (TypeMask & VER_MINORVERSION)
        {
            if (Condition == VER_EQUAL)
            {
                Condition = RtlpVerGetCondition(ConditionMask, VER_MINORVERSION);
            }

            DoNextCheck = (VersionInfo->dwMinorVersion == Version->dwMinorVersion);
            Comparison = RtlpVerCompare(Version->dwMinorVersion,
                                        VersionInfo->dwMinorVersion,
                                        Condition);

            if (!Comparison && !DoNextCheck)
            {
                return STATUS_REVISION_MISMATCH;
            }
        }

        if (DoNextCheck && (TypeMask & VER_SERVICEPACKMAJOR))
        {
            if (Condition == VER_EQUAL)
            {
                Condition = RtlpVerGetCondition(ConditionMask, VER_SERVICEPACKMAJOR);
            }

            DoNextCheck = (VersionInfo->wServicePackMajor == Version->wServicePackMajor);
            Comparison = RtlpVerCompare(Version->wServicePackMajor,
                                        VersionInfo->wServicePackMajor,
                                        Condition);

            if (!Comparison && !DoNextCheck)
            {
                return STATUS_REVISION_MISMATCH;
            }
        }

        /* The service pack minor version is checked on its own, whether or
         * not the service pack major version is part of the type mask. */
        if (DoNextCheck && (TypeMask & VER_SERVICEPACKMINOR))
        {
            if (Condition == VER_EQUAL)
            {
                Condition = RtlpVerGetCondition(ConditionMask, VER_SERVICEPACKMINOR);
            }

            Comparison = RtlpVerCompare(Version->wServicePackMinor,
                                        VersionInfo->wServicePackMinor,
                                        Condition);

            if (!Comparison)
            {
                return STATUS_REVISION_MISMATCH;
            }
        }
    }

    return STATUS_SUCCESS;
}

#endif /* _REACTOS_VERIFYVER_H */
