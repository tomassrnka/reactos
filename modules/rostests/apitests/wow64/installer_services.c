/* NT services used by installer loader and administrator checks. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <mountmgr.h>
#include <stdio.h>
#ifndef VOLUME_NAME_NT
#define VOLUME_NAME_NT 2
#endif

static unsigned Checks, Failures;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%lu\n", __LINE__, #x, GetLastError()); } } while (0)

static void CheckVolumeQuery(const WCHAR *NtPath)
{
    typedef LONG (WINAPI *IOCTL)(HANDLE, HANDLE, PVOID, PVOID, PVOID, ULONG, PVOID, ULONG, PVOID, ULONG);
    IOCTL Ioctl = (IOCTL)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeviceIoControlFile");
    struct { ULONG_PTR Status, Information; ULONG Guard[2]; } Io = {-1, 0, {0x12345678, 0xabcdef01}};
    union { MOUNTMGR_TARGET_NAME Name; WCHAR Space[MAX_PATH]; } Input;
    union { MOUNTMGR_VOLUME_PATHS Paths; WCHAR Space[MAX_PATH]; } Output;
    const WCHAR *End = wcschr(NtPath + wcslen(L"\\Device\\"), L'\\');
    HANDLE Device, Event;
    LONG Status;
    CHECK(End != NULL && Ioctl != NULL);
    if (!End || !Ioctl) return;
    Input.Name.DeviceNameLength = (End - NtPath) * sizeof(WCHAR);
    memcpy(Input.Name.DeviceName, NtPath, Input.Name.DeviceNameLength);
    Device = CreateFileW(MOUNTMGR_DOS_DEVICE_NAME, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    CHECK(Device != INVALID_HANDLE_VALUE);
    if (Device == INVALID_HANDLE_VALUE) return;
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(Event != NULL);
    if (Event)
    {
        Status = Ioctl(Device, Event, NULL, NULL, &Io, IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATH,
                       &Input, sizeof(USHORT) + Input.Name.DeviceNameLength, &Output, sizeof(Output));
        CHECK(Status == 0 || Status == 0x103);
        if (Status == 0 || Status == 0x103)
        {
            CHECK(WaitForSingleObject(Event, 10000) == WAIT_OBJECT_0);
            CHECK(Io.Status == 0 && Io.Information >= sizeof(ULONG) + 2 * sizeof(WCHAR));
            CHECK(Io.Guard[0] == 0x12345678 && Io.Guard[1] == 0xabcdef01);
            CHECK(Output.Paths.MultiSzLength >= 2 * sizeof(WCHAR) && Output.Paths.MultiSz[1] == L':');
        }
        CloseHandle(Event);
    }
    CloseHandle(Device);
}

static void CheckVolumeByHandle(void)
{
    typedef BOOL (WINAPI *VOLUME)(HANDLE, PWSTR, DWORD, PDWORD, PDWORD, PDWORD, PWSTR, DWORD);
    VOLUME Query = (VOLUME)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetVolumeInformationByHandleW");
    WCHAR Path[MAX_PATH], Root[4], Label[MAX_PATH], FileSystem[MAX_PATH];
    struct { WCHAR Text[MAX_PATH]; ULONG Guard; } OutLabel, OutSystem;
    DWORD Serial, Maximum, Flags, OutSerial, OutMaximum, OutFlags;
    HANDLE File;
    CHECK(Query != NULL);
    if (!Query) return;
    CHECK(GetModuleFileNameW(NULL, Path, MAX_PATH));
    memcpy(Root, Path, 3 * sizeof(WCHAR)); Root[3] = 0;
    CHECK(GetVolumeInformationW(Root, Label, MAX_PATH, &Serial, &Maximum, &Flags, FileSystem, MAX_PATH));
    File = CreateFileW(Path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, 0, NULL);
    CHECK(File != INVALID_HANDLE_VALUE);
    if (File == INVALID_HANDLE_VALUE) return;
    OutLabel.Guard = OutSystem.Guard = 0x12345678;
    CHECK(Query(File, OutLabel.Text, MAX_PATH, &OutSerial, &OutMaximum, &OutFlags, OutSystem.Text, MAX_PATH));
    CHECK(!wcscmp(Label, OutLabel.Text) && !wcscmp(FileSystem, OutSystem.Text));
    CHECK(Serial == OutSerial && Maximum == OutMaximum && Flags == OutFlags);
    CHECK(OutLabel.Guard == 0x12345678 && OutSystem.Guard == 0x12345678);
    CHECK(Query(File, NULL, 0, NULL, NULL, NULL, NULL, 0));
    CHECK(Query(File, OutLabel.Text, wcslen(Label) + 1, NULL, NULL, NULL, OutSystem.Text, wcslen(FileSystem) + 1));
    OutSystem.Text[0] = 0x5555;
    CHECK(!Query(File, NULL, 0, NULL, NULL, NULL, OutSystem.Text, wcslen(FileSystem)) && GetLastError() == ERROR_BAD_LENGTH);
    CHECK(OutSystem.Text[0] == 0x5555 && OutSystem.Guard == 0x12345678);
    /* Depending on label length, either the native query overflows or the
     * returned label cannot be terminated in the caller's empty buffer. */
    OutLabel.Text[0] = 0x5555;
    CHECK(!Query(File, OutLabel.Text, 0, NULL, NULL, NULL, NULL, 0));
    CHECK(GetLastError() == ERROR_MORE_DATA || GetLastError() == ERROR_BAD_LENGTH);
    CHECK(OutLabel.Text[0] == 0x5555 && OutLabel.Guard == 0x12345678);
    CHECK(GetFileType(File) == FILE_TYPE_DISK);
    CHECK(CloseHandle(File));
    CHECK(!Query(INVALID_HANDLE_VALUE, NULL, 0, &OutSerial, NULL, NULL, NULL, 0) && GetLastError() == ERROR_INVALID_HANDLE);
    File = CreateFileW(Root, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    CHECK(File != INVALID_HANDLE_VALUE);
    if (File != INVALID_HANDLE_VALUE)
    {
        CHECK(Query(File, NULL, 0, &OutSerial, NULL, NULL, NULL, 0) && OutSerial == Serial);
        CloseHandle(File);
    }
}

