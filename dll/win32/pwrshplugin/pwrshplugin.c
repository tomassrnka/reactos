/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     PowerShell WSMan plugin that declines to serve remoting requests
 * COPYRIGHT:   Copyright 2026 reactos-nitro contributors
 *
 * Clean-room description.
 *
 * The PowerShell 7 installer (and WMF-aware installers in general) look for a file named
 * %SystemRoot%\System32\pwrshplugin.dll with a minimum version: it is the WSMan plugin that
 * Windows ships for PowerShell remoting. This DLL occupies that file name and exports the
 * entry points of the WSMan plugin API (the WSManPlugin* functions documented in wsman.h),
 * so a loader finds every name it expects.
 *
 * It does NOT provide remoting. WSMan loads a plugin by calling WSManPluginStartup first and
 * abandons the plugin when that fails, so Startup returns ERROR_NOT_SUPPORTED and every
 * other entry point is inert: nothing is allocated, no context is ever handed out, no request
 * is accepted. No WSMan plugin configuration and no PowerShellEngine registry key is written
 * for it anywhere; Enable-PSRemoting keeps failing for lack of a WSMan service.
 *
 * GetCLRVersionForPSVersion and PerformWSManPluginReportCompletion are exported by the
 * Windows DLL but are not part of any documented API; they are exported here (without
 * parameters, E_NOTIMPL / ERROR_NOT_SUPPORTED) only so the export table matches by name.
 */

#include <windef.h>
#include <winbase.h>
#include <winerror.h>

BOOL
WINAPI
DllMain(HINSTANCE hinstDll, DWORD dwReason, LPVOID reserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hinstDll);
    return TRUE;
}

DWORD
WINAPI
WSManPluginStartup(DWORD flags, PCWSTR applicationIdentification, PCWSTR extraInfo, PVOID *pluginContext)
{
    if (pluginContext)
        *pluginContext = NULL;
    return ERROR_NOT_SUPPORTED;
}

DWORD
WINAPI
WSManPluginShutdown(PVOID pluginContext, DWORD flags, DWORD reason)
{
    /* Startup never succeeds, so there is nothing to shut down. */
    return ERROR_NOT_SUPPORTED;
}

void
WINAPI
WSManPluginShell(PVOID pluginContext, PVOID requestDetails, DWORD flags, PCWSTR extraInfo,
                 PVOID startupInfo, PVOID inboundShellInformation)
{
}

void
WINAPI
WSManPluginCommand(PVOID pluginContext, PVOID requestDetails, DWORD flags, PVOID shellContext,
                   PCWSTR commandLine, PVOID arguments)
{
}

void
WINAPI
WSManPluginSend(PVOID pluginContext, PVOID requestDetails, DWORD flags, PVOID shellContext,
                PVOID commandContext, PCWSTR stream, PVOID inboundData)
{
}

void
WINAPI
WSManPluginReceive(PVOID pluginContext, PVOID requestDetails, DWORD flags, PVOID shellContext,
                   PVOID commandContext, PVOID streamSet)
{
}

void
WINAPI
WSManPluginSignal(PVOID pluginContext, PVOID requestDetails, DWORD flags, PVOID shellContext,
                  PVOID commandContext, PCWSTR code)
{
}

void
WINAPI
WSManPluginConnect(PVOID pluginContext, PVOID requestDetails, DWORD flags, PVOID shellContext,
                   PVOID commandContext, PVOID inboundConnectInformation)
{
}

void
WINAPI
WSManPluginReleaseShellContext(PVOID shellContext)
{
}

void
WINAPI
WSManPluginReleaseCommandContext(PVOID shellContext, PVOID commandContext)
{
}

HRESULT
WINAPI
GetCLRVersionForPSVersion(void)
{
    return E_NOTIMPL;
}

DWORD
WINAPI
PerformWSManPluginReportCompletion(void)
{
    return ERROR_NOT_SUPPORTED;
}
