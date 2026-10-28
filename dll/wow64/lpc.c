/*
 * PROJECT:     ReactOS WoW64
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Guest LPC to the LSA authentication port (LsaRegisterLogonProcess, LsaLogonUser)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * LPC messages carry the pointers and structure layouts of the sender, and lsass is a native
 * process. A guest connects only to \LsaAuthenticationPort, and each request for which a
 * translation exists is rebuilt in the native layout (the guest's pointers widen, so lsass reads
 * the caller's memory from them as before), and its reply is converted back. Any other port,
 * or an LSA request without a translation, is refused.
 */

#include "wow64p.h"
#include <ntsecapi.h>
#include <lsass/lsass.h>

/* The guest's PORT_MESSAGE: CLIENT_ID and the view size are 32 bits wide */
typedef struct
{
    USHORT DataLength, TotalLength, Type, DataInfoOffset;
    ULONG ProcessId, ThreadId, MessageId, ClientViewSize;
} WOW64_PORT_MESSAGE;
C_ASSERT(sizeof(WOW64_PORT_MESSAGE) == 24);

/* The guest's LSA_API_MSG body, which follows the header (from lsass.h compiled for i386) */
typedef struct
{
    WOW64_STRING OriginName;
    ULONG LogonType;
    ULONG AuthenticationPackage;
    ULONG AuthenticationInformation;
    ULONG AuthenticationInformationLength;
    ULONG LocalGroups;
    ULONG LocalGroupsCount;
    TOKEN_SOURCE SourceContext;
} WOW64_LSA_LOGON_REQUEST;
C_ASSERT(sizeof(WOW64_LSA_LOGON_REQUEST) == 48);

typedef struct
{
    ULONG ProfileBuffer;
    ULONG ProfileBufferLength;
    LUID LogonId;
    ULONG Token;
    ULONG Reserved1;
    ULONG Quotas[5]; /* QUOTA_LIMITS without the time limit: pool, working set and pagefile limits */
    ULONG Reserved2;
    LONGLONG TimeLimit;
    LONG SubStatus;
} WOW64_LSA_LOGON_REPLY;
C_ASSERT(FIELD_OFFSET(WOW64_LSA_LOGON_REPLY, Quotas) == 24);
C_ASSERT(FIELD_OFFSET(WOW64_LSA_LOGON_REPLY, TimeLimit) == 48);
C_ASSERT(FIELD_OFFSET(WOW64_LSA_LOGON_REPLY, SubStatus) == 56);

typedef struct
{
    ULONG ApiNumber;
    LONG Status;
    union
    {
        WOW64_LSA_LOGON_REQUEST LogonRequest;
        WOW64_LSA_LOGON_REPLY LogonReply;
        struct { ULONG PackageNameLength; CHAR PackageName[LSASS_MAX_PACKAGE_NAME_LENGTH + 1]; } LookupRequest;
        struct { ULONG Package; } LookupReply;
    };
} WOW64_LSA_BODY;

/* MSV1_0_INTERACTIVE_LOGON and MSV1_0_INTERACTIVE_PROFILE for i386 */
typedef struct
{
    ULONG MessageType;
    WOW64_STRING LogonDomainName, UserName, Password;
} WOW64_MSV1_0_INTERACTIVE_LOGON;
C_ASSERT(sizeof(WOW64_MSV1_0_INTERACTIVE_LOGON) == 28);

typedef struct
{
    ULONG MessageType;
    USHORT LogonCount, BadPasswordCount;
    LONGLONG LogonTime, LogoffTime, KickOffTime, PasswordLastSet, PasswordCanChange, PasswordMustChange;
    WOW64_STRING LogonScript, HomeDirectory, FullName, ProfilePath, HomeDirectoryDrive, LogonServer;
    ULONG UserFlags;
} WOW64_MSV1_0_INTERACTIVE_PROFILE;
C_ASSERT(FIELD_OFFSET(WOW64_MSV1_0_INTERACTIVE_PROFILE, LogonScript) == 56);
C_ASSERT(sizeof(WOW64_MSV1_0_INTERACTIVE_PROFILE) == 112);

#define LSA_PORT_NAME L"\\LsaAuthenticationPort"
#define MAX_LSA_PORTS 16
static volatile LONG LsaPorts[MAX_LSA_PORTS]; /* the guest's handles to the LSA port, 0 for a free slot */

