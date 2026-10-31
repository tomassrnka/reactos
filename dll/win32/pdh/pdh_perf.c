/*
 * Performance Data Helper (pdh.dll): built-in counter sets and their providers.
 *
 * The counter sets are the ones that Windows publishes for the system objects
 * (Memory, Processor, System, PhysicalDisk, Network Interface, Process); their
 * names, name indices and counter types follow Windows. Values come from
 * NtQuerySystemInformation, the disk class driver and the IP helper.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winnls.h"
#include "winioctl.h"
#include "winternl.h"
#include "ntddstor.h"
#include "iphlpapi.h"

#include "pdh.h"
#include "pdhmsg.h"
#include "winperf.h"

#include "wine/debug.h"

#include "pdh_perf.h"

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

WINE_DEFAULT_DEBUG_CHANNEL(pdh);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#include "pdh_tables.h"

static CRITICAL_SECTION perf_cs;
static CRITICAL_SECTION_DEBUG perf_cs_debug =
{
    0, 0, &perf_cs,
    { &perf_cs_debug.ProcessLocksList, &perf_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": perf_cs") }
};
static CRITICAL_SECTION perf_cs = { &perf_cs_debug, -1, 0, 0, 0, 0 };

/* Layouts of the NtQuerySystemInformation records (the public header only has opaque ones). */

struct pdh_spi
{
    LARGE_INTEGER IdleProcessTime;
    LARGE_INTEGER IoReadTransferCount;
    LARGE_INTEGER IoWriteTransferCount;
    LARGE_INTEGER IoOtherTransferCount;
    ULONG IoReadOperationCount;
    ULONG IoWriteOperationCount;
    ULONG IoOtherOperationCount;
    ULONG AvailablePages;
    ULONG CommittedPages;
    ULONG CommitLimit;
    ULONG PeakCommitment;
    ULONG PageFaultCount;
    ULONG CopyOnWriteCount;
    ULONG TransitionCount;
    ULONG CacheTransitionCount;
    ULONG DemandZeroCount;
    ULONG PageReadCount;
    ULONG PageReadIoCount;
    ULONG CacheReadCount;
    ULONG CacheIoCount;
    ULONG DirtyPagesWriteCount;
    ULONG DirtyWriteIoCount;
    ULONG MappedPagesWriteCount;
    ULONG MappedWriteIoCount;
    ULONG PagedPoolPages;
    ULONG NonPagedPoolPages;
    ULONG PagedPoolAllocs;
    ULONG PagedPoolFrees;
    ULONG NonPagedPoolAllocs;
    ULONG NonPagedPoolFrees;
    ULONG FreeSystemPtes;
    ULONG ResidentSystemCodePage;
    ULONG TotalSystemDriverPages;
    ULONG TotalSystemCodePages;
    ULONG NonPagedPoolLookasideHits;
    ULONG PagedPoolLookasideHits;
    ULONG Spare3Count;
    ULONG ResidentSystemCachePage;
    ULONG ResidentPagedPoolPage;
    ULONG ResidentSystemDriverPage;
    ULONG Cc[29];
    ULONG ContextSwitches;
    ULONG FirstLevelTbFills;
    ULONG SecondLevelTbFills;
    ULONG SystemCalls;
    ULONGLONG Tail[8];  /* newer systems append more; the buffer is larger than the record we use */
};

struct pdh_cpu
{
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
};

struct pdh_vm_counters
{
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
};

struct pdh_process
{
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    struct pdh_vm_counters vm;
};

#define PDH_SYSINFO_PERFORMANCE   2
#define PDH_SYSINFO_PROCESS       5
#define PDH_SYSINFO_CPU_PERF      8

static NTSTATUS query_sysinfo( ULONG class, void *buf, ULONG size, ULONG *ret )
{
    return NtQuerySystemInformation( (SYSTEM_INFORMATION_CLASS)class, buf, size, ret );
}

/* ---- snapshots ----
 * A snapshot of each data source is taken once per collection ("generation"), so that all the
 * counters of one PdhCollectQueryData see the same numbers. */

static DWORD cur_gen = 1;

static struct { DWORD gen; BOOL ok; struct pdh_spi spi; } snap_spi;
static struct { DWORD gen; DWORD count; struct pdh_cpu *cpu; } snap_cpu;

/* the record of IOCTL_DISK_PERFORMANCE */
struct pdh_disk_perf
{
    LARGE_INTEGER BytesRead;
    LARGE_INTEGER BytesWritten;
    LARGE_INTEGER ReadTime;
    LARGE_INTEGER WriteTime;
    LARGE_INTEGER IdleTime;
    ULONG ReadCount;
    ULONG WriteCount;
    ULONG QueueDepth;
    ULONG SplitCount;
    LARGE_INTEGER QueryTime;
    ULONG StorageDeviceNumber;
    WCHAR StorageManagerName[8];
};