static void CheckObjectName(HANDLE File, const WCHAR *Path)
{
    typedef LONG (WINAPI *QUERY)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    typedef DWORD (WINAPI *FINALPATH)(HANDLE, LPWSTR, DWORD, DWORD);
    typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } NAME;
    QUERY Query = (QUERY)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject");
    FINALPATH FinalPath = (FINALPATH)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFinalPathNameByHandleW");
    ULONG Needed = 0, Returned = 0, Capacity = sizeof(NAME) + MAX_PATH * sizeof(WCHAR);
    BYTE *Storage;
    NAME *Name;
    WCHAR Final[MAX_PATH];
    DWORD Length;
    CHECK(Query && FinalPath);
    if (!Query || !FinalPath) return;
    CHECK(Query(File, 1, NULL, 0, &Needed) < 0);
    CHECK(Needed >= sizeof(NAME) && Needed < 0x10000);
    if (Needed < sizeof(NAME) || Needed >= 0x10000) return;
    Storage = HeapAlloc(GetProcessHeap(), 0, Capacity + 8);
    CHECK(Storage != NULL);
    if (!Storage) return;
    memset(Storage, 0xa5, Capacity + 8);
    Name = (NAME *)Storage;
    CHECK(Query(File, 1, Storage, Capacity, &Needed) == 0 && Needed <= Capacity);
    if (Needed > Capacity) { HeapFree(GetProcessHeap(), 0, Storage); return; }
    CHECK(Query(File, 1, Storage, sizeof(NAME), &Returned) < 0);
    memset(Storage, 0xa5, Capacity + 8);
    CHECK(Query(File, 1, Storage, Needed, &Returned) == 0 && Returned == Needed);
    CHECK(Name->Length && Name->MaximumLength >= Name->Length);
    CHECK((BYTE *)Name->Buffer >= Storage + sizeof(NAME) &&
          (BYTE *)Name->Buffer + Name->MaximumLength <= Storage + Needed);
    CHECK(Storage[Needed] == 0xa5 && Storage[Needed + 7] == 0xa5);
    Length = FinalPath(File, Final, MAX_PATH, 0);
    CHECK(Length && Length < MAX_PATH);
    if (Length && Length < MAX_PATH)
    {
        const WCHAR *Dos = !wcsncmp(Final, L"\\\\?\\", 4) ? Final + 4 : Final;
        CHECK(!_wcsicmp(Dos, Path));
        printf("FINAL PATH %ls\n", Final);
    }
    Length = FinalPath(File, Final, MAX_PATH, VOLUME_NAME_NT);
    CHECK(Length && Length < MAX_PATH && !wcsncmp(Final, L"\\Device\\", 8));
    if (Length && Length < MAX_PATH && !wcsncmp(Final, L"\\Device\\", 8)) CheckVolumeQuery(Final);
    HeapFree(GetProcessHeap(), 0, Storage);
}