static VOID Forget(ULONG Handle)
{
    ULONG i;
    for (i = 0; Handle && i < MAX_LSA_PORTS; ++i)
    {
        /* A plain read first: most calls to Wow64LpcForget concern a handle that is no port */
        if ((ULONG)LsaPorts[i] == Handle)
            InterlockedCompareExchange(&LsaPorts[i], 0, (LONG)Handle);
    }
}

/* The slot of a handle that was closed or moved without passing through Wow64LpcForget would
 * make a later object with the same handle value look like the LSA port, so a hit must still
 * be a port */
static BOOLEAN IsLsaPort(ULONG Handle)
{
    UCHAR Buffer[sizeof(OBJECT_TYPE_INFORMATION) + 64 * sizeof(WCHAR)];
    POBJECT_TYPE_INFORMATION Type = (POBJECT_TYPE_INFORMATION)Buffer;
    UNICODE_STRING Port, AlpcPort;
    ULONG i;
    BOOLEAN Found = FALSE;

    for (i = 0; Handle && !Found && i < MAX_LSA_PORTS; ++i)
        Found = (ULONG)LsaPorts[i] == Handle;
    if (!Found) return FALSE;
    RtlInitUnicodeString(&Port, L"Port");
    RtlInitUnicodeString(&AlpcPort, L"ALPC Port");
    if (NT_SUCCESS(NtQueryObject(HANDLE32(Handle), ObjectTypeInformation, Buffer, sizeof(Buffer), NULL)) &&
        (RtlEqualUnicodeString(&Type->TypeName, &Port, TRUE) || RtlEqualUnicodeString(&Type->TypeName, &AlpcPort, TRUE)))
        return TRUE;
    Forget(Handle);
    return FALSE;
}

VOID Wow64LpcForget(ULONG Handle)
{
    Forget(Handle);
}

static BOOLEAN Remember(ULONG Handle)
{
    ULONG i;
    for (i = 0; i < MAX_LSA_PORTS; ++i)
        if (!InterlockedCompareExchange(&LsaPorts[i], (LONG)Handle, 0)) return TRUE;
    return FALSE;
}

static BOOLEAN InGuest(ULONG Address, ULONG64 Size)
{
    return (ULONG64)Address + Size <= (ULONG64)Wow64HighestUserAddress + 1;
}

/* NtConnectPort(PHANDLE, PUNICODE_STRING, PSECURITY_QUALITY_OF_SERVICE, PPORT_VIEW, PREMOTE_PORT_VIEW,
 * PULONG MaxMessageLength, PVOID ConnectionInformation, PULONG ConnectionInformationLength).
 * The quality of service and the connection information (LSA_CONNECTION_INFO) hold no pointers. */
