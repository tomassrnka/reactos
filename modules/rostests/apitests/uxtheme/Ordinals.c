/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     uxtheme exports its named functions at the ordinals of Windows 10
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <windows.h>

/*
 * Programs that reach into uxtheme by ordinal (dark mode support, for one) expect the Windows 10 numbering. The ordinals
 * Windows 10 gives to functions without a name must not belong to a named function here, or the program calls the wrong
 * function. The names and ordinals are those of uxtheme.dll of Windows 10 22H2, measured on the reference system.
 */
typedef struct _EXPORT_ORDINAL
{
    const char *Name;
    WORD Ordinal;
} EXPORT_ORDINAL;

static const EXPORT_ORDINAL g_Exports[] =
{
    { "BeginPanningFeedback", 5 },
    { "EndPanningFeedback", 6 },
    { "UpdatePanningFeedback", 12 },
    { "BeginBufferedAnimation", 37 },
    { "BeginBufferedPaint", 38 },
    { "BufferedPaintClear", 39 },
    { "BufferedPaintInit", 40 },
    { "BufferedPaintRenderAnimation", 41 },
    { "BufferedPaintSetAlpha", 42 },
    { "DrawThemeBackgroundEx", 47 },
    { "BufferedPaintStopAllAnimations", 51 },
    { "BufferedPaintUnInit", 52 },
    { "CloseThemeData", 53 },
    { "DllCanUnloadNow", 54 },
    { "DllGetActivationFactory", 55 },
    { "DllGetClassObject", 56 },
    { "DrawThemeBackground", 57 },
    { "DrawThemeEdge", 58 },
    { "DrawThemeIcon", 59 },
    { "OpenThemeDataEx", 61 },
    { "DrawThemeParentBackground", 70 },
    { "DrawThemeParentBackgroundEx", 71 },
    { "DrawThemeText", 89 },
    { "GetImmersiveColorFromColorSetEx", 95 },
    { "GetImmersiveUserColorSetPreference", 98 },
    { "DrawThemeTextEx", 103 },
    { "EnableThemeDialogTexture", 114 },
    { "GetUserColorPreference", 120 },
    { "GetColorFromPreference", 121 },
    { "EnableTheming", 129 },
    { "EndBufferedAnimation", 134 },
    { "EndBufferedPaint", 140 },
    { "GetBufferedPaintBits", 141 },
    { "GetBufferedPaintDC", 142 },
    { "GetBufferedPaintTargetDC", 143 },
    { "GetBufferedPaintTargetRect", 144 },
    { "GetCurrentThemeName", 145 },
    { "GetThemeAnimationProperty", 146 },
    { "GetThemeAnimationTransform", 147 },
    { "GetThemeAppProperties", 148 },
    { "GetThemeBackgroundContentRect", 149 },
    { "GetThemeBackgroundExtent", 150 },
    { "GetThemeBackgroundRegion", 151 },
    { "GetThemeBitmap", 152 },
    { "GetThemeBool", 153 },
    { "GetThemeColor", 154 },
    { "GetThemeDocumentationProperty", 155 },
    { "GetThemeEnumValue", 156 },
    { "GetThemeFilename", 157 },
    { "GetThemeFont", 158 },
    { "GetThemeInt", 159 },
    { "GetThemeIntList", 160 },
    { "GetThemeMargins", 161 },
    { "GetThemeMetric", 162 },
    { "GetThemePartSize", 163 },
    { "GetThemePosition", 164 },
    { "GetThemePropertyOrigin", 165 },
    { "GetThemeRect", 166 },
    { "GetThemeStream", 167 },
    { "GetThemeString", 168 },
    { "GetThemeSysBool", 169 },
    { "GetThemeSysColor", 170 },
    { "GetThemeSysColorBrush", 171 },
    { "GetThemeSysFont", 172 },
    { "GetThemeSysInt", 173 },
    { "GetThemeSysSize", 174 },
    { "GetThemeSysString", 175 },
    { "GetThemeTextExtent", 176 },
    { "GetThemeTextMetrics", 177 },
    { "GetThemeTimingFunction", 178 },
    { "GetThemeTransitionDuration", 179 },
    { "GetWindowTheme", 180 },
    { "HitTestThemeBackground", 181 },
    { "IsAppThemed", 182 },
    { "IsCompositionActive", 183 },
    { "IsThemeActive", 184 },
    { "IsThemeBackgroundPartiallyTransparent", 185 },
    { "IsThemeDialogTextureEnabled", 186 },
    { "IsThemePartDefined", 187 },
    { "OpenThemeData", 188 },
    { "OpenThemeDataForDpi", 189 },
    { "SetThemeAppProperties", 190 },
    { "SetWindowTheme", 191 },
    { "SetWindowThemeAttribute", 192 },
    { "ThemeInitApiHook", 193 },
};

/* Ordinals of Windows 10 functions that have no name: RefreshImmersiveColorPolicyState, AllowDarkModeForWindow, SetPreferredAppMode, FlushMenuThemes and IsDarkModeAllowedForWindow of dark mode */
static const WORD g_Unnamed[] = { 104, 133, 135, 136, 137 };

static BOOL is_windows_10_or_later(void)
{
    typedef LONG (WINAPI *FN_RtlGetVersion)(OSVERSIONINFOW *);
    FN_RtlGetVersion pRtlGetVersion = (FN_RtlGetVersion)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW Info = { sizeof(Info) };

    return pRtlGetVersion && pRtlGetVersion(&Info) == 0 && Info.dwMajorVersion >= 10;
}

START_TEST(Ordinals)
{
    HMODULE Module = GetModuleHandleA("uxtheme.dll");
    size_t n, m;

    if (!is_reactos() && !is_windows_10_or_later())
    {
        skip("The ordinals are those of Windows 10\n");
        return;
    }
    if (!Module)
        Module = LoadLibraryA("uxtheme.dll");
    ok(Module != NULL, "uxtheme.dll did not load: %lu\n", GetLastError());
    if (!Module)
        return;

    for (n = 0; n < ARRAYSIZE(g_Exports); ++n)
    {
        FARPROC ByName = GetProcAddress(Module, g_Exports[n].Name);
        FARPROC ByOrdinal = GetProcAddress(Module, (LPCSTR)(ULONG_PTR)g_Exports[n].Ordinal);

        /* ReactOS does not have every function that Windows 10 has */
        if (!ByName && is_reactos())
            continue;
        ok(ByName != NULL, "%s is not exported\n", g_Exports[n].Name);
        ok(ByName == ByOrdinal, "%s is at %p by name and at %p by ordinal %u\n",
           g_Exports[n].Name, (void *)ByName, (void *)ByOrdinal, g_Exports[n].Ordinal);
    }

    for (n = 0; n < ARRAYSIZE(g_Unnamed); ++n)
    {
        FARPROC Proc = GetProcAddress(Module, (LPCSTR)(ULONG_PTR)g_Unnamed[n]);

        if (!Proc)
            continue;
        for (m = 0; m < ARRAYSIZE(g_Exports); ++m)
            ok(Proc != GetProcAddress(Module, g_Exports[m].Name), "Ordinal %u is %s\n", g_Unnamed[n], g_Exports[m].Name);
    }
}
