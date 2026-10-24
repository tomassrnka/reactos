/*
 * COPYRIGHT:            See COPYING in the top level directory
 * PROJECT:              ReactOS kernel
 * FILE:                 lib/opengl32/icdload.c
 * PURPOSE:              OpenGL32 lib, ICD dll loader
 */

#include "opengl32.h"

#include <winreg.h>

WINE_DEFAULT_DEBUG_CHANNEL(opengl32);

/* based off https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/gallium/frontends/wgl/gldrv.h */
typedef struct
{
    ULONG Version;                    /*!< Driver interface version */
    ULONG DriverVersion;              /*!< Driver version */
    WCHAR DriverName[MAX_PATH + 1];   /*!< Driver name */
} Drv_Opengl_Info, *pDrv_Opengl_Info;

#ifndef OPENGL_GETINFO_DRVNAME
#define OPENGL_GETINFO_DRVNAME 0
#endif

typedef enum
{
    OGL_CD_NOT_QUERIED,
    OGL_CD_NONE,
    OGL_CD_ROSSWI,
    OGL_CD_CUSTOM_ICD
} CUSTOM_DRIVER_STATE;

typedef enum
{
    OGL_SD_NOT_QUERIED,
    OGL_SD_NONE,
    OGL_SD_AVAILABLE,
    OGL_SD_BROKEN
} SYSTEM_DRIVER_STATE;

static CRITICAL_SECTION icdload_cs = {NULL, -1, 0, 0, 0, 0};
static struct ICD_Data* ICD_Data_List = NULL;
static const WCHAR OpenGLDrivers_Key[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers";
static const WCHAR CustomDrivers_Key[] = L"SOFTWARE\\ReactOS\\OpenGL";
static Drv_Opengl_Info CustomDrvInfo;
static CUSTOM_DRIVER_STATE CustomDriverState = OGL_CD_NOT_QUERIED;
static Drv_Opengl_Info SystemDrvInfo;
static SYSTEM_DRIVER_STATE SystemDriverState = OGL_SD_NOT_QUERIED;

static void APIENTRY wglSetCurrentValue(PVOID value)
{
    IntSetCurrentICDPrivate(value);
}

static PVOID APIENTRY wglGetCurrentValue()
{
    return IntGetCurrentICDPrivate();
}

/* An ICD passes the handle of the application, which may be stale or invalid */
static DHGLRC APIENTRY wglGetDHGLRC(struct wgl_context* context)
{
    struct wgl_context* ctx = get_context((HGLRC)context);

    return ctx ? ctx->dhglrc : 0;
}

/* GDI entry points (win32k) */
extern INT APIENTRY GdiDescribePixelFormat(HDC hdc, INT ipfd, UINT cjpfd, PPIXELFORMATDESCRIPTOR ppfd);
extern BOOL APIENTRY GdiSetPixelFormat(HDC hdc, INT ipfd);
extern BOOL APIENTRY GdiSwapBuffers(HDC hdc);

/* Fills Info from the OpenGLDrivers subkey of that name; only a registered driver has a Version */
static BOOL IntReadDriverRegistration(PCWSTR Name, pDrv_Opengl_Info Info)
{
    HKEY OglKey, DrvKey;
    DWORD dwInput, dwValueType;
    BOOL Found = FALSE;

    memset(Info, 0, sizeof(*Info));
    if(wcslen(Name) >= ARRAYSIZE(Info->DriverName))
        return FALSE;
    wcscpy(Info->DriverName, Name);

    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE, OpenGLDrivers_Key, 0, KEY_READ, &OglKey) != ERROR_SUCCESS)
        return FALSE;

    if(RegOpenKeyExW(OglKey, Name, 0, KEY_READ, &DrvKey) == ERROR_SUCCESS)
    {
        dwInput = sizeof(Info->Version);
        if((RegQueryValueExW(DrvKey, L"Version", 0, &dwValueType, (LPBYTE)&Info->Version, &dwInput) == ERROR_SUCCESS) &&
           (dwValueType == REG_DWORD))
        {
            dwInput = sizeof(Info->DriverVersion);
            if((RegQueryValueExW(DrvKey, L"DriverVersion", 0, &dwValueType, (LPBYTE)&Info->DriverVersion, &dwInput) != ERROR_SUCCESS) ||
               (dwValueType != REG_DWORD))
                Info->DriverVersion = 0;
            Found = TRUE;
        }
        RegCloseKey(DrvKey);
    }

    RegCloseKey(OglKey);
    return Found;
}