NTSTATUS Wow64LpcConnect(ULONG Number, const ULONG *a, HANDLE *Handle, BOOLEAN *Reported)
{
    UNICODE_STRING Name, Lsa;
    NTSTATUS Status;

    if (!a[1] || !InGuest(a[1], sizeof(WOW64_STRING)))
        return STATUS_ACCESS_VIOLATION;
    Name.Length = ((WOW64_STRING *)PTR32(a[1]))->Length;
    Name.MaximumLength = ((WOW64_STRING *)PTR32(a[1]))->MaximumLength;
    Name.Buffer = PTR32(((WOW64_STRING *)PTR32(a[1]))->Buffer);
    if (Name.Length > Name.MaximumLength || !InGuest((ULONG)(ULONG_PTR)Name.Buffer, Name.Length))
        return STATUS_ACCESS_VIOLATION;
    RtlInitUnicodeString(&Lsa, LSA_PORT_NAME);
    if (!RtlEqualUnicodeString(&Name, &Lsa, TRUE))
        return Wow64Unsupported(Number, WOW64_UNSUPPORTED_SERVICE, 0, Reported);
    /* The views carry native pointers and sizes; the LSA port uses neither */
    if (a[3] || a[4])
        return Wow64Unsupported(Number, WOW64_UNSUPPORTED_SERVICE, 1, Reported);

    Status = NtConnectPort(Handle, &Name, PTR32(a[2]), NULL, NULL, PTR32(a[5]), PTR32(a[6]), PTR32(a[7]));
    if (NT_SUCCESS(Status) && !Remember(HandleToUlong(*Handle)))
    {
        NtClose(*Handle);
        *Handle = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return Status;
}

/* The guest's MSV1_0_INTERACTIVE_LOGON in the native layout, in one heap block with its strings.
 * The block holds the password, so it is wiped before it is freed. */
static NTSTATUS InteractiveLogonFrom32(ULONG Number, ULONG Guest, ULONG Length, PVOID *Native, ULONG *NativeLength)
{
    WOW64_MSV1_0_INTERACTIVE_LOGON Logon;
    const WOW64_STRING *From[3];
    ULONG Bytes[3];
    UNICODE_STRING *To[3];
    PMSV1_0_INTERACTIVE_LOGON Blob;
    PUCHAR Next;
    ULONG Size = sizeof(MSV1_0_INTERACTIVE_LOGON), i;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Length < sizeof(Logon) || !InGuest(Guest, Length))
        return STATUS_INVALID_PARAMETER;
    _SEH2_TRY
    {
        RtlCopyMemory(&Logon, PTR32(Guest), sizeof(Logon));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status)) return Status;
    if (Logon.MessageType != MsV1_0InteractiveLogon)
    {
        if (Wow64FirstReport(Number, WOW64_UNSUPPORTED_CLASS, Logon.MessageType))
            DbgPrint("WoW64: unsupported LsaLogonUser logon submit type %lu, pid %lx\n", Logon.MessageType,
                     HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess));
        return STATUS_NOT_SUPPORTED;
    }
    From[0] = &Logon.LogonDomainName;
    From[1] = &Logon.UserName;
    From[2] = &Logon.Password;
    for (i = 0; i < 3; ++i)
    {
        /* As msv1_0 reads it, the password's length keeps the seed of its encoding in the high byte
         * (RtlRunEncodeUnicodeString) and the data is the low byte's worth of bytes; the seed is
         * passed on as it is. Domain and user name are plain. */
        Bytes[i] = i == 2 ? (From[i]->Length & 0xff) : From[i]->Length;
        if (Bytes[i] > From[i]->MaximumLength || (Bytes[i] && (!From[i]->Buffer || !InGuest(From[i]->Buffer, Bytes[i]))))
            return STATUS_INVALID_PARAMETER;
        Size += From[i]->MaximumLength;
    }
    Blob = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!Blob) return STATUS_NO_MEMORY;
    Blob->MessageType = MsV1_0InteractiveLogon;
    To[0] = &Blob->LogonDomainName;
    To[1] = &Blob->UserName;
    To[2] = &Blob->Password;
    Next = (PUCHAR)(Blob + 1);
    _SEH2_TRY
    {
        for (i = 0; i < 3; ++i)
        {
            To[i]->Length = From[i]->Length;
            To[i]->MaximumLength = From[i]->MaximumLength;
            /* As advapi32 builds it, a missing string keeps a NULL buffer */
            To[i]->Buffer = From[i]->Buffer ? (PWSTR)Next : NULL;
            if (Bytes[i]) RtlCopyMemory(Next, PTR32(From[i]->Buffer), Bytes[i]);
            Next += From[i]->MaximumLength;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
    {
        RtlSecureZeroMemory(Blob, Size);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Blob);
        return Status;
    }
    *Native = Blob;
    *NativeLength = Size;
    return STATUS_SUCCESS;
}

/* The guest's TOKEN_GROUPS in the native layout; the SIDs stay in guest memory, where lsass reads them */
static NTSTATUS GroupsFrom32(ULONG Guest, ULONG Count, PTOKEN_GROUPS *Native)
{
    const ULONG *Source = PTR32(Guest);
    PTOKEN_GROUPS Groups;
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    *Native = NULL;
    if (!Count) return STATUS_SUCCESS;
    if (!Guest || Count > WOW64_MAX_ARRAY_COUNT || !InGuest(Guest, sizeof(ULONG) + (ULONG64)Count * 2 * sizeof(ULONG)))
        return STATUS_INVALID_PARAMETER;
    Groups = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY,
                             FIELD_OFFSET(TOKEN_GROUPS, Groups) + (SIZE_T)Count * sizeof(SID_AND_ATTRIBUTES));
    if (!Groups) return STATUS_NO_MEMORY;
    Groups->GroupCount = Count;
    _SEH2_TRY
    {
        for (i = 0; i < Count; ++i)
        {
            Groups->Groups[i].Sid = PTR32(Source[1 + 2 * i]);
            Groups->Groups[i].Attributes = Source[2 + 2 * i];
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Groups);
        return Status;
    }
    *Native = Groups;
    return STATUS_SUCCESS;
}

