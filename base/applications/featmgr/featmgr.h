/*
 * PROJECT:     ReactOS-Nitro Optional Features / Addon Manager (FEATMGMT-01)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared engine for the optional-feature catalog, state tracking,
 *              and reversible enable/disable (first slice: OS-features tier).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#ifndef _FEATMGR_H_
#define _FEATMGR_H_

#include <windows.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FM_MAX_ID    64
#define FM_MAX_NAME  128
#define FM_MAX_DESC  512
#define FM_MAX_VER   32
#define FM_MAX_DEPS  256

/* Two-tier model (see design doc): OS features vs additional software. */
typedef enum _FM_TIER
{
    FM_TIER_OS = 0,       /* optional OS component/subsystem/driver */
    FM_TIER_SOFTWARE = 1  /* installable app/package (RAPPS-successor tier) */
} FM_TIER;

/* Installed-state, DISM /Get-Features-like, persisted in the registry. */
typedef enum _FM_STATE
{
    FM_STATE_NOT_INSTALLED = 0, /* known in catalog, never enabled */
    FM_STATE_ENABLED = 1,       /* payload applied, reversible undo recorded */
    FM_STATE_DISABLED = 2       /* previously enabled, now reverted */
} FM_STATE;

typedef struct _FM_FEATURE
{
    WCHAR   Id[FM_MAX_ID];
    WCHAR   Name[FM_MAX_NAME];
    WCHAR   Description[FM_MAX_DESC];
    WCHAR   Version[FM_MAX_VER];
    WCHAR   Dependencies[FM_MAX_DEPS];
    WCHAR   ManifestPath[MAX_PATH];
    FM_TIER Tier;
    FM_STATE State;
    BOOL    PayloadAvailable;    /* every declared payload source present on disk */
} FM_FEATURE;

/* Catalog / layout helpers. featmgr root = %SystemRoot%\Featmgr */
BOOL   FmGetRootDir(LPWSTR buf, DWORD cch);
BOOL   FmGetCatalogDir(LPWSTR buf, DWORD cch);
BOOL   FmGetPayloadDir(LPCWSTR id, LPWSTR buf, DWORD cch);

/* Enumerate the whole catalog, merged with persisted state.
 * Returns count; caller frees *outArr with FmFree(). */
int    FmEnumFeatures(FM_FEATURE **outArr);
void   FmFree(FM_FEATURE *arr);

/* Look up one feature (catalog + state merged). */
BOOL   FmFindFeature(LPCWSTR id, FM_FEATURE *out);
FM_STATE FmGetState(LPCWSTR id);

/* Reversible operations. log may be NULL; progress is written there. */
HRESULT FmEnable(LPCWSTR id, FILE *log);
HRESULT FmDisable(LPCWSTR id, FILE *log);

/* Utility: tier/state to display string. */
LPCWSTR FmTierStr(FM_TIER t);
LPCWSTR FmStateStr(FM_STATE s);

#ifdef __cplusplus
}
#endif

#endif /* _FEATMGR_H_ */