struct disk_info
{
    WCHAR name[64];
    struct pdh_disk_perf perf;
    BOOL have_perf;
};
static struct { DWORD gen; DWORD count; struct disk_info *disk; } snap_disk;

struct net_info
{
    WCHAR name[128];
    MIB_IFROW row;
};
static struct { DWORD gen; DWORD count; struct net_info *net; } snap_net;

static struct
{
    DWORD gen;
    DWORD count, threads;
    BYTE *buf;
    struct pdh_process **proc;
    WCHAR **name;
} snap_proc;

LONGLONG perf_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime( &ft );
    return ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

void perf_next_generation(void)
{
    EnterCriticalSection( &perf_cs );
    cur_gen++;
    LeaveCriticalSection( &perf_cs );
}

static void free_snap_proc(void)
{
    DWORD i;
    for (i = 0; i < snap_proc.count; i++) free( snap_proc.name[i] );
    free( snap_proc.name );
    free( snap_proc.proc );
    free( snap_proc.buf );
    memset( &snap_proc, 0, sizeof(snap_proc) );
}

static BOOL ensure_spi( BOOL force )
{
    ULONG len;

    if (!force && snap_spi.gen == cur_gen) return snap_spi.ok;
    memset( &snap_spi.spi, 0, sizeof(snap_spi.spi) );
    snap_spi.ok = query_sysinfo( PDH_SYSINFO_PERFORMANCE, &snap_spi.spi, sizeof(snap_spi.spi), &len ) >= 0;
    snap_spi.gen = cur_gen;
    return snap_spi.ok;
}

static DWORD cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo( &si );
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
}

static BOOL ensure_cpu( BOOL force )
{
    DWORD n;
    ULONG len;

    if (!force && snap_cpu.gen == cur_gen) return snap_cpu.cpu != NULL;
    free( snap_cpu.cpu );
    snap_cpu.cpu = NULL;
    snap_cpu.count = 0;
    snap_cpu.gen = cur_gen;
    n = cpu_count();
    if (!(snap_cpu.cpu = calloc( n, sizeof(*snap_cpu.cpu) ))) return FALSE;
    if (query_sysinfo( PDH_SYSINFO_CPU_PERF, snap_cpu.cpu, n * sizeof(*snap_cpu.cpu), &len ) < 0)
    {
        free( snap_cpu.cpu );
        snap_cpu.cpu = NULL;
        return FALSE;
    }
    snap_cpu.count = n;
    return TRUE;
}

static void append_letter( WCHAR *name, DWORD size, WCHAR letter )
{
    DWORD len = lstrlenW( name );
    if (len + 4 >= size) return;
    name[len++] = ' ';
    name[len++] = letter;
    name[len++] = ':';
    name[len] = 0;
}

