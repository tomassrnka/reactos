/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS kernel
 * FILE:        include/reactos/services/services.h
 * PURPOSE:     Private interface between SERVICES.EXE and ADVAPI32.DLL
 * PROGRAMMER:  Eric Kohl
 */

#ifndef __SERVICES_SERVICES_H__
#define __SERVICES_SERVICES_H__

/*
 * Internal control codes.
 * Neither in the range of public control codes (1-10 and 11-14 or 16 or 32 or 64)
 * nor in the range of user-defined control codes (128-255).
 */
/* Start a service that shares a process with other services */
#define SERVICE_CONTROL_START_SHARE 80
/* Start a service that runs in its own process */
#define SERVICE_CONTROL_START_OWN   81

/*
 * Start event name used by OpenSCManager
 * to know whether the SCM is initialized.
 */
#define SCM_START_EVENT L"SvcctrlStartEvent_A3752DX"

typedef struct _SCM_CONTROL_PACKET
{
    DWORD dwSize;
    DWORD dwControl;
    DWORD dwArgumentsCount;
    DWORD dwServiceTag;
    /* Fixed width: the packet goes from the 64-bit services.exe to WoW64 (32-bit)
     * service processes as well, so its layout must not depend on the pointer size */
    ULONG64 hServiceStatus;
    DWORD dwServiceNameOffset;
    /* Offset of the argument offsets vector: dwArgumentsCount DWORD offsets, relative
     * to the start of the vector, followed by the NUL-terminated UNICODE strings */
    DWORD dwArgumentsOffset;
} SCM_CONTROL_PACKET, *PSCM_CONTROL_PACKET;
C_ASSERT(sizeof(SCM_CONTROL_PACKET) == 32);

typedef struct _SCM_REPLY_PACKET
{
    DWORD dwError;
} SCM_REPLY_PACKET, *PSCM_REPLY_PACKET;

#endif /* __SERVICES_SERVICES_H__ */

/* EOF */
