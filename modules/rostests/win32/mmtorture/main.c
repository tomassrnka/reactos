/*
 * PROJECT:     ReactOS memory manager torture suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Entry point: one program, one workload per command
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "mmtorture.h"

static LONG WINAPI
QuietFilter(PEXCEPTION_POINTERS Pointers)
{
    return EXCEPTION_EXECUTE_HANDLER;
}

static VOID
Usage(VOID)
{
    printf("mmtorture run SECONDS STALL_SECONDS \"WORKER ARGS\"...  orchestrate and monitor\n"
           "mmtorture pressure PRIVATE_MB SECTION_MB THREADS         (a) memory pressure\n"
           "mmtorture procs SPAWNERS                                 (b) short-lived processes and threads\n"
           "mmtorture mapping DIR FILE_MB THREADS [LONG_VIEW_MB]      (c) data and image file mappings\n"
           "mmtorture vmapi THREADS                                  (d) virtual memory API churn\n"
           "mmtorture pool DIR THREADS [net]                         (e) kernel pool churn\n"
           "mmtorture probe                                          single-shot behaviour probes\n"
           "A worker given as \"!PROGRAM ARGS\" to run is started as is.\n");
}

int
main(int argc, char **argv)
{
    const char *Command = argc > 1 ? argv[1] : "";

    MmtSetupProcess();
    if (!strcmp(Command, "run"))
        return MmtMonitorMain(argc, argv);
    if (!strcmp(Command, "pressure"))
        return MmtPressureMain(argc, argv);
    if (!strcmp(Command, "procs"))
        return MmtProcsMain(argc, argv);
    if (!strcmp(Command, "child"))
    {
        /* The crash child is expected to die: no failure line for it */
        if (argc > 2 && !strcmp(argv[2], "crash"))
            SetUnhandledExceptionFilter(QuietFilter);
        return MmtChildMain(argc, argv);
    }
    if (!strcmp(Command, "mapping"))
        return MmtMappingMain(argc, argv);
    if (!strcmp(Command, "vmapi"))
        return MmtVmApiMain(argc, argv);
    if (!strcmp(Command, "pool"))
        return MmtPoolMain(argc, argv);
    if (!strcmp(Command, "probe"))
        return MmtProbeMain(argc, argv);
    Usage();
    return 2;
}