/* The native profile lsass left in this process, as a guest MSV1_0_INTERACTIVE_PROFILE in a new
 * block below the guest's limit. The native block is freed; the guest frees the new one with
 * LsaFreeReturnBuffer as for any profile. Other profile types are dropped. */
static NTSTATUS ProfileTo32(PVOID Native, ULONG *Guest, ULONG *GuestLength)
{
    PMSV1_0_INTERACTIVE_PROFILE From = Native;
    WOW64_MSV1_0_INTERACTIVE_PROFILE *To;
    const UNICODE_STRING *Source[6];
    WOW64_STRING *Target[6];
    SIZE_T Size = sizeof(*To), Zero = 0;
    PUCHAR Next;
    PVOID Block = NULL;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG i;

    *Guest = 0;
    *GuestLength = 0;
    if (From->MessageType != MsV1_0InteractiveProfile)
        goto Free;
    Source[0] = &From->LogonScript;
    Source[1] = &From->HomeDirectory;
    Source[2] = &From->FullName;
    Source[3] = &From->ProfilePath;
    Source[4] = &From->HomeDirectoryDrive;
    Source[5] = &From->LogonServer;
    for (i = 0; i < 6; ++i)
        Size += ALIGN_UP_BY(Source[i]->MaximumLength, sizeof(ULONG));
    Status = Wow64Allocate(Size, &Block);
    if (!NT_SUCCESS(Status)) goto Free;
    To = Block;
    To->MessageType = From->MessageType;
    To->LogonCount = From->LogonCount;
    To->BadPasswordCount = From->BadPasswordCount;
    To->LogonTime = From->LogonTime.QuadPart;
    To->LogoffTime = From->LogoffTime.QuadPart;
    To->KickOffTime = From->KickOffTime.QuadPart;
    To->PasswordLastSet = From->PasswordLastSet.QuadPart;
    To->PasswordCanChange = From->PasswordCanChange.QuadPart;
    To->PasswordMustChange = From->PasswordMustChange.QuadPart;
    To->UserFlags = From->UserFlags;
    Target[0] = &To->LogonScript;
    Target[1] = &To->HomeDirectory;
    Target[2] = &To->FullName;
    Target[3] = &To->ProfilePath;
    Target[4] = &To->HomeDirectoryDrive;
    Target[5] = &To->LogonServer;
    Next = (PUCHAR)(To + 1);
    for (i = 0; i < 6; ++i)
    {
        /* lsass wrote the profile, but the copy never reads past a string's own buffer */
        Target[i]->Length = min(Source[i]->Length, Source[i]->MaximumLength);
        Target[i]->MaximumLength = Source[i]->MaximumLength;
        if (Source[i]->Buffer)
        {
            Target[i]->Buffer = PtrToUlong(Next);
            RtlCopyMemory(Next, Source[i]->Buffer, Target[i]->Length);
            Next += ALIGN_UP_BY(Source[i]->MaximumLength, sizeof(ULONG));
        }
    }
    *Guest = PtrToUlong(Block);
    *GuestLength = (ULONG)Size;
Free:
    NtFreeVirtualMemory(NtCurrentProcess(), &Native, &Zero, MEM_RELEASE);
    return Status;
}

static ULONG Saturate(SIZE_T Value)
{
    return Value > MAXULONG ? MAXULONG : (ULONG)Value;
}

