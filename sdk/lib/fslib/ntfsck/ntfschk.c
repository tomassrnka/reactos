/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NtfsckRun: one check of an NTFS volume with the ntfsprogs-plus checker,
 *              reported in the form of the NTFS CHKDSK of Windows Server 2008 R2
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsck_ros.h"
#include <fslib/ntfsck.h>
#include "ntio.h"
#include "ntfsck_lib.h"
#include <pseh/pseh2.h>

#define NDEBUG
#include <debug.h>

#define NTFSCK_PRODUCT "ReactOS"

/* Switches that write to the volume: this version refuses them. */
#define NTFSCK_WRITE_FLAGS (NTFSCK_FLAG_FIX | NTFSCK_FLAG_SCAN | NTFSCK_FLAG_FORCE_DISMOUNT | \
                            NTFSCK_FLAG_REEVAL_BAD | NTFSCK_FLAG_SET_LOG_SIZE)

static PNTFSCK_PARAMS Run;
static struct ntfsck_lib_result Res;
static int Stage;                 /* the last stage whose header was printed */
static BOOLEAN StageClosed[4];
static ULONG ProblemLines;
static ULONG LastPercent;
static char LastProblem[1024];   /* the checker repeats some messages for every reference */
static ULONG LastRepeats;

static void FlushRepeats(void);

/*
 * The checker writes file names in UTF-8; fmifs callers take text in the ANSI code page
 * (chkdsk.exe prints it with %S). Decode UTF-8 (anything malformed becomes '?') and convert.
 */
static void Utf8ToAnsi(const char *In, char *Out, ULONG OutSize)
{
    WCHAR Wide[1024];
    ULONG n = 0, Bytes = 0;
    const unsigned char *p = (const unsigned char *)In;

    while (*p && n < ARRAYSIZE(Wide) - 2)
    {
        ULONG c = *p++, extra = 0, cp;
        if (c < 0x80)
        {
            Wide[n++] = (WCHAR)c;
            continue;
        }
        if ((c & 0xE0) == 0xC0)
        {
            extra = 1;
            cp = c & 0x1F;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            extra = 2;
            cp = c & 0x0F;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            extra = 3;
            cp = c & 0x07;
        }
        else
        {
            Wide[n++] = L'?';
            continue;
        }
        while (extra && (*p & 0xC0) == 0x80)
        {
            cp = (cp << 6) | (*p++ & 0x3F);
            extra--;
        }
        if (extra || cp > 0x10FFFF)
        {
            Wide[n++] = L'?';
        }
        else if (cp >= 0x10000)
        {
            cp -= 0x10000;
            Wide[n++] = (WCHAR)(0xD800 + (cp >> 10));
            Wide[n++] = (WCHAR)(0xDC00 + (cp & 0x3FF));
        }
        else
        {
            Wide[n++] = (WCHAR)cp;
        }
    }
    if (!NT_SUCCESS(RtlUnicodeToMultiByteN(Out, OutSize - 1, &Bytes, Wide, n * sizeof(WCHAR))))
        Bytes = 0;
    Out[Bytes] = '\0';
}

static void Out(const char *Format, ...)
{
    char Buffer[1024], Ansi[1024];
    TEXTOUTPUT Text;
    va_list Args;
    const char *p;

    va_start(Args, Format);
    ntfsck_vsnprintf(Buffer, sizeof(Buffer), Format, Args);
    va_end(Args);
    if (!Run->Callback)
        return;
    Text.Lines = 1;
    Text.Output = Buffer;
    for (p = Buffer; *p; p++)
    {
        if ((unsigned char)*p >= 0x80)
        {
            Utf8ToAnsi(Buffer, Ansi, sizeof(Ansi));
            Text.Output = Ansi;
            break;
        }
    }
    Run->Callback(OUTPUT, 0, &Text);
}

static void Progress(ULONG Percent)
{
    if (!Run->Callback || Percent == LastPercent)
        return;
    LastPercent = Percent;
    Run->Callback(PROGRESS, 0, &Percent);
}