static void CheckPipe(void)
{
    HANDLE Read = NULL, Write = NULL;
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), NULL, TRUE};
    const char Data[] = "installer pipe";
    char Buffer[64] = {0};
    DWORD Bytes, Available, Flags, Code = 99, Mode, Inbound, Outbound, Instances;
    BOOL ModeChanged;
    WCHAR Application[MAX_PATH], Command[MAX_PATH + 32];
    STARTUPINFOW Startup = {sizeof(Startup)};
    PROCESS_INFORMATION Process;
    CHECK(CreatePipe(&Read, &Write, &Attributes, 0));
    if (!Read || !Write) return;
    CHECK(GetNamedPipeInfo(Read, &Flags, &Outbound, &Inbound, &Instances));
    CHECK(!(Flags & PIPE_TYPE_MESSAGE) && Inbound && Instances >= 1);
    CHECK(GetHandleInformation(Write, &Flags) && (Flags & HANDLE_FLAG_INHERIT));
    CHECK(SetHandleInformation(Read, HANDLE_FLAG_INHERIT, 0));
    CHECK(WriteFile(Write, Data, sizeof(Data), &Bytes, NULL) && Bytes == sizeof(Data));
    CHECK(PeekNamedPipe(Read, Buffer, 4, &Bytes, &Available, NULL));
    CHECK(Bytes == 4 && Available == sizeof(Data) && !memcmp(Buffer, Data, 4));
    CHECK(ReadFile(Read, Buffer, sizeof(Data), &Bytes, NULL) && Bytes == sizeof(Data));
    CHECK(!memcmp(Buffer, Data, sizeof(Data)));
    Mode = PIPE_NOWAIT;
    ModeChanged = SetNamedPipeHandleState(Read, &Mode, NULL, NULL);
    CHECK(ModeChanged);
    if (ModeChanged && PeekNamedPipe(Read, NULL, 0, NULL, &Available, NULL) && !Available)
    {
        CHECK(!ReadFile(Read, Buffer, sizeof(Buffer), &Bytes, NULL) && GetLastError() == ERROR_NO_DATA);
    }
    Mode = PIPE_WAIT;
    CHECK(SetNamedPipeHandleState(Read, &Mode, NULL, NULL));
    CHECK(GetModuleFileNameW(NULL, Application, MAX_PATH));
    _snwprintf(Command, MAX_PATH + 32, L"\"%ls\" pipe-child", Application);
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    Startup.hStdOutput = Startup.hStdError = Write;
    if (CreateProcessW(Application, Command, NULL, NULL, TRUE, 0, NULL, NULL, &Startup, &Process))
    {
        CHECK(WaitForSingleObject(Process.hProcess, 30000) == WAIT_OBJECT_0);
        CHECK(GetExitCodeProcess(Process.hProcess, &Code) && Code == 59);
        if (Code == STILL_ACTIVE) TerminateProcess(Process.hProcess, 99);
        CloseHandle(Process.hThread);
        CloseHandle(Process.hProcess);
        CloseHandle(Write);
        CHECK(ReadFile(Read, Buffer, sizeof(Buffer), &Bytes, NULL) && Bytes == sizeof(Data));
        CHECK(!memcmp(Buffer, Data, sizeof(Data)));
    }
    else
    {
        CHECK(FALSE);
        CloseHandle(Write);
    }
    CloseHandle(Read);
}