/* Reads the driver name stored as the default value of a ...\ReactOS\OpenGL key; FALSE if there is none */
static BOOL IntReadRendererName(HKEY Root, PWSTR Name, DWORD NameSize)
{
    HKEY Key;
    DWORD dwType, dwSize = NameSize - sizeof(WCHAR);
    LONG ret;

    if(RegOpenKeyExW(Root, CustomDrivers_Key, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return FALSE;

    ret = RegQueryValueExW(Key, L"", 0, &dwType, (LPBYTE)Name, &dwSize);
    RegCloseKey(Key);

    if((ret != ERROR_SUCCESS) || (dwType != REG_SZ) || (dwSize % sizeof(WCHAR)))
        return FALSE;

    /* The value may lack its terminator, which the room that was left makes up */
    Name[dwSize / sizeof(WCHAR)] = UNICODE_NULL;
    return wcslen(Name) != 0;
}

/* The system default driver is only for a window or screen DC: a bitmap or printer DC keeps the generic implementation */
static BOOL IntIsDisplayDC(HDC hdc)
{
    return (GetObjectType(hdc) == OBJ_DC) && (GetDeviceCaps(hdc, TECHNOLOGY) == DT_RASDISPLAY);
}

/* The ICD that the system installs for display drivers that bring none (HKLM\Software\ReactOS\OpenGL) */
static pDrv_Opengl_Info IntGetSystemDefaultDriver(void)
{
    pDrv_Opengl_Info Info = NULL;

    EnterCriticalSection(&icdload_cs);
    if(SystemDriverState == OGL_SD_NOT_QUERIED)
    {
        WCHAR Name[MAX_PATH];

        SystemDriverState = OGL_SD_NONE;

        if(IntReadRendererName(HKEY_LOCAL_MACHINE, Name, sizeof(Name)) &&
           _wcsicmp(Name, L"ReactOS Software Implementation") &&
           IntReadDriverRegistration(Name, &SystemDrvInfo))
            SystemDriverState = OGL_SD_AVAILABLE;
    }
    if(SystemDriverState == OGL_SD_AVAILABLE)
        Info = &SystemDrvInfo;
    LeaveCriticalSection(&icdload_cs);

    return Info;
}

/* Retrieves the ICD data (driver version + relevant DLL entry points) for a device context */
struct ICD_Data* IntGetIcdData(HDC hdc)
{
    int ret;
    DWORD dwInput, dwValueType, Version, DriverVersion, Flags, RequiresSse2;
    Drv_Opengl_Info DrvInfo;
    pDrv_Opengl_Info pDrvInfo;
    struct ICD_Data* data;
    HKEY OglKey = NULL;
    HKEY DrvKey;
    WCHAR DllName[MAX_PATH];
    BOOL (WINAPI *DrvValidateVersion)(DWORD);
    void (WINAPI *DrvSetCallbackProcs)(int nProcs, PROC* pProcs);

    /* The following code is ReactOS specific and allows us to easily load an arbitrary ICD:
     * It checks HKCU\Software\ReactOS\OpenGL for a custom ICD and will always load it
     * no matter what driver the DC is associated with. It can also force using the
     * built-in Software Implementation*/
    if(CustomDriverState == OGL_CD_NOT_QUERIED)
    {
        WCHAR Name[MAX_PATH];

        /* Only do this once so there's not any significant performance penalty */
        CustomDriverState = OGL_CD_NONE;
        memset(&CustomDrvInfo, 0, sizeof(Drv_Opengl_Info));

        if(IntReadRendererName(HKEY_CURRENT_USER, Name, sizeof(Name)))
        {
            if(!_wcsicmp(Name, L"ReactOS Software Implementation"))
            {
                /* Always announce the fact that we're forcing ROSSWI */
                ERR("Forcing ReactOS Software Implementation\n");
                CustomDriverState = OGL_CD_ROSSWI;
                return NULL;
            }

            if(IntReadDriverRegistration(Name, &CustomDrvInfo))
            {
                CustomDriverState = OGL_CD_CUSTOM_ICD;

                /* Always announce the fact that we're overriding the default driver */
                ERR("Overriding the default OGL ICD with %S\n", CustomDrvInfo.DriverName);
            }
        }
    }

    /* If there's a custom ICD or ROSSWI was requested use it, otherwise proceed as usual */
    if(CustomDriverState == OGL_CD_CUSTOM_ICD)
    {
        pDrvInfo = &CustomDrvInfo;
    }
    else if(CustomDriverState == OGL_CD_ROSSWI)
    {
        return NULL;
    }
    else
    {
        /* First, see if the driver supports this */
        dwInput = OPENGL_GETINFO;
        ret = ExtEscape(hdc, QUERYESCSUPPORT, sizeof(DWORD), (LPCSTR)&dwInput, 0, NULL);

        if(ret <= 0)
        {
            /* Driver doesn't support opengl: the system default ICD, if one is installed, else the built-in implementation */
            if(!IntIsDisplayDC(hdc))
                return NULL;

            pDrvInfo = IntGetSystemDefaultDriver();
            if(!pDrvInfo)
                return NULL;
        }
        else
        {
            /* Query for the ICD DLL name and version */
            dwInput = OPENGL_GETINFO_DRVNAME;
            ret = ExtEscape(hdc, OPENGL_GETINFO, sizeof(DWORD), (LPCSTR)&dwInput, sizeof(DrvInfo), (LPSTR)&DrvInfo);

            if(ret <= 0)
            {
                ERR("Driver claims to support OPENGL_GETINFO escape code, but doesn't. ret: %X\n", ret);
                return NULL;
            }

            pDrvInfo = &DrvInfo;
        }
    }

    /* Protect the list while we are loading*/
    EnterCriticalSection(&icdload_cs);

    /* Search for it in the list of already loaded modules */
    data = ICD_Data_List;
    while(data)
    {
        if(!_wcsicmp(data->DriverName, pDrvInfo->DriverName))
        {
            /* Found it */
            TRACE("Found already loaded %p.\n", data);
            LeaveCriticalSection(&icdload_cs);
            return data;
        }
        data = data->next;
    }

    /* It was still not loaded, look for it in the registry */
    ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, OpenGLDrivers_Key, 0, KEY_READ, &OglKey);
    if(ret != ERROR_SUCCESS)
    {
        ERR("Failed to open the OpenGLDrivers key.\n");
        goto end;
    }
    ret = RegOpenKeyExW(OglKey, pDrvInfo->DriverName, 0, KEY_READ, &DrvKey);
    if(ret != ERROR_SUCCESS)
    {
        /* Some driver installer just provide the DLL name, like the Matrox G400 */
        TRACE("No driver subkey for %S, trying to get DLL name directly.\n", pDrvInfo->DriverName);
        dwInput = sizeof(DllName);
        ret = RegQueryValueExW(OglKey, pDrvInfo->DriverName, 0, &dwValueType, (LPBYTE)DllName, &dwInput);
        if((ret != ERROR_SUCCESS) || (dwValueType != REG_SZ))
        {
            ERR("Unable to get ICD DLL name!\n");
            RegCloseKey(OglKey);
            goto end;
        }
        Version = DriverVersion = Flags = 0;
        TRACE("DLL name is %S.\n", DllName);
    }
    else
    {
        /* The driver have a subkey for the ICD */
        TRACE("Querying details from registry for %S.\n", pDrvInfo->DriverName);
        dwInput = sizeof(DllName);
        ret = RegQueryValueExW(DrvKey, L"Dll", 0, &dwValueType, (LPBYTE)DllName, &dwInput);
        if((ret != ERROR_SUCCESS) || (dwValueType != REG_SZ))
        {
            ERR("Unable to get ICD DLL name!.\n");
            RegCloseKey(DrvKey);
            RegCloseKey(OglKey);
            goto end;
        }

        dwInput = sizeof(Version);
        ret = RegQueryValueExW(DrvKey, L"Version", 0, &dwValueType, (LPBYTE)&Version, &dwInput);
        if((ret != ERROR_SUCCESS) || (dwValueType != REG_DWORD))
        {
            WARN("No version in driver subkey\n");
        }
        else if(Version != pDrvInfo->Version)
        {
            ERR("Version mismatch between registry (%lu) and display driver (%lu).\n", Version, pDrvInfo->Version);
            RegCloseKey(DrvKey);
            RegCloseKey(OglKey);
            goto end;
        }

        dwInput = sizeof(DriverVersion);
        ret = RegQueryValueExW(DrvKey, L"DriverVersion", 0, &dwValueType, (LPBYTE)&DriverVersion, &dwInput);
        if((ret != ERROR_SUCCESS) || (dwValueType != REG_DWORD))
        {
            WARN("No driver version in driver subkey\n");
        }
        else if(DriverVersion != pDrvInfo->DriverVersion)
        {
            ERR("Driver version mismatch between registry (%lu) and display driver (%lu).\n", DriverVersion, pDrvInfo->DriverVersion);
            RegCloseKey(DrvKey);
            RegCloseKey(OglKey);
            goto end;
        }

        dwInput = sizeof(Flags);
        ret = RegQueryValueExW(DrvKey, L"Flags", 0, &dwValueType, (LPBYTE)&Flags, &dwInput);
        if((ret != ERROR_SUCCESS) || (dwValueType != REG_DWORD))
        {
            WARN("No driver version in driver subkey\n");
            Flags = 0;
        }

        /* ReactOS specific: a driver that was built for SSE2 is not loaded on a processor without it */
        dwInput = sizeof(RequiresSse2);
        ret = RegQueryValueExW(DrvKey, L"RequiresSSE2", 0, &dwValueType, (LPBYTE)&RequiresSse2, &dwInput);
        if((ret == ERROR_SUCCESS) && (dwValueType == REG_DWORD) && RequiresSse2 &&
           !IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE))
        {
            ERR("The ICD %S needs SSE2, which this processor lacks.\n", pDrvInfo->DriverName);
            RegCloseKey(DrvKey);
            RegCloseKey(OglKey);
            goto end;
        }

        /* We're done */
        RegCloseKey(DrvKey);
        TRACE("DLL name is %S, Version %lx, DriverVersion %lx, Flags %lx.\n", DllName, Version, DriverVersion, Flags);
    }
    /* No need for this anymore */
    RegCloseKey(OglKey);

    /* So far so good, allocate data */
    data = HeapAlloc(GetProcessHeap(), 0, sizeof(*data));
    if(!data)
    {
        ERR("Unable to allocate ICD data!\n");
        goto end;
    }

    /* Load the library */
    data->hModule = LoadLibraryW(DllName);
    if(!data->hModule)
    {
        ERR("Could not load the ICD DLL: %S.\n", DllName);
        HeapFree(GetProcessHeap(), 0, data);
        data = NULL;
        goto end;
    }

    /*
     * Validate version, if needed.
     * Some drivers (at least VBOX), initialize stuff upon this call.
     */
    DrvValidateVersion = (void*)GetProcAddress(data->hModule, "DrvValidateVersion");
    if(DrvValidateVersion)
    {
        if(!DrvValidateVersion(pDrvInfo->DriverVersion))
        {
            ERR("DrvValidateVersion failed!.\n");
            goto fail;
        }
    }

    /* Pass the callbacks */
    DrvSetCallbackProcs = (void*)GetProcAddress(data->hModule, "DrvSetCallbackProcs");
    if(DrvSetCallbackProcs)
    {
        PROC callbacks[] = {
            (PROC)wglSetCurrentValue,
            (PROC)wglGetCurrentValue,
            (PROC)wglGetDHGLRC};
        DrvSetCallbackProcs(ARRAYSIZE(callbacks), callbacks);
    }

    /* Get the DLL exports */
#define DRV_LOAD(x) do                                  \
{                                                       \
    data->x = (void*)GetProcAddress(data->hModule, #x); \
    if(!data->x) {                                      \
        ERR("%S lacks " #x "!\n", DllName);             \
        goto fail;                                      \
    }                                                   \
} while(0)
    DRV_LOAD(DrvCopyContext);
    DRV_LOAD(DrvCreateContext);
    DRV_LOAD(DrvCreateLayerContext);
    DRV_LOAD(DrvDeleteContext);
    DRV_LOAD(DrvDescribeLayerPlane);
    DRV_LOAD(DrvDescribePixelFormat);
    DRV_LOAD(DrvGetLayerPaletteEntries);
    DRV_LOAD(DrvGetProcAddress);
    DRV_LOAD(DrvReleaseContext);
    DRV_LOAD(DrvRealizeLayerPalette);
    DRV_LOAD(DrvSetContext);
    DRV_LOAD(DrvSetLayerPaletteEntries);
    DRV_LOAD(DrvSetPixelFormat);
    DRV_LOAD(DrvShareLists);
    DRV_LOAD(DrvSwapBuffers);
    DRV_LOAD(DrvSwapLayerBuffers);
#undef DRV_LOAD

    /* Let's see if GDI should handle this instead of the ICD DLL */
    // FIXME: maybe there is a better way
    if (GdiDescribePixelFormat(hdc, 0, 0, NULL) != 0)
    {
        /* GDI knows what to do with that. Override */
        TRACE("Forwarding WGL calls to win32k!\n");
        data->DrvDescribePixelFormat = GdiDescribePixelFormat;
        data->DrvSetPixelFormat = GdiSetPixelFormat;
        data->DrvSwapBuffers = GdiSwapBuffers;
    }

    /* Copy the DriverName */
    wcscpy(data->DriverName, pDrvInfo->DriverName);

    /* Push the list */
    data->next = ICD_Data_List;
    ICD_Data_List = data;

    TRACE("Returning %p.\n", data);
    TRACE("ICD driver %S (%S) successfully loaded.\n", pDrvInfo->DriverName, DllName);

end:
    /* A system default driver that does not load is not retried for every DC */
    if(!data && (pDrvInfo == &SystemDrvInfo))
        SystemDriverState = OGL_SD_BROKEN;

    /* Unlock and return */
    LeaveCriticalSection(&icdload_cs);
    return data;

fail:
    if(pDrvInfo == &SystemDrvInfo)
        SystemDriverState = OGL_SD_BROKEN;
    LeaveCriticalSection(&icdload_cs);
    FreeLibrary(data->hModule);
    HeapFree(GetProcessHeap(), 0, data);
    return NULL;
}

void IntDeleteAllICDs(void)
{
    struct ICD_Data* data;

    EnterCriticalSection(&icdload_cs);

    while (ICD_Data_List != NULL)
    {
        data = ICD_Data_List;
        ICD_Data_List = data->next;

        FreeLibrary(data->hModule);
        HeapFree(GetProcessHeap(), 0, data);
    }
}
