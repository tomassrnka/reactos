/*
 * PROJECT:     ReactOS Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Task Scheduler 2.0: the token of a LogonType "S4U" task
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * A task whose principal has LogonType S4U runs as its user without the user being logged on and
 * without a stored password: the service asks the LSA for a network logon of the user's local account
 * with MSV1_0_S4U_LOGON (the LSA checks that the account exists and is neither disabled nor expired,
 * nothing else). It may do so only because it is LocalSystem and registers as a logon process; the
 * caller of the RPC interface never gets near this: taskrun.c asks for a token only for the account a
 * task is allowed to run as (the registrant's own, or any account if an administrator registered it).
 *
 * The token has no network credentials and no interactive session, so the action runs on the service
 * window station and gets no window. Local accounts only: the LSA refuses any other domain.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#include <windef.h>
#include <winbase.h>
#include <ntsecapi.h>
#include <ntmsv1_0.h>

#include <wine/debug.h>

/* from taskrun.c and schedsvc_private.h, which needs RPC headers that ntsecapi.h does not mix with */
void schedsvc_lock(void);
void schedsvc_unlock(void);
HANDLE schedsvc_s4u_token(PSID sid);

WINE_DEFAULT_DEBUG_CHANNEL(schedsvc);

#define S4U_MESSAGE_TYPE 12     /* MsV1_0S4ULogon */

static HANDLE lsa;
static ULONG package;
static BOOL lsa_tried;

static BOOL connect_lsa(void)
{
    static CHAR process_name[] = "Schedule";
    static CHAR package_name[] = MSV1_0_PACKAGE_NAME;
    LSA_STRING process = { sizeof(process_name) - 1, sizeof(process_name), process_name };
    LSA_STRING package_string = { sizeof(package_name) - 1, sizeof(package_name), package_name };
    LSA_OPERATIONAL_MODE mode;
    NTSTATUS status;

    if (lsa) return TRUE;
    if (lsa_tried) return FALSE;
    lsa_tried = TRUE;

    status = LsaRegisterLogonProcess(&process, &lsa, &mode);
    if (status < 0)
    {
        ERR("LsaRegisterLogonProcess failed (0x%08lx)\n", status);
        lsa = NULL;
        return FALSE;
    }
    status = LsaLookupAuthenticationPackage(lsa, &package_string, &package);
    if (status < 0)
    {
        ERR("LsaLookupAuthenticationPackage failed (0x%08lx)\n", status);
        LsaDeregisterLogonProcess(lsa);
        lsa = NULL;
        return FALSE;
    }
    return TRUE;
}

/* a primary token of the S4U logon of the local account with this SID, or NULL */
HANDLE schedsvc_s4u_token(PSID sid)
{
    WCHAR user[256], domain[256], host[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD user_len = ARRAYSIZE(user), domain_len = ARRAYSIZE(domain), host_len = ARRAYSIZE(host);
    SID_NAME_USE use;
    MSV1_0_S4U_LOGON *logon;
    SIZE_T user_bytes, domain_bytes, size;
    TOKEN_SOURCE source;
    LSA_STRING origin;
    static CHAR origin_name[] = "Task Scheduler";
    PVOID profile = NULL;
    ULONG profile_len = 0;
    LUID luid;
    HANDLE token = NULL, primary = NULL;
    QUOTA_LIMITS quotas;
    NTSTATUS status, sub_status = 0;

    if (!LookupAccountSidW(NULL, sid, user, &user_len, domain, &domain_len, &use) || use != SidTypeUser)
    {
        WARN("S4U: no user account for the principal (%lu)\n", GetLastError());
        return NULL;
    }
    if (!GetComputerNameW(host, &host_len)) return NULL;
    /* local accounts only */
    if (wcsicmp(domain, host))
    {
        WARN("S4U: %s\\%s is not a local account\n", debugstr_w(domain), debugstr_w(user));
        return NULL;
    }

    schedsvc_lock();
    if (!connect_lsa())
    {
        schedsvc_unlock();
        return NULL;
    }

    user_bytes = wcslen(user) * sizeof(WCHAR);
    domain_bytes = wcslen(host) * sizeof(WCHAR);
    size = sizeof(*logon) + user_bytes + domain_bytes;
    if (!(logon = calloc(1, size)))
    {
        schedsvc_unlock();
        return NULL;
    }
    logon->MessageType = S4U_MESSAGE_TYPE;
    logon->UserPrincipalName.Buffer = (PWSTR)(logon + 1);
    logon->UserPrincipalName.Length = logon->UserPrincipalName.MaximumLength = user_bytes;
    memcpy(logon->UserPrincipalName.Buffer, user, user_bytes);
    logon->DomainName.Buffer = (PWSTR)((BYTE *)(logon + 1) + user_bytes);
    logon->DomainName.Length = logon->DomainName.MaximumLength = domain_bytes;
    memcpy(logon->DomainName.Buffer, host, domain_bytes);

    memset(&source, 0, sizeof(source));
    memcpy(source.SourceName, "Schedule", 8);
    AllocateLocallyUniqueId(&source.SourceIdentifier);
    origin.Length = sizeof(origin_name) - 1;
    origin.MaximumLength = sizeof(origin_name);
    origin.Buffer = origin_name;

    status = LsaLogonUser(lsa, &origin, Network, package, logon, size, NULL, &source, &profile, &profile_len,
                          &luid, &token, &quotas, &sub_status);
    schedsvc_unlock();
    free(logon);
    if (profile) LsaFreeReturnBuffer(profile);
    if (status < 0)
    {
        WARN("S4U logon of %s failed (0x%08lx, 0x%08lx)\n", debugstr_w(user), status, sub_status);
        return NULL;
    }
    if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &primary))
    {
        WARN("S4U: no primary token (%lu)\n", GetLastError());
        primary = NULL;
    }
    CloseHandle(token);
    return primary;
}