static void CheckAsyncPipe(void)
{
    WCHAR Name[80];
    HANDLE Read, Write;
    struct { OVERLAPPED Io; ULONG Guard[2]; } Pending = {{0}, {0x12345678, 0xabcdef01}};
    char Buffer[16] = {0};
    DWORD Bytes, Wait;
    _snwprintf(Name, 80, L"\\\\.\\pipe\\w643c-async-%lu", GetCurrentProcessId());
    Read = CreateNamedPipeW(Name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                           PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    CHECK(Read != INVALID_HANDLE_VALUE);
    if (Read == INVALID_HANDLE_VALUE) return;
    Write = CreateFileW(Name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(Write != INVALID_HANDLE_VALUE);
    if (Write == INVALID_HANDLE_VALUE) { CloseHandle(Read); return; }
    Pending.Io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(Pending.Io.hEvent != NULL);
    if (!Pending.Io.hEvent) { CloseHandle(Write); CloseHandle(Read); return; }
    CHECK(!ReadFile(Read, Buffer, sizeof(Buffer), &Bytes, &Pending.Io) && GetLastError() == ERROR_IO_PENDING);
    CHECK(WriteFile(Write, "pipe", 4, &Bytes, NULL) && Bytes == 4);
    Wait = WaitForSingleObject(Pending.Io.hEvent, 5000);
    CHECK(Wait == WAIT_OBJECT_0);
    /* Do not return with an outstanding request pointing into this frame. */
    if (Wait != WAIT_OBJECT_0) ExitProcess(1);
    CHECK(GetOverlappedResult(Read, &Pending.Io, &Bytes, FALSE) && Bytes == 4 && !memcmp(Buffer, "pipe", 4));
    CHECK(Pending.Guard[0] == 0x12345678 && Pending.Guard[1] == 0xabcdef01);
    CHECK(ResetEvent(Pending.Io.hEvent));
    CHECK(!ReadFile(Read, Buffer, sizeof(Buffer), &Bytes, &Pending.Io) && GetLastError() == ERROR_IO_PENDING);
    CHECK(CloseHandle(Write));
    Wait = WaitForSingleObject(Pending.Io.hEvent, 5000);
    CHECK(Wait == WAIT_OBJECT_0);
    if (Wait != WAIT_OBJECT_0) ExitProcess(1);
    CHECK(!GetOverlappedResult(Read, &Pending.Io, &Bytes, FALSE) && GetLastError() == ERROR_BROKEN_PIPE);
    CHECK(Pending.Guard[0] == 0x12345678 && Pending.Guard[1] == 0xabcdef01);
    CloseHandle(Pending.Io.hEvent);
    CloseHandle(Read);
}

static void CheckRename(void)
{
    WCHAR Temp[MAX_PATH], Source[MAX_PATH], Target[MAX_PATH];
    HANDLE File;
    DWORD Bytes, Value = 0x12345678, ReadBack = 0;
    CHECK(GetTempPathW(MAX_PATH, Temp));
    CHECK(GetTempFileNameW(Temp, L"w6s", 0, Source));
    CHECK(GetTempFileNameW(Temp, L"w6t", 0, Target));
    File = CreateFileW(Source, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(File != INVALID_HANDLE_VALUE);
    if (File != INVALID_HANDLE_VALUE)
    {
        CHECK(WriteFile(File, &Value, sizeof(Value), &Bytes, NULL) && Bytes == sizeof(Value));
        CHECK(FlushFileBuffers(File));
        CloseHandle(File);
    }
    CHECK(!MoveFileW(Source, Target));
    CHECK(GetFileAttributesW(Source) != INVALID_FILE_ATTRIBUTES);
    CHECK(MoveFileExW(Source, Target, MOVEFILE_REPLACE_EXISTING));
    CHECK(GetFileAttributesW(Source) == INVALID_FILE_ATTRIBUTES);
    File = CreateFileW(Target, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(File != INVALID_HANDLE_VALUE);
    if (File != INVALID_HANDLE_VALUE)
    {
        CHECK(ReadFile(File, &ReadBack, sizeof(ReadBack), &Bytes, NULL) && Bytes == sizeof(ReadBack));
        CHECK(ReadBack == Value);
        CloseHandle(File);
    }
    CHECK(MoveFileW(Target, Source));
    CHECK(GetFileAttributesW(Target) == INVALID_FILE_ATTRIBUTES);
    CHECK(DeleteFileW(Source));
    DeleteFileW(Target);
}

static void CheckAccess(HANDLE Token, PSECURITY_DESCRIPTOR Descriptor, BOOL Expected)
{
    GENERIC_MAPPING Mapping = {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    BYTE Privileges[1024];
    DWORD Bytes = sizeof(Privileges), Granted = 0xdeadbeef;
    BOOL Access = !Expected;
    CHECK(AccessCheck(Descriptor, Token, FILE_READ_DATA, &Mapping,
                     (PPRIVILEGE_SET)Privileges, &Bytes, &Granted, &Access));
    CHECK(Access == Expected);
    CHECK(Granted == (Expected ? FILE_READ_DATA : 0));
}

static void CheckPrivileges(void)
{
    HANDLE Token = NULL, Copy = NULL;
    TOKEN_PRIVILEGES Request = {1}, Previous;
    DWORD Returned = 0;
    TOKEN_INFORMATION_CLASS Class;
    LUID First = {0}, Second = {0};
    TOKEN_DEFAULT_DACL Dacl = {NULL};
    CHECK(AllocateLocallyUniqueId(&First));
    CHECK(AllocateLocallyUniqueId(&Second));
    CHECK(First.LowPart != Second.LowPart || First.HighPart != Second.HighPart);
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &Token));
    if (!Token) return;
    /* The well-known SE_CHANGE_NOTIFY_PRIVILEGE LUID avoids an unrelated
     * LSA name lookup in this test of the NT token adjustment contract. */
    Request.Privileges[0].Luid.LowPart = 23;
    Request.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    CHECK(AdjustTokenPrivileges(Token, FALSE, &Request, sizeof(Previous), &Previous, &Returned));
    CHECK(GetLastError() == ERROR_SUCCESS);
    CHECK(Returned >= sizeof(DWORD) && Returned <= sizeof(Previous));
    if (Returned >= sizeof(DWORD) && Returned <= sizeof(Previous))
        CHECK(AdjustTokenPrivileges(Token, FALSE, &Previous, 0, NULL, NULL));
    CloseHandle(Token);
    Token = NULL;
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &Token));
    if (!Token) return;
    CHECK(DuplicateTokenEx(Token, TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, NULL,
                           SecurityImpersonation, TokenImpersonation, &Copy));
    for (Class = TokenOwner; Class <= TokenPrimaryGroup; ++Class)
    {
        union { PSID Sid; BYTE Bytes[128]; } Info;
        DWORD Needed = 0;
        memset(&Info, 0xa5, sizeof(Info));
        CHECK(!GetTokenInformation(Token, Class, NULL, 0, &Needed) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
        CHECK(Needed > sizeof(PSID) && Needed < sizeof(Info));
        if (Needed <= sizeof(PSID) || Needed >= sizeof(Info)) continue;
        CHECK(GetTokenInformation(Token, Class, &Info, Needed, &Returned));
        CHECK(Returned == Needed && (BYTE *)Info.Sid >= Info.Bytes + sizeof(PSID) &&
              (BYTE *)Info.Sid < Info.Bytes + Needed && IsValidSid(Info.Sid));
        CHECK(Info.Bytes[Needed] == 0xa5);
        if (Copy) CHECK(SetTokenInformation(Copy, Class, &Info, sizeof(PSID)));
    }
    if (Copy)
    {
        CHECK(SetTokenInformation(Copy, TokenDefaultDacl, &Dacl, sizeof(Dacl)));
        CloseHandle(Copy);
    }
    CloseHandle(Token);
}

static void CheckObjectSecurity(void)
{
    HANDLE Event;
    SECURITY_DESCRIPTOR Descriptor;
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), &Descriptor, FALSE};
    ACL Empty;
    BYTE Relative[128], Queried[128];
    DWORD Bytes = sizeof(Relative);
    BOOL Present = FALSE, Defaulted = FALSE;
    PACL Dacl = (PACL)(ULONG_PTR)1;
    WCHAR Name[80];
    CHECK(InitializeSecurityDescriptor(&Descriptor, SECURITY_DESCRIPTOR_REVISION));
    CHECK(InitializeAcl(&Empty, sizeof(Empty), ACL_REVISION));
    CHECK(SetSecurityDescriptorDacl(&Descriptor, TRUE, &Empty, FALSE));
    _snwprintf(Name, 80, L"w643c-security-%lu", GetCurrentProcessId());
    Event = CreateEventW(&Attributes, FALSE, FALSE, Name);
    CHECK(Event != NULL);
    if (!Event) return;
    CHECK(SetSecurityDescriptorDacl(&Descriptor, TRUE, NULL, FALSE));
    CHECK(SetKernelObjectSecurity(Event, DACL_SECURITY_INFORMATION, &Descriptor));
    if (GetKernelObjectSecurity(Event, DACL_SECURITY_INFORMATION, Queried, sizeof(Queried), &Bytes))
    {
        CHECK(TRUE);
        CHECK(GetSecurityDescriptorDacl(Queried, &Present, &Dacl, &Defaulted) && Present && Dacl == NULL);
    }
    else CHECK(FALSE);
    Bytes = sizeof(Relative);
    CHECK(MakeSelfRelativeSD(&Descriptor, Relative, &Bytes));
    CHECK(SetKernelObjectSecurity(Event, DACL_SECURITY_INFORMATION, Relative));
    CloseHandle(Event);
}

