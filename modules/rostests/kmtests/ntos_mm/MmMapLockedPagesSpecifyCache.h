/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite MmMapLockedPagesSpecifyCache test declarations
 * PROGRAMMER:      Pierre Schweitzer <pierre@reactos.org>
 */


#ifndef _KMTEST_MMMAPLOCKEDPAGESSPECIFYCACHE_H_
#define _KMTEST_MMMAPLOCKEDPAGESSPECIFYCACHE_H_

typedef struct _QUERY_BUFFER
{
    USHORT Length;
    PVOID Buffer;
    BOOLEAN Cached;
    NTSTATUS Status;
} QUERY_BUFFER, *PQUERY_BUFFER;

typedef struct _READ_BUFFER
{
    USHORT Length;
    PVOID Buffer;
    ULONG Pattern;
} READ_BUFFER, *PREAD_BUFFER;

typedef struct _EXIT_BUFFER
{
    ULONG_PTR ProcessId;
    PVOID Buffer;
    NTSTATUS Status;
} EXIT_BUFFER, *PEXIT_BUFFER;

#define IOCTL_QUERY_BUFFER   1
#define IOCTL_READ_BUFFER    2
#define IOCTL_CLEAN          3
#define IOCTL_MAP_IN_PROCESS 4
#define IOCTL_CHECK_EXITED   5

#define EXIT_BUFFER_LENGTH (2 * PAGE_SIZE)

#define WRITE_PATTERN 0xA4A5A6A7

#endif /* !defined _KMTEST_MMMAPLOCKEDPAGESSPECIFYCACHE_H_ */
