/*
 * PROJECT:     Dr. Watson crash reporter
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Write the Application Error event of a crash
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <psapi.h>
#include <strsafe.h>
#include <rpc.h>
#include "drwtsnmsg.h"

static void GetFileVersionString(const std::wstring& Path, WCHAR* Buffer, size_t BufferCount)
{
    DWORD Handle, Size;
    VS_FIXEDFILEINFO* Info;
    UINT Length;

    StringCchCopyW(Buffer, BufferCount, L"0.0.0.0");
    Size = GetFileVersionInfoSizeW(Path.c_str(), &Handle);
    if (!Size)
        return;

    std::vector<BYTE> Data(Size);
    if (GetFileVersionInfoW(Path.c_str(), 0, Size, &Data[0]) &&
        VerQueryValueW(&Data[0], L"\\", (LPVOID*)&Info, &Length) && Length >= sizeof(*Info))
    {
        StringCchPrintfW(Buffer, BufferCount, L"%u.%u.%u.%u",
                         HIWORD(Info->dwFileVersionMS), LOWORD(Info->dwFileVersionMS),
                         HIWORD(Info->dwFileVersionLS), LOWORD(Info->dwFileVersionLS));
    }
}

/* The link time stamp of the image as mapped in the crashed process */
static DWORD GetImageTimeStamp(HANDLE hProcess, PVOID Base)
{
    IMAGE_DOS_HEADER DosHeader;
    struct
    {
        DWORD Signature;
        IMAGE_FILE_HEADER FileHeader;
    } NtHeaders;
    SIZE_T Read;

    if (!ReadProcessMemory(hProcess, Base, &DosHeader, sizeof(DosHeader), &Read) ||
        DosHeader.e_magic != IMAGE_DOS_SIGNATURE ||
        !ReadProcessMemory(hProcess, (PBYTE)Base + DosHeader.e_lfanew, &NtHeaders, sizeof(NtHeaders), &Read) ||
        NtHeaders.Signature != IMAGE_NT_SIGNATURE)
    {
        return 0;
    }
    return NtHeaders.FileHeader.TimeDateStamp;
}

static std::wstring GetFileName(const std::wstring& Path)
{
    std::wstring::size_type pos = Path.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? Path.substr(pos + 1) : Path;
}

/*
 * Event 1000 of the source "Application Error" in the Application log, with
 * the 13 insertion strings Windows 7 writes: program name, version and time
 * stamp; faulting module name, version and time stamp; exception code; fault
 * offset; process id; process start time; program path; module path; report id.
 */
void WriteApplicationErrorEvent(DumpData& data)
{
    WCHAR AppVersion[32], AppTimeStamp[16], ModuleVersion[32], ModuleTimeStamp[16];
    WCHAR Code[16], Offset[24], ProcessId[16], StartTime[24], ReportId[40];
    std::wstring ModulePath = L"unknown", ModuleName = L"unknown";
    ULONG_PTR Address = (ULONG_PTR)data.ExceptionInfo.ExceptionRecord.ExceptionAddress;
    ULONG_PTR ModuleOffset = Address;
    DWORD ModuleStamp = 0;
    HMODULE MainModule = NULL;
    DWORD Needed;

    GetFileVersionString(data.ProcessPath, AppVersion, _countof(AppVersion));
    if (EnumProcessModules(data.ProcessHandle, &MainModule, sizeof(MainModule), &Needed) && Needed)
        StringCchPrintfW(AppTimeStamp, _countof(AppTimeStamp), L"%08lx", GetImageTimeStamp(data.ProcessHandle, MainModule));
    else
        StringCchCopyW(AppTimeStamp, _countof(AppTimeStamp), L"00000000");

    /* Outside any image Windows names the module "unknown" and gives the address itself */
    MEMORY_BASIC_INFORMATION mbi;
    std::vector<WCHAR> Path(32768);
    DWORD Length = 0;
    if (VirtualQueryEx(data.ProcessHandle, (PVOID)Address, &mbi, sizeof(mbi)) == sizeof(mbi) &&
        mbi.Type == MEM_IMAGE)
    {
        Length = GetModuleFileNameExW(data.ProcessHandle, (HMODULE)mbi.AllocationBase, &Path[0], (DWORD)Path.size());
    }
    /* A full buffer may hold a cut path */
    if (Length && Length < Path.size() - 1)
    {
        ModulePath.assign(&Path[0], Length);
        ModuleName = GetFileName(ModulePath);
        ModuleOffset = Address - (ULONG_PTR)mbi.AllocationBase;
        ModuleStamp = GetImageTimeStamp(data.ProcessHandle, mbi.AllocationBase);
        GetFileVersionString(ModulePath, ModuleVersion, _countof(ModuleVersion));
    }
    else
    {
        StringCchCopyW(ModuleVersion, _countof(ModuleVersion), L"0.0.0.0");
    }
    StringCchPrintfW(ModuleTimeStamp, _countof(ModuleTimeStamp), L"%08lx", ModuleStamp);

    StringCchPrintfW(Code, _countof(Code), L"%08lx", data.ExceptionInfo.ExceptionRecord.ExceptionCode);
#ifdef _WIN64
    StringCchPrintfW(Offset, _countof(Offset), L"%016I64x", (ULONG64)ModuleOffset);
#else
    StringCchPrintfW(Offset, _countof(Offset), L"%08lx", (ULONG)ModuleOffset);
#endif
    StringCchPrintfW(ProcessId, _countof(ProcessId), L"%lx", data.ProcessID);

    FILETIME Creation, Exit, Kernel, User;
    ULONGLONG Start = 0;
    if (GetProcessTimes(data.ProcessHandle, &Creation, &Exit, &Kernel, &User))
        Start = ((ULONGLONG)Creation.dwHighDateTime << 32) | Creation.dwLowDateTime;
    StringCchPrintfW(StartTime, _countof(StartTime), L"%016I64x", Start);

    UUID Uuid;
    if (UuidCreate(&Uuid) != RPC_S_OK)
        ZeroMemory(&Uuid, sizeof(Uuid));
    StringCchPrintfW(ReportId, _countof(ReportId), L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     Uuid.Data1, Uuid.Data2, Uuid.Data3, Uuid.Data4[0], Uuid.Data4[1], Uuid.Data4[2],
                     Uuid.Data4[3], Uuid.Data4[4], Uuid.Data4[5], Uuid.Data4[6], Uuid.Data4[7]);

    LPCWSTR Strings[13] =
    {
        data.ProcessName.c_str(), AppVersion, AppTimeStamp,
        ModuleName.c_str(), ModuleVersion, ModuleTimeStamp,
        Code, Offset, ProcessId, StartTime,
        data.ProcessPath.c_str(), ModulePath.c_str(), ReportId
    };

    HANDLE hEventLog = RegisterEventSourceW(NULL, L"Application Error");
    if (!hEventLog)
        return;
    ReportEventW(hEventLog, EVENTLOG_ERROR_TYPE, CATEGORY_APPLICATION_CRASH, MSG_APPLICATION_ERROR,
                 NULL, _countof(Strings), 0, Strings, NULL);
    DeregisterEventSource(hEventLog);
}
