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

/* Console and base server requests without capture buffers */
typedef struct
{
    ULONG ConsoleHandle, OutputHandle;
    COORD ScreenBufferSize, CursorPosition, ViewOrigin;
    WORD Attributes;
    COORD ViewSize, MaximumViewSize;
} WOW64_CONSOLE_SCREEN_BUFFER_INFO;
C_ASSERT(sizeof(WOW64_CONSOLE_SCREEN_BUFFER_INFO) == 32);
C_ASSERT(FIELD_OFFSET(WOW64_CONSOLE_SCREEN_BUFFER_INFO, ViewSize) == 22); /* COORD is 2-byte aligned */

typedef struct { ULONG ConsoleHandle; UINT CodePage; BOOL OutputCP; } WOW64_CONSOLE_GET_CP;
typedef struct { ULONG ConsoleHandle; UINT CodePage; BOOL OutputCP; ULONG EventHandle; } WOW64_CONSOLE_SET_CP;
typedef struct { ULONG ConsoleHandle; LANGID LangId; } WOW64_CONSOLE_LANGID;
C_ASSERT(sizeof(WOW64_CONSOLE_GET_CP) == 12 && sizeof(WOW64_CONSOLE_SET_CP) == 16);
C_ASSERT(sizeof(WOW64_CONSOLE_LANGID) == 8);
typedef struct { ULONG ConsoleHandle, HandleType, DesiredAccess, InheritHandle, ShareMode, Handle; }
    WOW64_CONSOLE_OPENCONSOLE;
C_ASSERT(sizeof(WOW64_CONSOLE_OPENCONSOLE) == 24);
typedef struct { ULONG ConsoleHandle, DesiredAccess, InheritHandle, ShareMode, ScreenBufferType;
                 UCHAR GraphicsBufferInfo[20]; ULONG hMutex, lpBitMap, OutputHandle; }
    WOW64_CONSOLE_CREATESCREENBUFFER;
C_ASSERT(sizeof(WOW64_CONSOLE_CREATESCREENBUFFER) == 52);
C_ASSERT(sizeof(BASE_GETSET_PROCESS_SHUTDOWN_PARAMS) == 8);

/* ConsolepAttach: the start info is a pointer into the guest capture buffer */
typedef struct { ULONG ProcessId, ConsoleStartInfo, CtrlRoutine, PropRoutine; } WOW64_CONSOLE_ATTACH;
C_ASSERT(sizeof(WOW64_CONSOLE_ATTACH) == 16);

/* ConsolepFree, ConsolepGetConsoleWindow and ConsolepGetProcessList. The process list is a
 * pointer into the guest capture buffer. */
typedef struct { ULONG ConsoleHandle; } WOW64_CONSOLE_FREE;
C_ASSERT(sizeof(WOW64_CONSOLE_FREE) == 4);
typedef struct { ULONG ConsoleHandle, WindowHandle; } WOW64_CONSOLE_GETWINDOW;
C_ASSERT(sizeof(WOW64_CONSOLE_GETWINDOW) == 8);
typedef struct { ULONG ConsoleHandle, ProcessCount, ProcessIdsList; } WOW64_CONSOLE_GETPROCESSLIST;
C_ASSERT(sizeof(WOW64_CONSOLE_GETPROCESSLIST) == 12);