static unsigned long long Kb(unsigned long long Clusters)
{
    return Clusters * Res.cluster_size / 1024;
}

static void StartStage1(void)
{
    if (Stage >= 1)
        return;
    Stage = 1;
    Out("CHKDSK is verifying files (stage 1 of 3)...\n");
    Progress(0);
}

static void EndStage1(void)
{
    ULONG i;

    FlushRepeats();
    StartStage1();
    if (StageClosed[1])
        return;
    StageClosed[1] = TRUE;
    for (i = 0; i < Res.bad_records && i < ARRAYSIZE(Res.bad_record_numbers); i++)
        Out("File record segment %I64u is corrupt.\n", Res.bad_record_numbers[i]);
    Out("  %I64u file records processed.\n", Res.mft_records);
    Out("File verification completed.\n");
    Out("  %I64u large file records processed.\n", Res.large_records);
    Out("  %I64u bad file records processed.\n", Res.bad_records);
    Out("  %I64u EA records processed.\n", Res.ea_records);
    Out("  %I64u reparse records processed.\n", Res.reparse_records);
    Progress(10);
}

static void StartStage2(void)
{
    EndStage1();
    if (Stage >= 2)
        return;
    Stage = 2;
    Out("CHKDSK is verifying indexes (stage 2 of 3)...\n");
}

static void EndStage2(void)
{
    FlushRepeats();
    StartStage2();
    if (StageClosed[2])
        return;
    StageClosed[2] = TRUE;
    /*
     * The entry count Windows prints here is its own progress measure: on every volume of the
     * calibration it is the record count plus twice the index count, less four.
     */
    Out("  %I64u index entries processed.\n",
        Res.mft_records + 2 * Res.indexes - (Res.indexes >= 2 ? 4 : 0));
    Out("Index verification completed.\n");
    Out("  %I64u unindexed files scanned.\n", Res.orphans);
    Out("  0 unindexed files recovered.\n");
    Progress(77);
}

static void Stage3(void)
{
    EndStage2();
    if (Stage >= 3)
        return;
    Stage = 3;
    Out("CHKDSK is verifying security descriptors (stage 3 of 3)...\n");
    Out("  %I64u file SDs/SIDs processed.\n", Res.mft_records);
    Out("Security descriptor verification completed.\n");
    /* As Windows counts it on every calibration volume: the index count less two. */
    Out("  %I64u data files processed.\n", Res.indexes >= 2 ? Res.indexes - 2 : 0);
    Progress(100);
}

static void FlushRepeats(void)
{
    if (LastRepeats)
        Out("  (the message above %lu more time%s)\n", LastRepeats, LastRepeats == 1 ? "" : "s");
    LastRepeats = 0;
    LastProblem[0] = '\0';
}

static BOOLEAN StartsWith(const char *Text, const char *Prefix)
{
    return !strncmp(Text, Prefix, strlen(Prefix));
}

/* A step of the checker ("Parse #2: Scan mft entries in volume. 100% completed") */
static void Step(const char *Name)
{
    if (StartsWith(Name, "Scan mft entries"))
        StartStage1();
    else if (StartsWith(Name, "Check system files"))
        StartStage1();
    else if (StartsWith(Name, "Check index entries"))
        StartStage2();
}