static void CheckCurrentDirectory(void)
{
    typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } STRING;
    typedef struct { STRING Name; HANDLE Directory; PVOID Reference; } TEST_RELATIVE;
    typedef BOOLEAN (WINAPI *CONVERT)(PCWSTR, STRING *, PWSTR *, TEST_RELATIVE *);
    typedef VOID (WINAPI *RELEASE)(TEST_RELATIVE *);
    typedef VOID (WINAPI *FREE_STRING)(STRING *);
    typedef DWORD (WINAPI *FINALPATH)(HANDLE, LPWSTR, DWORD, DWORD);
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    CONVERT Convert = (CONVERT)GetProcAddress(Ntdll, "RtlDosPathNameToRelativeNtPathName_U");
    RELEASE Release = (RELEASE)GetProcAddress(Ntdll, "RtlReleaseRelativeName");
    FREE_STRING FreeString = (FREE_STRING)GetProcAddress(Ntdll, "RtlFreeUnicodeString");
    FINALPATH FinalPath = (FINALPATH)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFinalPathNameByHandleW");
    WCHAR Saved[MAX_PATH] = {0}, Temp[MAX_PATH] = {0}, First[MAX_PATH], Second[MAX_PATH], Path[MAX_PATH], Expected[MAX_PATH] = {0};
    STRING Name = {0};
    TEST_RELATIVE Relative = {{0}};
    CHECK(Convert && Release && FreeString && FinalPath);
    if (!Convert || !Release || !FreeString || !FinalPath) return;
    CHECK(GetCurrentDirectoryW(MAX_PATH, Saved) && GetTempPathW(MAX_PATH, Temp));
    if (!Saved[0] || !Temp[0]) return;
    _snwprintf(First, MAX_PATH, L"%lsw643c-cwd-%lu-a", Temp, GetCurrentProcessId());
    _snwprintf(Second, MAX_PATH, L"%lsw643c-cwd-%lu-b", Temp, GetCurrentProcessId());
    CHECK(CreateDirectoryW(First, NULL));
    CHECK(CreateDirectoryW(Second, NULL));
    CHECK(GetLongPathNameW(First, Expected, MAX_PATH));
    CHECK(SetCurrentDirectoryW(First));
    CHECK(Convert(L".\\", &Name, NULL, &Relative));
    CHECK(Relative.Directory && Relative.Reference);
    CHECK(SetCurrentDirectoryW(Second));
    CHECK(GetCurrentDirectoryW(MAX_PATH, Path) && !_wcsicmp(Path, Second));
    if (Relative.Directory)
    {
        DWORD Length = FinalPath(Relative.Directory, Path, MAX_PATH, 0);
        if (Length > 4 && Length < MAX_PATH)
        {
            SIZE_T Characters = wcslen(Path);
            if (Characters > 4 && Path[Characters - 1] == L'\\') Path[Characters - 1] = 0;
            printf("CWD REFERENCE length=%lu path=%ls\n", Length, Path);
        }
        CHECK(Length > 4 && Length < MAX_PATH && !_wcsicmp(Path + 4, Expected));
    }
    Release(&Relative);
    FreeString(&Name);
    CHECK(SetCurrentDirectoryW(Saved));
    CHECK(RemoveDirectoryW(First));
    CHECK(RemoveDirectoryW(Second));
}

