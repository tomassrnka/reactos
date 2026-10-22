/* NtPowerInformation from a 32-bit process: the fixed-size records without pointers
 * (battery state, power capabilities, processor information, execution state) and
 * GetSystemPowerStatus. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"

typedef struct { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; } PROCESSOR_POWER32;

/* Size: the record size for all processors; Short: a length that is too short (one record
 * less one byte: the ReactOS kernel fills ProcessorInformation for processor 0 only and
 * accepts any length that holds one record, also for 64-bit callers) */
static void Query(POWER_INFORMATION_LEVEL Level, ULONG Size, ULONG Short, const char *Name)
{
    UCHAR Input[16], *Buffer;
    ULONG BufferSize = Size + 128, i;
    NTSTATUS Status;

    Buffer = HeapAlloc(GetProcessHeap(), 0, BufferSize);
    ok(Buffer != NULL, "%s: no memory\n", Name);
    if (!Buffer) return;
    memset(Buffer, 0x5a, BufferSize);
    Status = NtPowerInformation(Level, NULL, 0, Buffer, Size);
    ok_ntstatus(Status, STATUS_SUCCESS);
    for (i = Size; i < BufferSize && Buffer[i] == 0x5a; ++i);
    ok(i == BufferSize, "%s: wrote past %lu bytes, at %lu\n", Name, Size, i);
    for (i = 0; i < Size && Buffer[i] == 0x5a; ++i);
    ok(i < Size, "%s: nothing written\n", Name);

    Status = NtPowerInformation(Level, NULL, 0, Buffer, Short);
    ok(Status == STATUS_BUFFER_TOO_SMALL, "%s short: 0x%lx\n", Name, Status);
    memset(Buffer, 0x5a, BufferSize);
    Status = NtPowerInformation(Level, NULL, 0, Buffer, Size + 64);
    ok(Status == STATUS_SUCCESS, "%s long: 0x%lx\n", Name, Status);
    for (i = Size; i < BufferSize && Buffer[i] == 0x5a; ++i);
    ok(i == BufferSize, "%s long: wrote past %lu bytes, at %lu\n", Name, Size, i);
    Status = NtPowerInformation(Level, NULL, 0, (PVOID)0x10, Size);
    ok(Status == STATUS_ACCESS_VIOLATION, "%s bad buffer: 0x%lx\n", Name, Status);
    /* A NULL output and an input buffer fail; the status differs between Windows versions */
    Status = NtPowerInformation(Level, NULL, 0, NULL, 0);
    ok(!NT_SUCCESS(Status), "%s null: 0x%lx\n", Name, Status);
    Status = NtPowerInformation(Level, Input, sizeof(Input), Buffer, Size);
    ok(!NT_SUCCESS(Status), "%s input: 0x%lx\n", Name, Status);
    HeapFree(GetProcessHeap(), 0, Buffer);
}

START_TEST(Wow64PowerInformation)
{
    SYSTEM_BATTERY_STATE Battery;
    SYSTEM_POWER_STATUS PowerStatus;
    PROCESSOR_POWER32 Processors[64];
    SYSTEM_INFO Info;
    EXECUTION_STATE State;
    NTSTATUS Status;
    BOOL Ret;

    C_ASSERT(sizeof(SYSTEM_BATTERY_STATE) == 32);
    C_ASSERT(sizeof(SYSTEM_POWER_CAPABILITIES) == 76);
    C_ASSERT(sizeof(PROCESSOR_POWER32) == 24);

    Query(SystemBatteryState, sizeof(SYSTEM_BATTERY_STATE), sizeof(SYSTEM_BATTERY_STATE) - 1, "SystemBatteryState");
    Query(SystemPowerCapabilities, sizeof(SYSTEM_POWER_CAPABILITIES), sizeof(SYSTEM_POWER_CAPABILITIES) - 1,
          "SystemPowerCapabilities");
    Query(SystemExecutionState, sizeof(EXECUTION_STATE), sizeof(EXECUTION_STATE) - 1, "SystemExecutionState");
    GetSystemInfo(&Info);
    ok(Info.dwNumberOfProcessors <= ARRAYSIZE(Processors), "%lu processors\n", Info.dwNumberOfProcessors);
    if (Info.dwNumberOfProcessors > ARRAYSIZE(Processors)) return;
    Query(ProcessorInformation, Info.dwNumberOfProcessors * sizeof(PROCESSOR_POWER32), sizeof(PROCESSOR_POWER32) - 1,
          "ProcessorInformation");

    memset(&Battery, 0x5a, sizeof(Battery));
    Status = NtPowerInformation(SystemBatteryState, NULL, 0, &Battery, sizeof(Battery));
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(Battery.AcOnLine == TRUE || Battery.BatteryPresent, "No battery and not on AC power\n");
    memset(Processors, 0x5a, sizeof(Processors));
    Status = NtPowerInformation(ProcessorInformation, NULL, 0, Processors, Info.dwNumberOfProcessors * sizeof(Processors[0]));
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(Processors[0].Number == 0, "First processor number %lu\n", Processors[0].Number);
    ok(Processors[0].CurrentMhz != 0x5a5a5a5a, "Processor speed not written\n");
    Status = NtPowerInformation(SystemExecutionState, NULL, 0, &State, sizeof(State));
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(!(State & ~(ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED | ES_USER_PRESENT | ES_AWAYMODE_REQUIRED | ES_CONTINUOUS)),
       "Execution state %lx\n", State);

    Ret = GetSystemPowerStatus(&PowerStatus);
    ok(Ret, "GetSystemPowerStatus failed: %lu\n", GetLastError());
    if (Ret && !Battery.BatteryPresent)
        ok(PowerStatus.ACLineStatus == AC_LINE_ONLINE, "AC line status %u\n", PowerStatus.ACLineStatus);
}