void NtfsckLine(int Stream, const char *Text)
{
    char Line[1024];
    size_t Len;
    const char *p;

    if (StartsWith(Text, "Parse #"))
    {
        p = strchr(Text, ':');
        if (p)
            Step(p + 2 <= Text + strlen(Text) ? p + 2 : p + 1);
        return;
    }
    p = Text;
    while (*p == ' ')
        p++;
    /* Progress tails and the checker's own verdicts: the report gives its own. */
    if (StartsWith(p, "100% completed") || StartsWith(p, "Check volume:") ||
        StartsWith(p, "Clean, No errors found") || strstr(p, " errors left (errors:"))
    {
        return;
    }

    /*
     * A read-only check answers each repair question of the checker with No: say that
     * instead ("Remove them from their parents. Fix it? No").
     */
    strncpy(Line, Text, sizeof(Line) - 32);
    Line[sizeof(Line) - 32] = '\0';
    Len = strlen(Line);
    while (Len && Line[Len - 1] == ' ')
        Line[--Len] = '\0';
    if (!strcmp(Line, "No") || !Len)
        return;
    if (Len >= 4 && !strcmp(Line + Len - 4, "? No"))
    {
        Line[Len -= 3] = '\0';
        if (Len >= 7 && !strcmp(Line + Len - 7, "Fix it?"))
            Line[Len -= 7] = '\0';
        while (Len && Line[Len - 1] == ' ')
            Line[--Len] = '\0';
        strcat(Line, " (not done: read-only check)");
    }

    if (Stream == 1)
    {
        ProblemLines++;
        if (!strcmp(Line, LastProblem))
        {
            LastRepeats++;
            return;
        }
        FlushRepeats();
        strcpy(LastProblem, Line);
        Out("%s\n", Line);
    }
    else if (Run->Flags & NTFSCK_FLAG_VERBOSE)
    {
        Out("%s\n", Line);
    }
}

static void Summary(void)
{
    unsigned long long TotalKb = Res.total_sectors * Res.bytes_per_sector / 1024;
    unsigned long long FreeKb = Kb(Res.free_clusters);
    unsigned long long FilesKb = Kb(Res.files_clusters), IndexKb = Kb(Res.index_clusters);
    unsigned long long BadKb = Kb(Res.bad_clusters);
    unsigned long long SystemKb = TotalKb - FreeKb - FilesKb - IndexKb - BadKb;

    if (TotalKb < FreeKb + FilesKb + IndexKb + BadKb)
        SystemKb = 0;
    Out("\n");
    Out("%10I64u KB total disk space.\n", TotalKb);
    Out("%10I64u KB in %I64u files.\n", FilesKb, Res.files);
    Out("%10I64u KB in %I64u indexes.\n", IndexKb, Res.indexes);
    Out("%10I64u KB in bad sectors.\n", BadKb);
    Out("%10I64u KB in use by the system.\n", SystemKb);
    Out("%10I64u KB occupied by the log file.\n", Res.log_bytes / 1024);
    Out("%10I64u KB available on disk.\n", FreeKb);
    Out("\n");
    Out("%10u bytes in each allocation unit.\n", Res.cluster_size);
    Out("%10I64u total allocation units on disk.\n", Res.total_clusters);
    Out("%10I64u allocation units available on disk.\n", Res.free_clusters);
}

static BOOLEAN Finish(ULONG ExitCode, NTSTATUS Status, BOOLEAN Ran)
{
    Run->ExitCode = ExitCode;
    Run->Status = Status;
    return Ran;
}

static BOOLEAN JournalPending(void)
{
    return Res.log_state == NTFSCK_LOG_ACTIVE || Res.log_state == NTFSCK_LOG_NG_COMMITTED;
}

/* The volume path as the checker takes it; freed by NtfsckRun, also after an exception. */
static ANSI_STRING Path;