static BOOL ensure_disk( BOOL force )
{
    DWORD i, n, letter_disk[26];
    struct disk_info *disks;
    WCHAR path[32];
    HANDLE h;

    if (!force && snap_disk.gen == cur_gen) return TRUE;
    free( snap_disk.disk );
    snap_disk.disk = NULL;
    snap_disk.count = 0;
    snap_disk.gen = cur_gen;

    for (i = 0; i < 26; i++)
    {
        STORAGE_DEVICE_NUMBER num;
        DWORD ret;

        letter_disk[i] = ~0u;
        lstrcpyW( path, L"A:\\" );
        path[0] = 'A' + i;
        if (GetDriveTypeW( path ) != DRIVE_FIXED) continue;
        lstrcpyW( path, L"\\\\.\\A:" );
        path[4] = 'A' + i;
        h = CreateFileW( path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
        if (h == INVALID_HANDLE_VALUE) continue;
        if (DeviceIoControl( h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &num, sizeof(num), &ret, NULL ) &&
            num.DeviceType == FILE_DEVICE_DISK)
            letter_disk[i] = num.DeviceNumber;
        CloseHandle( h );
    }

    if (!(disks = calloc( 16, sizeof(*disks) ))) return FALSE;
    for (n = 0, i = 0; i < 16; i++)
    {
        DWORD ret, l;

        swprintf( path, ARRAY_SIZE(path), L"\\\\.\\PhysicalDrive%u", i );
        h = CreateFileW( path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
        if (h == INVALID_HANDLE_VALUE) continue;
        swprintf( disks[n].name, ARRAY_SIZE(disks[n].name), L"%u", i );
        for (l = 0; l < 26; l++) if (letter_disk[l] == i) append_letter( disks[n].name, ARRAY_SIZE(disks[n].name), 'A' + l );
        disks[n].have_perf = DeviceIoControl( h, IOCTL_DISK_PERFORMANCE, NULL, 0, &disks[n].perf,
                                              sizeof(disks[n].perf), &ret, NULL );
        CloseHandle( h );
        n++;
    }
    snap_disk.disk = disks;
    snap_disk.count = n;
    return TRUE;
}

typedef DWORD (WINAPI *GetIfTable_t)( PMIB_IFTABLE, PULONG, BOOL );

static void sanitize_adapter_name( WCHAR *s )
{
    for (; *s; s++)
    {
        if (*s == '(') *s = '[';
        else if (*s == ')') *s = ']';
        else if (*s == '#' || *s == '/' || *s == '\\') *s = '_';
    }
}

static BOOL ensure_net( BOOL force )
{
    static HMODULE iphlpapi;
    GetIfTable_t pGetIfTable;
    MIB_IFTABLE *table = NULL;
    ULONG size = 0;
    DWORD i, n, ret;

    if (!force && snap_net.gen == cur_gen) return TRUE;
    free( snap_net.net );
    snap_net.net = NULL;
    snap_net.count = 0;
    snap_net.gen = cur_gen;

    if (!iphlpapi && !(iphlpapi = LoadLibraryW( L"iphlpapi.dll" ))) return FALSE;
    if (!(pGetIfTable = (GetIfTable_t)GetProcAddress( iphlpapi, "GetIfTable" ))) return FALSE;

    ret = pGetIfTable( NULL, &size, FALSE );
    if (ret != ERROR_INSUFFICIENT_BUFFER || !size) return FALSE;
    if (!(table = malloc( size ))) return FALSE;
    if (pGetIfTable( table, &size, FALSE ) != NO_ERROR)
    {
        free( table );
        return FALSE;
    }
    if (!(snap_net.net = calloc( table->dwNumEntries + 1, sizeof(*snap_net.net) )))
    {
        free( table );
        return FALSE;
    }
    for (n = 0, i = 0; i < table->dwNumEntries; i++)
    {
        MIB_IFROW *row = &table->table[i];
        DWORD len, j;

        if (row->dwType == MIB_IF_TYPE_LOOPBACK) continue;
        len = row->dwDescrLen;
        if (len && row->bDescr[len - 1] == 0) len--;
        if (len >= ARRAY_SIZE(snap_net.net[n].name)) len = ARRAY_SIZE(snap_net.net[n].name) - 1;
        len = MultiByteToWideChar( CP_ACP, 0, (const char *)row->bDescr, len, snap_net.net[n].name,
                                   ARRAY_SIZE(snap_net.net[n].name) - 1 );
        snap_net.net[n].name[len] = 0;
        if (!len)
        {
            WCHAR num[16];
            swprintf( num, ARRAY_SIZE(num), L"%lu", row->dwIndex );
            lstrcpyW( snap_net.net[n].name, L"Interface " );
            lstrcatW( snap_net.net[n].name, num );
        }
        sanitize_adapter_name( snap_net.net[n].name );
        for (j = 0; j < n; j++)  /* make duplicate names unique, as Windows does */
        {
            if (!wcsicmp( snap_net.net[j].name, snap_net.net[n].name ))
            {
                DWORD k = 2;
                WCHAR base[128];
                lstrcpyW( base, snap_net.net[n].name );
                do
                {
                    WCHAR num[16];
                    swprintf( num, ARRAY_SIZE(num), L" _%u", k++ );
                    lstrcpyW( snap_net.net[n].name, base );
                    lstrcatW( snap_net.net[n].name, num );
                }
                while (!wcsicmp( snap_net.net[j].name, snap_net.net[n].name ));
                break;
            }
        }
        snap_net.net[n].row = *row;
        n++;
    }
    snap_net.count = n;
    free( table );
    return TRUE;
}

static BOOL ensure_proc( BOOL force )
{
    ULONG size = 0x20000, ret;
    NTSTATUS status;
    BYTE *buf = NULL;
    struct pdh_process *p;
    DWORD n = 0, i;

    if (!force && snap_proc.gen == cur_gen) return snap_proc.buf != NULL;
    free_snap_proc();
    snap_proc.gen = cur_gen;

    for (;;)
    {
        free( buf );
        if (!(buf = malloc( size ))) return FALSE;
        status = query_sysinfo( PDH_SYSINFO_PROCESS, buf, size, &ret );
        if (status != (NTSTATUS)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */) break;
        size = max( ret, size ) + 0x10000;
    }
    if (status < 0)
    {
        free( buf );
        return FALSE;
    }

    for (p = (struct pdh_process *)buf;; p = (struct pdh_process *)((BYTE *)p + p->NextEntryOffset))
    {
        n++;
        snap_proc.threads += p->NumberOfThreads;
        if (!p->NextEntryOffset) break;
    }
    snap_proc.buf = buf;
    snap_proc.count = n;
    snap_proc.proc = calloc( n, sizeof(*snap_proc.proc) );
    snap_proc.name = calloc( n, sizeof(*snap_proc.name) );
    if (!snap_proc.proc || !snap_proc.name)
    {
        free_snap_proc();
        return FALSE;
    }
    for (i = 0, p = (struct pdh_process *)buf; i < n; i++, p = (struct pdh_process *)((BYTE *)p + p->NextEntryOffset))
    {
        WCHAR name[260];
        DWORD len, j, dup;

        snap_proc.proc[i] = p;
        if (!p->UniqueProcessId) lstrcpyW( name, L"Idle" );
        else if (p->UniqueProcessId == (HANDLE)4) lstrcpyW( name, L"System" );
        else if (p->ImageName.Buffer && p->ImageName.Length)
        {
            len = min( p->ImageName.Length / sizeof(WCHAR), ARRAY_SIZE(name) - 16 );
            memcpy( name, p->ImageName.Buffer, len * sizeof(WCHAR) );
            name[len] = 0;
            if (len > 4 && !wcsicmp( name + len - 4, L".exe" )) name[len - 4] = 0;
        }
        else lstrcpyW( name, L"Unknown" );
        sanitize_adapter_name( name );  /* same character replacements */

        for (dup = 0, j = 0; j < i; j++)  /* count earlier processes of the same name */
        {
            WCHAR *o = snap_proc.name[j];
            size_t l = lstrlenW( name );
            if (!wcsnicmp( o, name, l ) && (!o[l] || (o[l] == '#' && !wcschr( o + l + 1, '#' )))) dup++;
        }
        if (dup)
        {
            WCHAR num[16];
            swprintf( num, ARRAY_SIZE(num), L"#%u", dup );
            lstrcatW( name, num );
        }
        snap_proc.name[i] = wcsdup( name );
    }
    return TRUE;
}

/* ---- name tables ---- */

static const struct perf_object_def *sorted_objects[ARRAY_SIZE(objects)];
static BOOL sorted_init;

static int __cdecl cmp_object( const void *a, const void *b )
{
    return wcsicmp( (*(const struct perf_object_def * const *)a)->name, (*(const struct perf_object_def * const *)b)->name );
}

DWORD perf_object_count(void)
{
    return ARRAY_SIZE(objects);
}

const struct perf_object_def *perf_object( DWORD i )
{
    if (!sorted_init)
    {
        DWORD k;
        for (k = 0; k < ARRAY_SIZE(objects); k++) sorted_objects[k] = &objects[k];
        qsort( sorted_objects, ARRAY_SIZE(objects), sizeof(sorted_objects[0]), cmp_object );
        sorted_init = TRUE;
    }
    return i < ARRAY_SIZE(objects) ? sorted_objects[i] : NULL;
}

const struct perf_object_def *perf_find_object( const WCHAR *name )
{
    DWORD i;
    for (i = 0; i < ARRAY_SIZE(objects); i++)
        if (!wcsicmp( objects[i].name, name )) return &objects[i];
    return NULL;
}

const struct perf_counter_def *perf_find_counter( const struct perf_object_def *obj, const WCHAR *name )
{
    DWORD i;
    for (i = 0; i < obj->count; i++)
        if (!wcsicmp( obj->counters[i].name, name )) return &obj->counters[i];
    return NULL;
}

/* The name table: the Perflib\009 Counter value (index, name pairs), else the built-in schema */

static WCHAR *names_data;
static WCHAR **names_str;   /* parallel arrays: index and name of each entry */
static DWORD *names_idx;
static DWORD names_count;
static BOOL names_loaded;

static void add_name( DWORD index, const WCHAR *name )
{
    DWORD *ni;
    WCHAR **ns;

    if (!(ni = realloc( names_idx, (names_count + 1) * sizeof(*ni) ))) return;
    names_idx = ni;
    if (!(ns = realloc( names_str, (names_count + 1) * sizeof(*ns) ))) return;
    names_str = ns;
    names_idx[names_count] = index;
    names_str[names_count] = (WCHAR *)name;
    names_count++;
}

static void load_names(void)
{
    HKEY key;
    DWORD type, size = 0, i;
    WCHAR *p;

    if (names_loaded) return;
    names_loaded = TRUE;

    if (!RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Perflib\\009", 0,
                        KEY_QUERY_VALUE, &key ))
    {
        if (!RegQueryValueExW( key, L"Counter", NULL, &type, NULL, &size ) && type == REG_MULTI_SZ && size >= sizeof(WCHAR) &&
            (names_data = malloc( size + 2 * sizeof(WCHAR) )))
        {
            if (!RegQueryValueExW( key, L"Counter", NULL, &type, (BYTE *)names_data, &size ))
            {
                names_data[size / sizeof(WCHAR)] = 0;
                names_data[size / sizeof(WCHAR) + 1] = 0;
                for (p = names_data; *p;)
                {
                    WCHAR *idx = p, *name = p + lstrlenW( p ) + 1;
                    if (!*name) break;
                    add_name( wcstoul( idx, NULL, 10 ), name );
                    p = name + lstrlenW( name ) + 1;
                }
            }
            else
            {
                free( names_data );
                names_data = NULL;
            }
        }
        RegCloseKey( key );
    }
    if (names_count) return;

    /* no registry data: serve the names of the built-in schema */
    for (i = 0; i < ARRAY_SIZE(objects); i++)
    {
        DWORD k;
        add_name( objects[i].index, objects[i].name );
        for (k = 0; k < objects[i].count; k++) add_name( objects[i].counters[k].index, objects[i].counters[k].name );
    }
}

BOOL perf_name_by_index( DWORD index, const WCHAR **name )
{
    DWORD i;
    BOOL found = FALSE;

    EnterCriticalSection( &perf_cs );
    load_names();
    for (i = 0; i < names_count; i++)
    {
        if (names_idx[i] == index)
        {
            *name = names_str[i];
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection( &perf_cs );
    return found;
}

BOOL perf_index_by_name( const WCHAR *name, DWORD *index )
{
    DWORD i;
    BOOL found = FALSE;

    EnterCriticalSection( &perf_cs );
    load_names();
    for (i = 0; i < names_count; i++)
    {
        if (!wcsicmp( names_str[i], name ))
        {
            *index = names_idx[i];
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection( &perf_cs );
    return found;
}

/* ---- instances ---- */

PDH_STATUS perf_get_instances( const struct perf_object_def *obj, WCHAR ***names, DWORD *count )
{
    WCHAR **list = NULL;
    DWORD i, n = 0;
    PDH_STATUS ret = ERROR_SUCCESS;

    *names = NULL;
    *count = 0;
    if (obj->kind == INST_NONE) return ERROR_SUCCESS;

    EnterCriticalSection( &perf_cs );
    switch (obj->kind)
    {
    case INST_CPU:
        n = cpu_count() + 1;
        if (!(list = calloc( n, sizeof(*list) ))) { ret = PDH_MEMORY_ALLOCATION_FAILURE; break; }
        for (i = 0; i + 1 < n; i++)
        {
            WCHAR buf[16];
            swprintf( buf, ARRAY_SIZE(buf), L"%u", i );
            list[i] = wcsdup( buf );
        }
        list[n - 1] = wcsdup( L"_Total" );
        break;

    case INST_DISK:
        ensure_disk( TRUE );
        n = snap_disk.count + 1;
        if (!(list = calloc( n, sizeof(*list) ))) { ret = PDH_MEMORY_ALLOCATION_FAILURE; break; }
        for (i = 0; i + 1 < n; i++) list[i] = wcsdup( snap_disk.disk[i].name );
        list[n - 1] = wcsdup( L"_Total" );
        break;

    case INST_NET:
        ensure_net( TRUE );
        n = snap_net.count;
        if (!n) break;
        if (!(list = calloc( n, sizeof(*list) ))) { ret = PDH_MEMORY_ALLOCATION_FAILURE; break; }
        for (i = 0; i < n; i++) list[i] = wcsdup( snap_net.net[i].name );
        break;

    case INST_PROC:
        if (!ensure_proc( TRUE )) { ret = PDH_CSTATUS_NO_INSTANCE; break; }
        n = snap_proc.count + 1;
        if (!(list = calloc( n, sizeof(*list) ))) { ret = PDH_MEMORY_ALLOCATION_FAILURE; break; }
        for (i = 0; i + 1 < n; i++) list[i] = wcsdup( snap_proc.name[i] );
        list[n - 1] = wcsdup( L"_Total" );
        break;

    default:
        break;
    }
    LeaveCriticalSection( &perf_cs );

    if (ret) { free( list ); return ret; }
    *names = list;
    *count = n;
    return ERROR_SUCCESS;
}

void perf_free_instances( WCHAR **names, DWORD count )
{
    DWORD i;
    for (i = 0; i < count; i++) free( names[i] );
    free( names );
}

/* ---- counter values ---- */

#define TIME_NOW(v) ((v)->second = perf_now())

static void fetch_memory( DWORD id, LONGLONG *first, LONGLONG *second, BOOL *ok )
{
    static DWORD page_size;
    const struct pdh_spi *s;
    LONGLONG pages;

    if (!page_size)
    {
        SYSTEM_INFO si;
        GetSystemInfo( &si );
        page_size = si.dwPageSize;
    }
    if (!(*ok = ensure_spi( FALSE ))) return;
    s = &snap_spi.spi;
    *second = 0;

    switch (id)
    {
    case CID_MEMORY_PAGE_FAULTS_SEC:        *first = s->PageFaultCount; break;
    case CID_MEMORY_TRANSITION_FAULTS_SEC:  *first = s->TransitionCount; break;
    case CID_MEMORY_CACHE_FAULTS_SEC:       *first = s->CacheTransitionCount; break;
    case CID_MEMORY_DEMAND_ZERO_FAULTS_SEC: *first = s->DemandZeroCount; break;
    case CID_MEMORY_PAGES_SEC:              *first = (LONGLONG)s->PageReadCount + s->DirtyPagesWriteCount + s->MappedPagesWriteCount; break;
    case CID_MEMORY_PAGES_INPUT_SEC:        *first = s->PageReadCount; break;
    case CID_MEMORY_PAGE_READS_SEC:         *first = s->PageReadIoCount; break;
    case CID_MEMORY_PAGES_OUTPUT_SEC:       *first = (LONGLONG)s->DirtyPagesWriteCount + s->MappedPagesWriteCount; break;
    case CID_MEMORY_PAGE_WRITES_SEC:        *first = (LONGLONG)s->DirtyWriteIoCount + s->MappedWriteIoCount; break;
    case CID_MEMORY_AVAILABLE_BYTES:        *first = (LONGLONG)s->AvailablePages * page_size; return;
    case CID_MEMORY_AVAILABLE_KBYTES:       *first = (LONGLONG)s->AvailablePages * page_size / 1024; return;
    case CID_MEMORY_AVAILABLE_MBYTES:       *first = (LONGLONG)s->AvailablePages * page_size / (1024 * 1024); return;
    case CID_MEMORY_COMMITTED_BYTES:        *first = (LONGLONG)s->CommittedPages * page_size; return;
    case CID_MEMORY_COMMIT_LIMIT:           *first = (LONGLONG)s->CommitLimit * page_size; return;
    case CID_MEMORY_POOL_PAGED_BYTES:       *first = (LONGLONG)s->PagedPoolPages * page_size; return;
    case CID_MEMORY_POOL_NONPAGED_BYTES:    *first = (LONGLONG)s->NonPagedPoolPages * page_size; return;
    case CID_MEMORY_COMMITTED_BYTES_IN_USE:
        pages = s->CommittedPages;
        *first = pages;
        *second = s->CommitLimit;
        return;
    default:
        *ok = FALSE;
        return;
    }
    *second = perf_now();
}

static void fetch_system( DWORD id, LONGLONG *first, LONGLONG *second, BOOL *ok )
{
    const struct pdh_spi *s;

    *second = 0;
    switch (id)
    {
    case CID_SYSTEM_SYSTEM_UP_TIME:
        *second = perf_now();
        *first = *second - (LONGLONG)GetTickCount64() * 10000;
        *ok = TRUE;
        return;
    case CID_SYSTEM_PROCESSES:
        if (!(*ok = ensure_proc( FALSE ))) return;
        *first = snap_proc.count;
        return;
    case CID_SYSTEM_THREADS:
        if (!(*ok = ensure_proc( FALSE ))) return;
        *first = snap_proc.threads;
        return;
    }

    if (!(*ok = ensure_spi( FALSE ))) return;
    s = &snap_spi.spi;
    switch (id)
    {
    case CID_SYSTEM_FILE_READ_OPERATIONS_SEC:    *first = s->IoReadOperationCount; break;
    case CID_SYSTEM_FILE_WRITE_OPERATIONS_SEC:   *first = s->IoWriteOperationCount; break;
    case CID_SYSTEM_FILE_CONTROL_OPERATIONS_SEC: *first = s->IoOtherOperationCount; break;
    case CID_SYSTEM_FILE_DATA_OPERATIONS_SEC:    *first = (LONGLONG)s->IoReadOperationCount + s->IoWriteOperationCount; break;
    case CID_SYSTEM_FILE_READ_BYTES_SEC:         *first = s->IoReadTransferCount.QuadPart; break;
    case CID_SYSTEM_FILE_WRITE_BYTES_SEC:        *first = s->IoWriteTransferCount.QuadPart; break;
    case CID_SYSTEM_FILE_CONTROL_BYTES_SEC:      *first = s->IoOtherTransferCount.QuadPart; break;
    case CID_SYSTEM_CONTEXT_SWITCHES_SEC:        *first = s->ContextSwitches; break;
    case CID_SYSTEM_SYSTEM_CALLS_SEC:            *first = s->SystemCalls; break;
    default: *ok = FALSE; return;
    }
    *second = perf_now();
}

static LONGLONG cpu_value( DWORD id, const struct pdh_cpu *c )
{
    LONGLONG priv;

    switch (id)
    {
    case CID_PROCESSOR_PROCESSOR_TIME:
    case CID_PROCESSOR_IDLE_TIME:         return c->IdleTime.QuadPart;
    case CID_PROCESSOR_USER_TIME:         return c->UserTime.QuadPart;
    case CID_PROCESSOR_PRIVILEGED_TIME:
        priv = c->KernelTime.QuadPart - c->IdleTime.QuadPart;
        return priv > 0 ? priv : 0;
    case CID_PROCESSOR_DPC_TIME:          return c->DpcTime.QuadPart;
    case CID_PROCESSOR_INTERRUPT_TIME:    return c->InterruptTime.QuadPart;
    case CID_PROCESSOR_INTERRUPTS_SEC:    return c->InterruptCount;
    }
    return 0;
}

static void fetch_cpu( DWORD id, const WCHAR *inst, LONGLONG *first, LONGLONG *second, PDH_STATUS *status )
{
    DWORD i;

    if (!ensure_cpu( FALSE )) { *status = PDH_CSTATUS_INVALID_DATA; return; }
    if (!wcsicmp( inst, L"_Total" ))
    {
        LONGLONG sum = 0;
        for (i = 0; i < snap_cpu.count; i++) sum += cpu_value( id, &snap_cpu.cpu[i] );
        /* the time counters of the total are the average over the processors */
        *first = id == CID_PROCESSOR_INTERRUPTS_SEC ? sum : sum / (LONGLONG)snap_cpu.count;
    }
    else
    {
        WCHAR *end;
        i = wcstoul( inst, &end, 10 );
        if (!*inst || *end || i >= snap_cpu.count) { *status = PDH_CSTATUS_NO_INSTANCE; return; }
        *first = cpu_value( id, &snap_cpu.cpu[i] );
    }
    *second = perf_now();
}

static LONGLONG disk_value( DWORD id, const struct disk_info *d )
{
    const struct pdh_disk_perf *p = &d->perf;

    if (!d->have_perf) return 0;
    switch (id)
    {
    case CID_PHYSICALDISK_CURRENT_DISK_QUEUE_LENGTH: return p->QueueDepth;
    case CID_PHYSICALDISK_DISK_TIME:                 return p->ReadTime.QuadPart + p->WriteTime.QuadPart;
    case CID_PHYSICALDISK_DISK_READ_TIME:            return p->ReadTime.QuadPart;
    case CID_PHYSICALDISK_DISK_WRITE_TIME:           return p->WriteTime.QuadPart;
    case CID_PHYSICALDISK_IDLE_TIME:                 return p->IdleTime.QuadPart;
    case CID_PHYSICALDISK_DISK_TRANSFERS_SEC:        return (LONGLONG)p->ReadCount + p->WriteCount;
    case CID_PHYSICALDISK_DISK_READS_SEC:            return p->ReadCount;
    case CID_PHYSICALDISK_DISK_WRITES_SEC:           return p->WriteCount;
    case CID_PHYSICALDISK_DISK_BYTES_SEC:            return p->BytesRead.QuadPart + p->BytesWritten.QuadPart;
    case CID_PHYSICALDISK_DISK_READ_BYTES_SEC:       return p->BytesRead.QuadPart;
    case CID_PHYSICALDISK_DISK_WRITE_BYTES_SEC:      return p->BytesWritten.QuadPart;
    }
    return 0;
}

static void fetch_disk( DWORD id, const WCHAR *inst, LONGLONG *first, LONGLONG *second, PDH_STATUS *status )
{
    DWORD i;

    if (!ensure_disk( FALSE )) { *status = PDH_CSTATUS_INVALID_DATA; return; }
    if (!wcsicmp( inst, L"_Total" ))
    {
        LONGLONG sum = 0;
        for (i = 0; i < snap_disk.count; i++) sum += disk_value( id, &snap_disk.disk[i] );
        *first = sum;
    }
    else
    {
        for (i = 0; i < snap_disk.count; i++) if (!wcsicmp( snap_disk.disk[i].name, inst )) break;
        if (i == snap_disk.count) { *status = PDH_CSTATUS_NO_INSTANCE; return; }
        *first = disk_value( id, &snap_disk.disk[i] );
    }
    *second = id == CID_PHYSICALDISK_CURRENT_DISK_QUEUE_LENGTH ? 0 : perf_now();
}

static void fetch_net( DWORD id, const WCHAR *inst, LONGLONG *first, LONGLONG *second, PDH_STATUS *status )
{
    const MIB_IFROW *r;
    DWORD i;

    if (!ensure_net( FALSE )) { *status = PDH_CSTATUS_INVALID_DATA; return; }
    for (i = 0; i < snap_net.count; i++) if (!wcsicmp( snap_net.net[i].name, inst )) break;
    if (i == snap_net.count) { *status = PDH_CSTATUS_NO_INSTANCE; return; }
    r = &snap_net.net[i].row;

    *second = 0;
    switch (id)
    {
    case CID_NETWORK_INTERFACE_BYTES_TOTAL_SEC:       *first = (LONGLONG)r->dwInOctets + r->dwOutOctets; break;
    case CID_NETWORK_INTERFACE_BYTES_RECEIVED_SEC:    *first = r->dwInOctets; break;
    case CID_NETWORK_INTERFACE_BYTES_SENT_SEC:        *first = r->dwOutOctets; break;
    case CID_NETWORK_INTERFACE_PACKETS_SEC:           *first = (LONGLONG)r->dwInUcastPkts + r->dwInNUcastPkts + r->dwOutUcastPkts + r->dwOutNUcastPkts; break;
    case CID_NETWORK_INTERFACE_PACKETS_RECEIVED_SEC:  *first = (LONGLONG)r->dwInUcastPkts + r->dwInNUcastPkts; break;
    case CID_NETWORK_INTERFACE_PACKETS_SENT_SEC:      *first = (LONGLONG)r->dwOutUcastPkts + r->dwOutNUcastPkts; break;
    case CID_NETWORK_INTERFACE_CURRENT_BANDWIDTH:     *first = r->dwSpeed; return;
    case CID_NETWORK_INTERFACE_PACKETS_RECEIVED_ERRORS: *first = r->dwInErrors; return;
    case CID_NETWORK_INTERFACE_PACKETS_OUTBOUND_ERRORS: *first = r->dwOutErrors; return;
    default: *status = PDH_CSTATUS_INVALID_DATA; return;
    }
    *second = perf_now();
}

static LONGLONG proc_value( DWORD id, const struct pdh_process *p )
{
    switch (id)
    {
    case CID_PROCESS_PROCESSOR_TIME:     return p->UserTime.QuadPart + p->KernelTime.QuadPart;
    case CID_PROCESS_USER_TIME:          return p->UserTime.QuadPart;
    case CID_PROCESS_PRIVILEGED_TIME:    return p->KernelTime.QuadPart;
    case CID_PROCESS_VIRTUAL_BYTES:      return p->vm.VirtualSize;
    case CID_PROCESS_PAGE_FAULTS_SEC:    return p->vm.PageFaultCount;
    case CID_PROCESS_WORKING_SET:        return p->vm.WorkingSetSize;
    case CID_PROCESS_PRIVATE_BYTES:      return p->vm.PrivateUsage;
    case CID_PROCESS_PAGE_FILE_BYTES:    return p->vm.PagefileUsage;
    case CID_PROCESS_THREAD_COUNT:       return p->NumberOfThreads;
    case CID_PROCESS_HANDLE_COUNT:       return p->HandleCount;
    case CID_PROCESS_ID_PROCESS:         return (LONGLONG)(ULONG_PTR)p->UniqueProcessId;
    case CID_PROCESS_CREATING_PROCESS_ID: return (LONGLONG)(ULONG_PTR)p->InheritedFromUniqueProcessId;
    case CID_PROCESS_ELAPSED_TIME:       return p->CreateTime.QuadPart;
    }
    return 0;
}

static void fetch_proc( DWORD id, const WCHAR *inst, LONGLONG *first, LONGLONG *second, PDH_STATUS *status )
{
    DWORD i;

    if (!ensure_proc( FALSE )) { *status = PDH_CSTATUS_INVALID_DATA; return; }
    if (!wcsicmp( inst, L"_Total" ))
    {
        LONGLONG sum = 0;
        if (id != CID_PROCESS_ID_PROCESS && id != CID_PROCESS_CREATING_PROCESS_ID && id != CID_PROCESS_ELAPSED_TIME)
            for (i = 0; i < snap_proc.count; i++) sum += proc_value( id, snap_proc.proc[i] );
        *first = sum;
    }
    else
    {
        for (i = 0; i < snap_proc.count; i++) if (!wcsicmp( snap_proc.name[i], inst )) break;
        if (i == snap_proc.count) { *status = PDH_CSTATUS_NO_INSTANCE; return; }
        *first = proc_value( id, snap_proc.proc[i] );
    }
    switch (id)
    {
    case CID_PROCESS_VIRTUAL_BYTES: case CID_PROCESS_WORKING_SET: case CID_PROCESS_PRIVATE_BYTES:
    case CID_PROCESS_PAGE_FILE_BYTES: case CID_PROCESS_THREAD_COUNT: case CID_PROCESS_HANDLE_COUNT:
    case CID_PROCESS_ID_PROCESS: case CID_PROCESS_CREATING_PROCESS_ID:
        *second = 0;
        break;
    default:
        *second = perf_now();
    }
}

PDH_STATUS perf_fetch( const struct perf_object_def *obj, const struct perf_counter_def *ctr, const WCHAR *inst,
                       DWORD instance_index, LONGLONG *first, LONGLONG *second )
{
    PDH_STATUS status = ERROR_SUCCESS;
    WCHAR key[300];
    BOOL ok = TRUE;

    *first = *second = 0;
    if (obj->kind != INST_NONE)
    {
        if (!inst || wcschr( inst, '*' ) || wcschr( inst, '?' )) return PDH_CSTATUS_NO_INSTANCE;
        lstrcpynW( key, inst, ARRAY_SIZE(key) - 16 );
        if (instance_index && instance_index != ~0u)
        {
            WCHAR num[16];
            swprintf( num, ARRAY_SIZE(num), L"#%u", instance_index );
            lstrcatW( key, num );
        }
    }

    EnterCriticalSection( &perf_cs );
    switch (obj->kind)
    {
    case INST_NONE:
        if (obj == perf_find_object( L"Memory" )) fetch_memory( ctr->id, first, second, &ok );
        else fetch_system( ctr->id, first, second, &ok );
        if (!ok) status = PDH_CSTATUS_INVALID_DATA;
        break;
    case INST_CPU:  fetch_cpu( ctr->id, key, first, second, &status ); break;
    case INST_DISK: fetch_disk( ctr->id, key, first, second, &status ); break;
    case INST_NET:  fetch_net( ctr->id, key, first, second, &status ); break;
    case INST_PROC: fetch_proc( ctr->id, key, first, second, &status ); break;
    }
    LeaveCriticalSection( &perf_cs );
    return status;
}
