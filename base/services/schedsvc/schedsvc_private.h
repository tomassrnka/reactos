/*
 * Copyright 2014 Dmitry Timoshkov
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

#ifndef __WINE_SCHEDSVC_PRIVATE_H__
#define __WINE_SCHEDSVC_PRIVATE_H__

void schedsvc_auto_start(void);
void add_job(const WCHAR *name);
void remove_job(const WCHAR *name);
void check_task_state(void);
void add_process_to_queue(HANDLE hproc);
void update_process_status(DWORD pid);
BOOL get_next_runtime(LARGE_INTEGER *rt);
void check_task_time(void);
void load_at_tasks(void);
void check_missed_task_time(void);
#ifdef __REACTOS__
/* the caller of an RPC call, from its impersonation token */
struct caller
{
    WCHAR *sid;         /* string SID */
    BOOL privileged;    /* LocalSystem or an administrator */
};

HRESULT schedsvc_begin_call(struct caller *caller);
void schedsvc_end_call(struct caller *caller);
HRESULT schedsvc_check_owner(const struct caller *caller, const WCHAR *path);
HRESULT schedsvc_check_quota(const struct caller *caller);
HRESULT schedsvc_check_folder_quota(const struct caller *caller, unsigned int n_new);
HRESULT schedsvc_folder_recorded(const struct caller *caller, const WCHAR *path);
void schedsvc_folder_forgotten(const WCHAR *path);
HRESULT schedsvc_validate_xml(const WCHAR *xml);
void schedsvc_lock(void);
void schedsvc_unlock(void);
BOOL schedsvc_binding_is_local(RPC_BINDING_HANDLE binding);
RPC_STATUS CALLBACK schedsvc_if_callback(RPC_IF_HANDLE iface, void *context);
HRESULT schedsvc_task_registered(const struct caller *caller, const WCHAR *path, const WCHAR *xml);
HRESULT schedsvc_task_deleted(const WCHAR *path);
HRESULT schedsvc_run_task(const struct caller *caller, const WCHAR *path, GUID *guid);
BOOL schedsvc_valid_path(const WCHAR *path);
LONG schedsvc_log_fault(EXCEPTION_POINTERS *info);
HRESULT schedsvc_open_xml(const WCHAR *xml, IStream **stream);
HRESULT schedsvc_task_update_xml(const WCHAR *path, const WCHAR *xml);
HRESULT schedsvc_task_get_xml(const WCHAR *path, WCHAR **xml);
BOOL schedsvc_task_running(const WCHAR *path);
HRESULT schedsvc_get_last_run(const struct caller *caller, const WCHAR *path, SYSTEMTIME *last, DWORD *result);
HRESULT schedsvc_scheduled_runtimes(const struct caller *caller, const WCHAR *path, const SYSTEMTIME *start,
                                    const SYSTEMTIME *end, DWORD n_requested, DWORD *n_runtimes,
                                    SYSTEMTIME **runtimes);
HRESULT schedsvc_enum_instances(const struct caller *caller, const WCHAR *path, DWORD *n_guids, GUID **guids);
HRESULT schedsvc_instance_info(const struct caller *caller, const GUID *guid, WCHAR **path, DWORD *state,
                               WCHAR **action, WCHAR **info, DWORD *n_guids, GUID **guids, DWORD *pid);
HRESULT schedsvc_stop(const struct caller *caller, const WCHAR *path, const GUID *guid);
/* the Windows-shaped copy of the task store in the registry (taskcache.c); best effort, never trusted */
void schedsvc_mirror_registered(const WCHAR *path, const WCHAR *xml);
void schedsvc_mirror_deleted(const WCHAR *path);
/* a primary token of the S4U logon of a local account (s4u.c) */
HANDLE schedsvc_s4u_token(PSID sid);
#endif

#endif /* __WINE_SCHEDSVC_PRIVATE_H__ */