static void CheckObjectDirectory(void)
{
    typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } STRING;
    typedef struct { ULONG Length; HANDLE Root; STRING *Name; ULONG Flags; PVOID Security, Qos; } ATTRIBUTES;
    typedef struct { STRING Name, Type; } ENTRY;
    typedef LONG (WINAPI *CREATE_DIRECTORY)(PHANDLE, ULONG, ATTRIBUTES *);
    typedef LONG (WINAPI *CREATE_LINK)(PHANDLE, ULONG, ATTRIBUTES *, STRING *);
    typedef LONG (WINAPI *QUERY_LINK)(HANDLE, STRING *, PULONG);
    typedef LONG (WINAPI *QUERY_DIRECTORY)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    CREATE_DIRECTORY CreateDirectory = (CREATE_DIRECTORY)GetProcAddress(Ntdll, "NtCreateDirectoryObject");
    CREATE_LINK CreateLink = (CREATE_LINK)GetProcAddress(Ntdll, "NtCreateSymbolicLinkObject");
    QUERY_LINK QueryLink = (QUERY_LINK)GetProcAddress(Ntdll, "NtQuerySymbolicLinkObject");
    QUERY_DIRECTORY QueryDirectory = (QUERY_DIRECTORY)GetProcAddress(Ntdll, "NtQueryDirectoryObject");
    WCHAR Path[100], TargetBuffer[80];
    STRING Name, Target = {24, 26, L"\\Device\\Null"}, Result = {0, sizeof(TargetBuffer), TargetBuffer};
    ATTRIBUTES Attributes = {sizeof(Attributes), NULL, &Name, 0x40, NULL, NULL};
    HANDLE Directory = NULL, Link = NULL;
    ULONG Needed = 0, Returned = 0, Context = 0, i;
    BYTE *Buffer;
    ENTRY *Entry;
    CHECK(CreateDirectory && CreateLink && QueryLink && QueryDirectory);
    if (!CreateDirectory || !CreateLink || !QueryLink || !QueryDirectory) return;
    _snwprintf(Path, 100, L"\\BaseNamedObjects\\w643c-directory-%lu", GetCurrentProcessId());
    Name.Buffer = Path; Name.Length = wcslen(Path) * sizeof(WCHAR); Name.MaximumLength = Name.Length + sizeof(WCHAR);
    CHECK(CreateDirectory(&Directory, 0xf000f, &Attributes) == 0);
    if (!Directory) return;
    CHECK(QueryDirectory(Directory, NULL, 0, TRUE, TRUE, &Context, &Needed) == (LONG)0x8000001a);
    Name.Buffer = L"link"; Name.Length = 8; Name.MaximumLength = 10; Attributes.Root = Directory;
    Target.Length = wcslen(Target.Buffer) * sizeof(WCHAR); Target.MaximumLength = Target.Length + sizeof(WCHAR);
    CHECK(CreateLink(&Link, 0xf0001, &Attributes, &Target) == 0);
    if (!Link) { CloseHandle(Directory); return; }
    CHECK(QueryLink(Link, &Result, &Returned) == 0 && Result.Length == Target.Length &&
          !memcmp(Result.Buffer, Target.Buffer, Target.Length));
    CHECK(QueryDirectory(Directory, NULL, 0, TRUE, TRUE, &Context, &Needed) == (LONG)0xc0000023);
    CHECK(Needed >= 2 * sizeof(ENTRY) && Needed < 1024);
    if (Needed >= 2 * sizeof(ENTRY) && Needed < 1024)
    {
        Buffer = HeapAlloc(GetProcessHeap(), 0, Needed + 8);
        CHECK(Buffer != NULL);
        if (Buffer)
        {
            memset(Buffer, 0xa5, Needed + 8);
            CHECK(QueryDirectory(Directory, Buffer, Needed - 1, TRUE, TRUE, &Context, &Returned) == (LONG)0xc0000023);
            CHECK(Returned == Needed);
            CHECK(QueryDirectory(Directory, Buffer, Needed, TRUE, TRUE, &Context, &Returned) == 0 && Returned == Needed);
            Entry = (ENTRY *)Buffer;
            for (i = 0; i < 2; ++i)
            {
                STRING *String = i ? &Entry->Type : &Entry->Name;
                CHECK((BYTE *)String->Buffer >= Buffer + 2 * sizeof(ENTRY) &&
                      (BYTE *)String->Buffer + String->MaximumLength <= Buffer + Needed);
            }
            CHECK(Entry->Name.Length == 8 && !wcsncmp(Entry->Name.Buffer, L"link", 4));
            CHECK(!wcscmp(Entry->Type.Buffer, L"SymbolicLink"));
            CHECK(Entry[1].Name.Buffer == NULL && Entry[1].Type.Buffer == NULL);
            CHECK(Buffer[Needed] == 0xa5 && Buffer[Needed + 7] == 0xa5);
            CHECK(QueryDirectory(Directory, Buffer, Needed, TRUE, FALSE, &Context, &Returned) == (LONG)0x8000001a);
            CHECK(QueryDirectory(Directory, Buffer, Needed, FALSE, TRUE, &Context, &Returned) == 0);
            CHECK(Entry[1].Name.Buffer == NULL && Entry[1].Type.Buffer == NULL);
            CHECK(Buffer[Needed] == 0xa5 && Buffer[Needed + 7] == 0xa5);
            HeapFree(GetProcessHeap(), 0, Buffer);
        }
    }
    CloseHandle(Link);
    CloseHandle(Directory);
}