static BOOLEAN DoRun(PNTFSCK_PARAMS Params)
{
    struct ntfsck_lib_opts Opts;
    NTSTATUS Status;
    int Ret;

    if (Params->Flags & NTFSCK_WRITE_FLAGS)
    {
        Out("This version of CHKDSK cannot fix NTFS volumes: /F, /R, /X, /B and /L:size\n"
            "are not available. Run CHKDSK without them to check the volume read-only.\n");
        return Finish(3, STATUS_NOT_SUPPORTED, FALSE);
    }

    Status = RtlUnicodeStringToAnsiString(&Path, (PUNICODE_STRING)Params->Volume, TRUE);
    if (!NT_SUCCESS(Status))
        return Finish(3, Status, FALSE);

    memset(&Opts, 0, sizeof(Opts));
    Opts.verbose = (Params->Flags & NTFSCK_FLAG_VERBOSE) ? 1 : 0;
    Opts.dirty_only = (Params->Flags & NTFSCK_FLAG_DIRTY_ONLY) ? 1 : 0;
    /* /L alone: the log file size, no check (as Windows CHKDSK does) */
    if (Params->Flags & NTFSCK_FLAG_LOG_SIZE)
        Opts.dirty_only = Opts.info_only = 1;
    Opts.windows_tolerance = 1;
    /*
     * A read-only check does not lock the volume, as Windows CHKDSK does not without /F: a
     * lock would refuse every other open for the length of the check (programs writing to the
     * volume would fail). The file system writes its cached changes back first; on a volume in
     * use, changes made during the check can still show as problems, as on Windows.
     */
    NtioPolicy.lock_mode = NTIO_LOCK_NONE;
    NtioPolicy.flush_first = !Opts.dirty_only;
    NtioPolicy.dismount_on_close = 0;

    Ret = ntfsck_lib_run(Path.Buffer, &Opts, &Res);
    NtfsckFlushStreams();
    FlushRepeats();
    if (ntio_close_all())
        DPRINT1("ntfsck: the checker left the volume open; closed\n");
    if (NtioState.writes_refused)
        DPRINT1("ntfsck: %d writes refused on the read-only volume\n", NtioState.writes_refused);

    Params->VolumeDirty = Res.volume_dirty ? TRUE : FALSE;

    if (Opts.info_only)
    {
        if (Ret == NTFSCK_RET_OPERATIONAL_ERROR || Res.mount_errno || Res.not_ntfs ||
            Res.log_state == NTFSCK_LOG_UNKNOWN || !Res.log_bytes)
        {
            Out("Unable to determine volume version and state.  CHKDSK aborted.\n");
            return Finish(3, STATUS_DISK_CORRUPT_ERROR, FALSE);
        }
        Out("The current log file size is %I64u KB.\n", Res.log_bytes / 1024);
        return Finish(0, STATUS_SUCCESS, TRUE);
    }
    if (Opts.dirty_only)
    {
        if (Ret == NTFSCK_RET_OPERATIONAL_ERROR || Res.mount_errno)
            return Finish(3, STATUS_DISK_CORRUPT_ERROR, FALSE);
        if (Res.volume_dirty)
            return Finish(3, STATUS_DISK_CORRUPT_ERROR, FALSE);
        return Finish(0, STATUS_SUCCESS, TRUE);
    }

    if (Params->Flags & NTFSCK_FLAG_VERBOSE)
    {
        Out("Volume access: opened %d, size %I64d, sector %d, read %I64u bytes, flush 0x%08x, open 0x%08x.\n",
            NtioState.opened, NtioState.size, NtioState.sector_size, NtioState.bytes_read,
            NtioState.flush_status, NtioState.open_status);
    }
    if (Res.not_ntfs)
    {
        Out("The volume does not contain a recognized NTFS file system.\n");
        return Finish(3, STATUS_UNRECOGNIZED_VOLUME, FALSE);
    }
    if (Res.mount_errno)
    {
        if (!NtioState.opened && NtioState.open_status)
        {
            Out("Cannot open volume for direct access.\n");
            return Finish(3, NtioState.open_status, FALSE);
        }
        Out("Unable to determine volume version and state.  CHKDSK aborted.\n");
        return Finish(3, STATUS_DISK_CORRUPT_ERROR, FALSE);
    }

    if (Res.aborted)
    {
        Out("CHKDSK could not complete the check of this volume (a check step failed;\n"
            "the messages above say where). The volume was not changed.\n");
        return Finish(3, STATUS_DISK_CORRUPT_ERROR, FALSE);
    }

    Stage3();
    /* The two structures Windows names when they are wrong (its messages) */
    if (Res.lcn_bitmap_blocks)
        Out("The Volume Bitmap is incorrect.\n");
    if (Res.mft_bitmap_blocks)
        Out("The master file table's (MFT) BITMAP attribute is incorrect.\n");
    if ((Res.errors_left || Ret == NTFSCK_RET_LEFT_UNCORRECTED) && Res.ng_seq_start != Res.ng_seq_end)
    {
        Out("The volume was written to while it was checked: a problem reported here may be\n"
            "a change that was still being written. Run CHKDSK again when the volume is idle.\n");
    }
    /* The checker only notes an inconsistent log (it would empty it in a repair): it is a problem. */
    if (Res.log_state == NTFSCK_LOG_UNREADABLE)
        Out("The NTFS log file ($LogFile) is corrupt or cannot be read.\n");
    if (JournalPending())
    {
        Out("The NTFS log file holds changes that were not applied to the volume: the\n"
            "volume was not recovered after the system stopped. Problems reported here\n"
            "may be ones that the log would repair.\n");
    }
    if (Res.errors_left || Ret == NTFSCK_RET_LEFT_UNCORRECTED || Ret == NTFSCK_RET_OPERATIONAL_ERROR ||
        Res.log_state == NTFSCK_LOG_UNREADABLE)
    {
        Out("%s found problems with the file system.\n", NTFSCK_PRODUCT);
        Out("Run CHKDSK with the /F (fix) option to correct these.\n");
        Summary();
        return Finish(3, STATUS_DISK_CORRUPT_ERROR, TRUE);
    }
    Out("%s has checked the file system and found no problems.\n", NTFSCK_PRODUCT);
    Summary();
    return Finish(0, STATUS_SUCCESS, TRUE);
}