static NTSTATUS RequestLogonUser(ULONG Number, HANDLE Port, UCHAR *Message, ULONG DataLength, ULONG TotalLength,
                                 LSA_API_MSG *Native)
{
    WOW64_LSA_BODY *Body = (WOW64_LSA_BODY *)(Message + sizeof(WOW64_PORT_MESSAGE));
    WOW64_LSA_LOGON_REQUEST Request;
    WOW64_LSA_LOGON_REPLY Reply;
    PVOID AuthInfo = NULL;
    PTOKEN_GROUPS Groups = NULL;
    ULONG AuthInfoLength = 0;
    NTSTATUS Status, Result, Written = STATUS_SUCCESS;

    if (DataLength < FIELD_OFFSET(WOW64_LSA_BODY, LogonRequest) + sizeof(Request) ||
        TotalLength < sizeof(WOW64_PORT_MESSAGE) + FIELD_OFFSET(WOW64_LSA_BODY, LogonReply) + sizeof(Reply))
        return STATUS_INVALID_PARAMETER; /* checked before the logon, the reply must fit in the message */
    RtlCopyMemory(&Request, &Body->LogonRequest, sizeof(Request));

    if (Request.AuthenticationInformationLength)
    {
        Status = InteractiveLogonFrom32(Number, Request.AuthenticationInformation,
                                        Request.AuthenticationInformationLength, &AuthInfo, &AuthInfoLength);
        if (!NT_SUCCESS(Status)) return Status;
    }
    Status = GroupsFrom32(Request.LocalGroups, Request.LocalGroupsCount, &Groups);
    if (!NT_SUCCESS(Status)) goto Done;
    if (Request.OriginName.Length > Request.OriginName.MaximumLength ||
        (Request.OriginName.Length && !InGuest(Request.OriginName.Buffer, Request.OriginName.Length)))
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    RtlZeroMemory(Native, sizeof(*Native));
    Native->ApiNumber = LSASS_REQUEST_LOGON_USER;
    Native->h.u1.s1.DataLength = LSA_PORT_DATA_SIZE(Native->LogonUser);
    Native->h.u1.s1.TotalLength = LSA_PORT_MESSAGE_SIZE;
    Native->LogonUser.Request.OriginName.Length = Request.OriginName.Length;
    Native->LogonUser.Request.OriginName.MaximumLength = Request.OriginName.MaximumLength;
    Native->LogonUser.Request.OriginName.Buffer = PTR32(Request.OriginName.Buffer);
    Native->LogonUser.Request.LogonType = Request.LogonType;
    Native->LogonUser.Request.AuthenticationPackage = Request.AuthenticationPackage;
    Native->LogonUser.Request.AuthenticationInformation = AuthInfo;
    Native->LogonUser.Request.AuthenticationInformationLength = AuthInfoLength;
    Native->LogonUser.Request.LocalGroups = Groups;
    Native->LogonUser.Request.LocalGroupsCount = Request.LocalGroupsCount;
    Native->LogonUser.Request.SourceContext = Request.SourceContext;

    Status = NtRequestWaitReplyPort(Port, &Native->h, &Native->h);
    if (!NT_SUCCESS(Status)) goto Done;

    RtlZeroMemory(&Reply, sizeof(Reply));
    Reply.SubStatus = Native->LogonUser.Reply.SubStatus;
    Result = Native->Status;
    if (NT_SUCCESS(Result) && Native->LogonUser.Reply.ProfileBuffer)
        Result = ProfileTo32(Native->LogonUser.Reply.ProfileBuffer, &Reply.ProfileBuffer, &Reply.ProfileBufferLength);
    if (NT_SUCCESS(Result))
    {
        Reply.LogonId = Native->LogonUser.Reply.LogonId;
        Reply.Token = HandleToUlong(Native->LogonUser.Reply.Token);
        Reply.Quotas[0] = Saturate(Native->LogonUser.Reply.Quotas.PagedPoolLimit);
        Reply.Quotas[1] = Saturate(Native->LogonUser.Reply.Quotas.NonPagedPoolLimit);
        Reply.Quotas[2] = Saturate(Native->LogonUser.Reply.Quotas.MinimumWorkingSetSize);
        Reply.Quotas[3] = Saturate(Native->LogonUser.Reply.Quotas.MaximumWorkingSetSize);
        Reply.Quotas[4] = Saturate(Native->LogonUser.Reply.Quotas.PagefileLimit);
        Reply.TimeLimit = Native->LogonUser.Reply.Quotas.TimeLimit.QuadPart;
    }
    else if (NT_SUCCESS(Native->Status) && Native->LogonUser.Reply.Token)
    {
        /* The logon succeeded but its reply could not be converted: the guest never sees the token */
        NtClose(Native->LogonUser.Reply.Token);
    }
    _SEH2_TRY
    {
        Body->Status = Result;
        RtlCopyMemory(&Body->LogonReply, &Reply, sizeof(Reply));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Written = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Written))
    {
        /* The guest cannot receive the reply: it must not be left holding nothing but a token and a profile */
        if (NT_SUCCESS(Result))
        {
            SIZE_T Zero = 0;
            PVOID Profile = UlongToPtr(Reply.ProfileBuffer);
            if (Reply.Token) NtClose(HANDLE32(Reply.Token));
            if (Profile) NtFreeVirtualMemory(NtCurrentProcess(), &Profile, &Zero, MEM_RELEASE);
        }
        Status = Written;
    }
    else
        Status = STATUS_SUCCESS;

