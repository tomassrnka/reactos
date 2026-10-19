/* Explicit i386 CSR layouts, from ReactOS base.h and conmsg.h.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once


typedef struct _WOW64_BASE_DATA
{
    WOW64_STRING WindowsDirectory;
    WOW64_STRING WindowsSystemDirectory;
    WOW64_STRING NamedObjectDirectory;
    USHORT WindowsMajorVersion;
    USHORT WindowsMinorVersion;
    USHORT BuildNumber;
    USHORT CSDNumber;
    USHORT RCNumber;
    WCHAR CSDVersion[128];
    WOW64_SYSTEM_BASIC_INFORMATION SysInfo;
    SYSTEM_TIMEOFDAY_INFORMATION TimeOfDay;
    ULONG IniFileMapping;
    NLS_USER_INFO NlsUserInfo;
    BOOLEAN DefaultSeparateVDM;
    BOOLEAN IsWowTaskReady;
    WOW64_STRING WindowsSys32x86Directory;
    BOOLEAN fTermsrvAppInstallMode;
    TIME_ZONE_INFORMATION tziTermsrvClientTimeZone;
    KSYSTEM_TIME ktTermsrvClientBias;
    ULONG TermsrvClientTimeZoneId;
    BOOLEAN LUIDDeviceMapsEnabled;
    ULONG TermsrvClientTimeZoneChangeNum;
} WOW64_BASE_DATA, *PWOW64_BASE_DATA;

typedef struct _WOW64_CONSOLE_PROPERTIES
{
    INT   IconIndex;
    ULONG hIcon;
    ULONG hIconSm;
    DWORD dwHotKey;
    DWORD dwStartupFlags;

    // NT_CONSOLE_PROPS
    WORD wFillAttribute;
    WORD wPopupFillAttribute;

    //
    // Not on MSDN, but show up in binary
    //
    WORD wShowWindow;
    WORD wUnknown;

    COORD dwScreenBufferSize;
    COORD dwWindowSize;
    COORD dwWindowOrigin;
    DWORD nFont;
    DWORD nInputBufferSize;
    COORD dwFontSize;
    UINT  uFontFamily;
    UINT  uFontWeight;
    WCHAR FaceName[LF_FACESIZE];
    UINT  uCursorSize;
    BOOL  bFullScreen;
    BOOL  bQuickEdit;
    BOOL  bInsertMode;
    BOOL  bAutoPosition;
    UINT  uHistoryBufferSize;
    UINT  uNumberOfHistoryBuffers;
    BOOL  bHistoryNoDup;
    COLORREF ColorTable[16];

    // NT_FE_CONSOLE_PROPS
    UINT uCodePage;
} WOW64_CONSOLE_PROPERTIES;

typedef struct
{
    ULONG ConsoleHandle, InputWaitHandle, InputHandle, OutputHandle, ErrorHandle;
    ULONG InitEvents[MAX_INIT_EVENTS];
    WOW64_CONSOLE_PROPERTIES;
} WOW64_CONSOLE_START;
typedef struct _WOW64_CONSOLE_CONNECT
{
    WOW64_CONSOLE_START ConsoleStartInfo;

    BOOLEAN IsConsoleApp;
    BOOLEAN IsWindowVisible;

    // USHORT Padding;

    ULONG CtrlRoutine;
    ULONG PropRoutine;
    ULONG ImeRoutine;

    ULONG  TitleLength;
    WCHAR  ConsoleTitle[MAX_PATH + 1];  // Console title or full path to the startup shortcut
    ULONG  DesktopLength;
    ULONG Desktop;                     // Contrary to the case of CONSOLE_ALLOCCONSOLE, the
                                        // desktop string is allocated in the process' heap,
                                        // and CSR will read it via NtReadVirtualMemory.
    ULONG  AppNameLength;
    WCHAR  AppName[128];                // Full path of the launched app
    ULONG  CurDirLength;
    WCHAR  CurDir[MAX_PATH + 1];
} WOW64_CONSOLE_CONNECT, *PWOW64_CONSOLE_CONNECT;


C_ASSERT(sizeof(WOW64_BASE_DATA) == 0x1ac8);
C_ASSERT(sizeof(WOW64_CONSOLE_START) == 0xfc);
C_ASSERT(sizeof(WOW64_CONSOLE_CONNECT) == 0x638);

typedef struct
{
    ULONG ConsoleHandle, OutputHandle;
    CHAR StaticBuffer[80];
    ULONG Buffer, NumBytes, Reserved1;
    BOOLEAN UsingStaticBuffer, Unicode;
    CHAR Reserved2[6];
} WOW64_CONSOLE_WRITE;