/*
 * The checker keeps its state in globals that only loading the image initializes. A caller
 * loads ntfsck.dll for one check and unloads it after (and runs one check at a time); a
 * second check in the same load is refused.
 */
static LONG ImageUsed;

BOOLEAN NTAPI NtfsckRun(PNTFSCK_PARAMS Params)
{
    BOOLEAN Ran;

    if (!Params || Params->Size < sizeof(NTFSCK_PARAMS) || Params->Version != NTFSCK_PARAMS_VERSION ||
        !Params->Volume)
    {
        return FALSE;
    }
    Params->ExitCode = 3;
    Params->Status = STATUS_UNSUCCESSFUL;
    Params->VolumeDirty = FALSE;

    if (InterlockedExchange(&ImageUsed, 1))
    {
        Params->Status = STATUS_INVALID_DEVICE_STATE;
        return FALSE;
    }

    NtfsckHeap = RtlCreateHeap(HEAP_GROWABLE, NULL, 0, 0, NULL, NULL);
    if (!NtfsckHeap)
    {
        Params->Status = STATUS_NO_MEMORY;
        return FALSE;
    }
    Run = Params;
    memset(&Res, 0, sizeof(Res));
    Stage = 0;
    memset(StageClosed, 0, sizeof(StageClosed));
    ProblemLines = 0;
    LastPercent = ~0UL;
    LastProblem[0] = '\0';
    LastRepeats = 0;
    memset(&NtioState, 0, sizeof(NtioState));
    RtlInitEmptyAnsiString(&Path, NULL, 0);
    NtfsckResetStreams();

    /*
     * The checker reads damaged volumes: should it fault anyway, the caller (chkdsk.exe,
     * Explorer) must not die with it. The image is unloaded after the check.
     */
    _SEH2_TRY
    {
        Ran = DoRun(Params);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        NTSTATUS Code = _SEH2_GetExceptionCode();
        ntio_close_all();
        Out("CHKDSK stopped: the NTFS checker failed with exception 0x%08lx.\n", Code);
        Ran = Finish(3, Code, FALSE);
    }
    _SEH2_END;

    if (Path.Buffer)
        RtlFreeAnsiString(&Path);
    RtlInitEmptyAnsiString(&Path, NULL, 0);
    Run = NULL;
    RtlDestroyHeap(NtfsckHeap);
    NtfsckHeap = NULL;
    return Ran;
}