int wmain(int argc, WCHAR **argv)
{
    typedef LONG (WINAPI *COMPARE)(PVOID, PVOID);
    COMPARE Compare = (COMPARE)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAreMappedFilesTheSame");
    WCHAR Path[MAX_PATH];
    HANDLE File, Mapping, Token = NULL, Duplicate = NULL;
    PVOID View;
    SECURITY_DESCRIPTOR Descriptor;
    SID_IDENTIFIER_AUTHORITY Authority = {SECURITY_WORLD_SID_AUTHORITY};
    PSID World = NULL;
    ACL Empty;
    BYTE Relative[1024];
    DWORD Bytes;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc > 1 && !wcscmp(argv[1], L"pipe-child"))
    {
        const char Data[] = "installer pipe";
        return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), Data, sizeof(Data), &Bytes, NULL) &&
               Bytes == sizeof(Data) ? 59 : 99;
    }

    CHECK(GetModuleFileNameW(NULL, Path, MAX_PATH));
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, 0, NULL);
    CHECK(File != INVALID_HANDLE_VALUE);
    if (File != INVALID_HANDLE_VALUE)
    {
        CheckObjectName(File, Path);
        Mapping = CreateFileMappingW(File, NULL, PAGE_READONLY, 0, 0, NULL);
        CHECK(Mapping != NULL);
        if (Mapping)
        {
            View = MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, 0);
            CHECK(View != NULL);
            if (View)
            {
                CHECK(Compare && Compare(GetModuleHandleW(NULL), View) == 0);
                UnmapViewOfFile(View);
            }
            CloseHandle(Mapping);
        }
        CloseHandle(File);
    }

    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &Token));
    if (Token)
    {
        CHECK(DuplicateTokenEx(Token, TOKEN_QUERY, NULL, SecurityImpersonation, TokenImpersonation, &Duplicate));
        if (Duplicate)
        {
            CHECK(AllocateAndInitializeSid(&Authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &World));
            if (World)
            {
                CHECK(InitializeSecurityDescriptor(&Descriptor, SECURITY_DESCRIPTOR_REVISION));
                CHECK(SetSecurityDescriptorOwner(&Descriptor, World, FALSE));
                CHECK(SetSecurityDescriptorGroup(&Descriptor, World, FALSE));
                CHECK(SetSecurityDescriptorDacl(&Descriptor, TRUE, NULL, FALSE));
                CheckAccess(Duplicate, &Descriptor, TRUE);
                Bytes = sizeof(Relative);
                CHECK(MakeSelfRelativeSD(&Descriptor, Relative, &Bytes));
                CheckAccess(Duplicate, Relative, TRUE);
                CHECK(InitializeAcl(&Empty, sizeof(Empty), ACL_REVISION));
                CHECK(SetSecurityDescriptorDacl(&Descriptor, TRUE, &Empty, FALSE));
                CheckAccess(Duplicate, &Descriptor, FALSE);
                Bytes = sizeof(Relative);
                CHECK(MakeSelfRelativeSD(&Descriptor, Relative, &Bytes));
                CheckAccess(Duplicate, Relative, FALSE);
                FreeSid(World);
            }
            CloseHandle(Duplicate);
        }
        CloseHandle(Token);
    }
    Duplicate = NULL;
    CHECK(!DuplicateTokenEx(NULL, TOKEN_QUERY, NULL, SecurityImpersonation, TokenImpersonation, &Duplicate));
    CHECK(GetLastError() == ERROR_INVALID_HANDLE);
    CheckRename();
    CheckPipe();
    CheckAsyncPipe();
    CheckVolumeByHandle();
    CheckPrivileges();
    CheckObjectSecurity();
    CheckCurrentDirectory();
    CheckObjectDirectory();
    printf("INSTALLER SERVICES checks=%u failures=%u\n", Checks, Failures);
    return Failures ? 1 : 0;
}