Done:
    if (Groups) RtlFreeHeap(RtlGetProcessHeap(), 0, Groups);
    if (AuthInfo)
    {
        RtlSecureZeroMemory(AuthInfo, AuthInfoLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, AuthInfo);
    }
    return Status;
}

/* NtRequestWaitReplyPort(PortHandle, PPORT_MESSAGE LpcRequest, PPORT_MESSAGE LpcReply): the request
 * and the reply of an LSA call share one message, as lsalib sends it. */
NTSTATUS Wow64LpcRequest(ULONG Number, ULONG Handle, ULONG Request, ULONG Reply, BOOLEAN *Reported)
{
    WOW64_PORT_MESSAGE Header;
    WOW64_LSA_BODY *Body;
    LSA_API_MSG Native;
    ULONG DataLength, Api;
    NTSTATUS Status;

    if (!IsLsaPort(Handle) || Request != Reply)
        return Wow64Unsupported(Number, WOW64_UNSUPPORTED_SERVICE, 2, Reported);
    if (!InGuest(Request, sizeof(Header) + FIELD_OFFSET(WOW64_LSA_BODY, LogonRequest)))
        return STATUS_ACCESS_VIOLATION;
    RtlCopyMemory(&Header, PTR32(Request), sizeof(Header));
    DataLength = Header.DataLength;
    if (Header.TotalLength < sizeof(Header) + DataLength || !InGuest(Request, (ULONG64)Header.TotalLength) ||
        DataLength < FIELD_OFFSET(WOW64_LSA_BODY, LogonRequest))
        return STATUS_INVALID_PARAMETER;
    Body = (WOW64_LSA_BODY *)((PUCHAR)PTR32(Request) + sizeof(Header));
    Api = Body->ApiNumber;

    switch (Api)
    {
    case LSASS_REQUEST_LOGON_USER:
        Status = RequestLogonUser(Number, HANDLE32(Handle), PTR32(Request), DataLength, Header.TotalLength, &Native);
        break;
    case LSASS_REQUEST_LOOKUP_AUTHENTICATION_PACKAGE:
        if (DataLength < FIELD_OFFSET(WOW64_LSA_BODY, LookupRequest) + sizeof(Body->LookupRequest))
            return STATUS_INVALID_PARAMETER;
        RtlZeroMemory(&Native, sizeof(Native));
        Native.ApiNumber = Api;
        Native.h.u1.s1.DataLength = LSA_PORT_DATA_SIZE(Native.LookupAuthenticationPackage);
        Native.h.u1.s1.TotalLength = LSA_PORT_MESSAGE_SIZE;
        Native.LookupAuthenticationPackage.Request.PackageNameLength = Body->LookupRequest.PackageNameLength;
        RtlCopyMemory(Native.LookupAuthenticationPackage.Request.PackageName, Body->LookupRequest.PackageName,
                      sizeof(Native.LookupAuthenticationPackage.Request.PackageName));
        Status = NtRequestWaitReplyPort(HANDLE32(Handle), &Native.h, &Native.h);
        if (NT_SUCCESS(Status))
        {
            Body->Status = Native.Status;
            Body->LookupReply.Package = Native.LookupAuthenticationPackage.Reply.Package;
        }
        break;
    case LSASS_REQUEST_DEREGISTER_LOGON_PROCESS:
        RtlZeroMemory(&Native, sizeof(Native));
        Native.ApiNumber = Api;
        Native.h.u1.s1.DataLength = LSA_PORT_DATA_SIZE(Native.DeregisterLogonProcess);
        Native.h.u1.s1.TotalLength = LSA_PORT_MESSAGE_SIZE;
        Status = NtRequestWaitReplyPort(HANDLE32(Handle), &Native.h, &Native.h);
        if (NT_SUCCESS(Status))
            Body->Status = Native.Status;
        break;
    default:
        /* LsaCallAuthenticationPackage carries package specific buffers with pointers of their own */
        return Wow64Unsupported(Number, WOW64_UNSUPPORTED_CLASS, Api, Reported);
    }

    if (NT_SUCCESS(Status))
    {
        WOW64_PORT_MESSAGE *Out = PTR32(Request);
        Out->Type = Native.h.u2.s2.Type;
        Out->ProcessId = HandleToUlong(Native.h.ClientId.UniqueProcess);
        Out->ThreadId = HandleToUlong(Native.h.ClientId.UniqueThread);
        Out->MessageId = Native.h.MessageId;
    }
    return Status;
}
