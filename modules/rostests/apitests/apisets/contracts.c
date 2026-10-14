/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     A contract must not hide functions that its home module exports
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <string.h>

/*
 * Every contract below is hosted in kernelbase.dll, sechost.dll or a similar module on Windows, and its functions
 * live in the home module on ReactOS. The function names are those Windows has behind the contract: the forwarders of
 * the downlevel stubs of Windows 10, of kernel32 on Windows 7, the import libraries of the contracts and the imports of
 * real applications. A function that the home module does not export, because its export level withholds it, is not
 * checked.
 */
typedef struct _CONTRACT_DATA
{
    const char *Home;
    const char *Contracts;
    const char *Functions;
} CONTRACT_DATA;

static const CONTRACT_DATA g_Data[] =
{
    {
        "dbghelp.dll",
        "api-ms-win-core-debug-minidump-l1-1-0.dll\0",
        "MiniDumpReadDumpStream\0MiniDumpWriteDump\0"
    },
    {
        "advapi32.dll",
        "api-MS-Win-Core-LocalRegistry-L1-1-0.dll\0",
        "RegCloseKey\0RegCreateKeyExA\0RegCreateKeyExW\0RegDeleteKeyExA\0RegDeleteKeyExW\0RegDeleteTreeA\0"
        "RegDeleteTreeW\0RegDeleteValueA\0RegDeleteValueW\0RegDisablePredefinedCacheEx\0RegEnumKeyExA\0"
        "RegEnumKeyExW\0RegEnumValueA\0RegEnumValueW\0RegFlushKey\0RegGetKeySecurity\0RegGetValueA\0"
        "RegGetValueW\0RegLoadKeyA\0RegLoadKeyW\0RegLoadMUIStringA\0RegLoadMUIStringW\0"
        "RegNotifyChangeKeyValue\0RegOpenCurrentUser\0RegOpenKeyExA\0RegOpenKeyExW\0RegOpenUserClassesRoot\0"
        "RegQueryInfoKeyA\0RegQueryInfoKeyW\0RegQueryValueExA\0RegQueryValueExW\0RegRestoreKeyA\0"
        "RegRestoreKeyW\0RegSaveKeyExA\0RegSaveKeyExW\0RegSetKeySecurity\0RegSetValueExA\0RegSetValueExW\0"
        "RegUnLoadKeyA\0RegUnLoadKeyW\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-core-registry-l1-1-0.dll\0api-ms-win-core-registry-l1-1-2.dll\0",
        "RegCloseKey\0RegCopyTreeW\0RegCreateKeyExA\0RegCreateKeyExW\0RegDeleteKeyExA\0RegDeleteKeyExW\0"
        "RegDeleteKeyValueA\0RegDeleteKeyValueW\0RegDeleteTreeA\0RegDeleteTreeW\0RegDeleteValueA\0"
        "RegDeleteValueW\0RegDisablePredefinedCacheEx\0RegEnumKeyExA\0RegEnumKeyExW\0RegEnumValueA\0"
        "RegEnumValueW\0RegFlushKey\0RegGetKeySecurity\0RegGetValueA\0RegGetValueW\0RegLoadAppKeyA\0"
        "RegLoadAppKeyW\0RegLoadKeyA\0RegLoadKeyW\0RegLoadMUIStringA\0RegLoadMUIStringW\0"
        "RegNotifyChangeKeyValue\0RegOpenCurrentUser\0RegOpenKeyExA\0RegOpenKeyExW\0RegOpenUserClassesRoot\0"
        "RegQueryInfoKeyA\0RegQueryInfoKeyW\0RegQueryMultipleValuesA\0RegQueryMultipleValuesW\0"
        "RegQueryValueExA\0RegQueryValueExW\0RegRestoreKeyA\0RegRestoreKeyW\0RegSaveKeyExA\0RegSaveKeyExW\0"
        "RegSetKeySecurity\0RegSetKeyValueA\0RegSetKeyValueW\0RegSetValueExA\0RegSetValueExW\0RegUnLoadKeyA\0"
        "RegUnLoadKeyW\0"
    },
    {
        "shlwapi.dll",
        "api-ms-win-core-shlwapi-legacy-l1-1-0.dll\0",
        "IsCharBlankW\0IsCharCntrlW\0IsCharDigitW\0IsCharPunctW\0IsCharSpaceA\0IsCharSpaceW\0IsCharXDigitW\0"
        "PathAddBackslashA\0PathAddBackslashW\0PathAddExtensionA\0PathAddExtensionW\0PathAppendA\0"
        "PathAppendW\0PathCanonicalizeA\0PathCanonicalizeW\0PathCombineA\0PathCombineW\0PathCommonPrefixA\0"
        "PathCommonPrefixW\0PathFileExistsA\0PathFileExistsW\0PathFindExtensionA\0PathFindExtensionW\0"
        "PathFindFileNameA\0PathFindFileNameW\0PathFindNextComponentA\0PathFindNextComponentW\0PathGetArgsA\0"
        "PathGetArgsW\0PathGetCharTypeA\0PathGetCharTypeW\0PathGetDriveNumberA\0PathGetDriveNumberW\0"
        "PathIsFileSpecA\0PathIsFileSpecW\0PathIsLFNFileSpecA\0PathIsLFNFileSpecW\0PathIsPrefixA\0"
        "PathIsPrefixW\0PathIsRelativeA\0PathIsRelativeW\0PathIsRootA\0PathIsRootW\0PathIsSameRootA\0"
        "PathIsSameRootW\0PathIsUNCA\0PathIsUNCServerA\0PathIsUNCServerShareA\0PathIsUNCServerShareW\0"
        "PathIsUNCServerW\0PathIsUNCW\0PathIsValidCharA\0PathIsValidCharW\0PathMatchSpecA\0PathMatchSpecExA\0"
        "PathMatchSpecExW\0PathMatchSpecW\0PathParseIconLocationA\0PathParseIconLocationW\0PathQuoteSpacesA\0"
        "PathQuoteSpacesW\0PathRelativePathToA\0PathRelativePathToW\0PathRemoveBackslashA\0"
        "PathRemoveBackslashW\0PathRemoveBlanksA\0PathRemoveBlanksW\0PathRemoveExtensionA\0"
        "PathRemoveExtensionW\0PathRemoveFileSpecA\0PathRemoveFileSpecW\0PathRenameExtensionA\0"
        "PathRenameExtensionW\0PathSearchAndQualifyA\0PathSearchAndQualifyW\0PathSkipRootA\0PathSkipRootW\0"
        "PathStripPathA\0PathStripPathW\0PathStripToRootA\0PathStripToRootW\0PathUnExpandEnvStringsA\0"
        "PathUnExpandEnvStringsW\0PathUnquoteSpacesA\0PathUnquoteSpacesW\0SHExpandEnvironmentStringsA\0"
        "SHExpandEnvironmentStringsW\0SHTruncateString\0"
    },
    {
        "shlwapi.dll",
        "api-ms-win-core-shlwapi-obsolete-l1-1-0.dll\0",
        "QISearch\0SHLoadIndirectString\0StrCSpnA\0StrCSpnIA\0StrCSpnIW\0StrCSpnW\0StrCatBuffA\0StrCatBuffW\0"
        "StrCatChainW\0StrChrA\0StrChrIA\0StrChrIW\0StrChrNIW\0StrChrNW\0StrChrW\0StrCmpCA\0StrCmpCW\0"
        "StrCmpICA\0StrCmpICW\0StrCmpIW\0StrCmpLogicalW\0StrCmpNA\0StrCmpNCA\0StrCmpNCW\0StrCmpNIA\0"
        "StrCmpNICA\0StrCmpNICW\0StrCmpNIW\0StrCmpNW\0StrCmpW\0StrCpyNW\0StrCpyNXA\0StrCpyNXW\0StrDupA\0"
        "StrDupW\0StrIsIntlEqualA\0StrIsIntlEqualW\0StrPBrkA\0StrPBrkW\0StrRChrA\0StrRChrIA\0StrRChrIW\0"
        "StrRChrW\0StrRStrIA\0StrRStrIW\0StrSpnA\0StrSpnW\0StrStrA\0StrStrIA\0StrStrIW\0StrStrNIW\0StrStrNW\0"
        "StrStrW\0StrToInt64ExA\0StrToInt64ExW\0StrToIntA\0StrToIntExA\0StrToIntExW\0StrToIntW\0StrTrimA\0"
        "StrTrimW\0"
    },
    {
        "user32.dll",
        "api-ms-win-core-string-l2-1-0.dll\0api-ms-win-core-string-l2-1-1.dll\0",
        "CharLowerBuffW\0CharLowerW\0CharNextW\0CharPrevW\0CharUpperBuffW\0CharUpperW\0IsCharAlphaNumericW\0"
        "IsCharAlphaW\0IsCharLowerW\0IsCharUpperW\0SHLoadIndirectString\0"
    },
    {
        "user32.dll",
        "api-ms-win-core-stringansi-l1-1-0.dll\0",
        "CharLowerA\0CharLowerBuffA\0CharNextA\0CharNextExA\0CharPrevA\0CharPrevExA\0CharUpperA\0"
        "CharUpperBuffA\0IsCharAlphaA\0IsCharAlphaNumericA\0IsCharLowerA\0IsCharUpperA\0"
    },
    {
        "user32.dll",
        "api-ms-win-core-stringloader-l1-1-0.dll\0api-ms-win-core-stringloader-l1-1-1.dll\0",
        "LoadStringA\0LoadStringW\0"
    },
    {
        "shlwapi.dll",
        "api-ms-win-core-url-l1-1-0.dll\0",
        "GetAcceptLanguagesA\0GetAcceptLanguagesW\0HashData\0IsInternetESCEnabled\0ParseURLA\0ParseURLW\0"
        "PathCreateFromUrlA\0PathCreateFromUrlAlloc\0PathCreateFromUrlW\0PathIsURLA\0PathIsURLW\0"
        "UrlApplySchemeA\0UrlApplySchemeW\0UrlCanonicalizeA\0UrlCanonicalizeW\0UrlCombineA\0UrlCombineW\0"
        "UrlCompareA\0UrlCompareW\0UrlCreateFromPathA\0UrlCreateFromPathW\0UrlEscapeA\0UrlEscapeW\0"
        "UrlFixupW\0UrlGetLocationA\0UrlGetLocationW\0UrlGetPartA\0UrlGetPartW\0UrlHashA\0UrlHashW\0UrlIsA\0"
        "UrlIsNoHistoryA\0UrlIsNoHistoryW\0UrlIsOpaqueA\0UrlIsOpaqueW\0UrlIsW\0UrlUnescapeA\0UrlUnescapeW\0"
    },
    {
        "version.dll",
        "api-ms-win-core-version-l1-1-0.dll\0api-ms-win-core-version-l1-1-1.dll\0",
        "GetFileVersionInfoExW\0GetFileVersionInfoSizeExW\0GetFileVersionInfoSizeW\0GetFileVersionInfoW\0"
        "VerFindFileW\0VerQueryValueW\0"
    },
    {
        "version.dll",
        "api-ms-win-core-versionansi-l1-1-0.dll\0api-ms-win-core-versionansi-l1-1-1.dll\0",
        "GetFileVersionInfoA\0GetFileVersionInfoExA\0GetFileVersionInfoSizeA\0GetFileVersionInfoSizeExA\0"
        "VerFindFileA\0VerQueryValueA\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-eventing-classicprovider-l1-1-0.dll\0",
        "GetTraceEnableFlags\0GetTraceEnableLevel\0GetTraceLoggerHandle\0RegisterTraceGuidsW\0TraceEvent\0"
        "TraceMessage\0TraceMessageVa\0UnregisterTraceGuids\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-eventing-consumer-l1-1-0.dll\0api-ms-win-eventing-consumer-l1-1-1.dll\0",
        "CloseTrace\0OpenTraceW\0ProcessTrace\0QueryTraceProcessingHandle\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-eventing-controller-l1-1-0.dll\0",
        "ControlTraceW\0EnableTraceEx2\0EnumerateTraceGuidsEx\0EventAccessControl\0EventAccessQuery\0"
        "EventAccessRemove\0QueryAllTracesW\0StartTraceW\0StopTraceW\0TraceQueryInformation\0"
        "TraceSetInformation\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-eventing-obsolete-l1-1-0.dll\0",
        "RegisterTraceGuidsA\0RemoveTraceCallback\0SetTraceCallback\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-eventing-provider-l1-1-0.dll\0",
        "EventActivityIdControl\0EventEnabled\0EventProviderEnabled\0EventRegister\0EventSetInformation\0"
        "EventUnregister\0EventWrite\0EventWriteEx\0EventWriteString\0EventWriteTransfer\0"
    },
    {
        "winmm.dll",
        "api-ms-win-mm-time-l1-1-0.dll\0",
        "timeBeginPeriod\0timeEndPeriod\0timeGetDevCaps\0timeGetSystemTime\0timeGetTime\0"
    },
    {
        "advapi32.dll",
        "api-MS-Win-Security-Base-L1-1-0.dll\0api-ms-win-security-base-l1-1-1.dll\0",
        "AccessCheck\0AccessCheckAndAuditAlarmW\0AccessCheckByType\0AccessCheckByTypeAndAuditAlarmW\0"
        "AccessCheckByTypeResultList\0AccessCheckByTypeResultListAndAuditAlarmByHandleW\0"
        "AccessCheckByTypeResultListAndAuditAlarmW\0AddAccessAllowedAce\0AddAccessAllowedAceEx\0"
        "AddAccessAllowedObjectAce\0AddAccessDeniedAce\0AddAccessDeniedAceEx\0AddAccessDeniedObjectAce\0"
        "AddAce\0AddAuditAccessAce\0AddAuditAccessAceEx\0AddAuditAccessObjectAce\0AddMandatoryAce\0"
        "AdjustTokenGroups\0AdjustTokenPrivileges\0AllocateAndInitializeSid\0AllocateLocallyUniqueId\0"
        "AreAllAccessesGranted\0AreAnyAccessesGranted\0CheckTokenMembership\0"
        "ConvertToAutoInheritPrivateObjectSecurity\0CopySid\0CreatePrivateObjectSecurity\0"
        "CreatePrivateObjectSecurityEx\0CreatePrivateObjectSecurityWithMultipleInheritance\0"
        "CreateRestrictedToken\0CreateWellKnownSid\0DeleteAce\0DestroyPrivateObjectSecurity\0DuplicateToken\0"
        "DuplicateTokenEx\0EqualDomainSid\0EqualPrefixSid\0EqualSid\0FindFirstFreeAce\0FreeSid\0GetAce\0"
        "GetAclInformation\0GetFileSecurityW\0GetKernelObjectSecurity\0GetLengthSid\0"
        "GetPrivateObjectSecurity\0GetSecurityDescriptorControl\0GetSecurityDescriptorDacl\0"
        "GetSecurityDescriptorGroup\0GetSecurityDescriptorLength\0GetSecurityDescriptorOwner\0"
        "GetSecurityDescriptorRMControl\0GetSecurityDescriptorSacl\0GetSidIdentifierAuthority\0"
        "GetSidLengthRequired\0GetSidSubAuthority\0GetSidSubAuthorityCount\0GetTokenInformation\0"
        "GetWindowsAccountDomainSid\0ImpersonateAnonymousToken\0ImpersonateLoggedOnUser\0ImpersonateSelf\0"
        "InitializeAcl\0InitializeSecurityDescriptor\0InitializeSid\0IsTokenRestricted\0IsValidAcl\0"
        "IsValidRelativeSecurityDescriptor\0IsValidSecurityDescriptor\0IsValidSid\0IsWellKnownSid\0"
        "MakeAbsoluteSD\0MakeAbsoluteSD2\0MakeSelfRelativeSD\0MapGenericMask\0ObjectCloseAuditAlarmW\0"
        "ObjectDeleteAuditAlarmW\0ObjectOpenAuditAlarmW\0ObjectPrivilegeAuditAlarmW\0PrivilegeCheck\0"
        "PrivilegedServiceAuditAlarmW\0QuerySecurityAccessMask\0RevertToSelf\0SetAclInformation\0"
        "SetFileSecurityW\0SetKernelObjectSecurity\0SetPrivateObjectSecurity\0SetPrivateObjectSecurityEx\0"
        "SetSecurityAccessMask\0SetSecurityDescriptorControl\0SetSecurityDescriptorDacl\0"
        "SetSecurityDescriptorGroup\0SetSecurityDescriptorOwner\0SetSecurityDescriptorRMControl\0"
        "SetSecurityDescriptorSacl\0SetTokenInformation\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-security-credentials-l1-1-0.dll\0",
        "CredDeleteA\0CredDeleteW\0CredEnumerateA\0CredEnumerateW\0CredFindBestCredentialA\0"
        "CredFindBestCredentialW\0CredFree\0CredGetSessionTypes\0CredGetTargetInfoA\0CredGetTargetInfoW\0"
        "CredIsMarshaledCredentialW\0CredIsProtectedA\0CredIsProtectedW\0CredMarshalCredentialA\0"
        "CredMarshalCredentialW\0CredProtectA\0CredProtectW\0CredReadA\0CredReadDomainCredentialsA\0"
        "CredReadDomainCredentialsW\0CredReadW\0CredUnmarshalCredentialA\0CredUnmarshalCredentialW\0"
        "CredUnprotectA\0CredUnprotectW\0CredWriteA\0CredWriteDomainCredentialsA\0"
        "CredWriteDomainCredentialsW\0CredWriteW\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-security-cryptoapi-l1-1-0.dll\0",
        "CryptAcquireContextA\0CryptAcquireContextW\0CryptContextAddRef\0CryptCreateHash\0CryptDecrypt\0"
        "CryptDeriveKey\0CryptDestroyHash\0CryptDestroyKey\0CryptDuplicateHash\0CryptDuplicateKey\0"
        "CryptEncrypt\0CryptEnumProviderTypesA\0CryptEnumProviderTypesW\0CryptEnumProvidersA\0"
        "CryptEnumProvidersW\0CryptExportKey\0CryptGenKey\0CryptGenRandom\0CryptGetDefaultProviderA\0"
        "CryptGetDefaultProviderW\0CryptGetHashParam\0CryptGetKeyParam\0CryptGetProvParam\0CryptGetUserKey\0"
        "CryptHashData\0CryptHashSessionKey\0CryptImportKey\0CryptReleaseContext\0CryptSetHashParam\0"
        "CryptSetKeyParam\0CryptSetProvParam\0CryptSetProviderA\0CryptSetProviderExA\0CryptSetProviderExW\0"
        "CryptSetProviderW\0CryptSignHashA\0CryptSignHashW\0CryptVerifySignatureA\0CryptVerifySignatureW\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-security-lsapolicy-l1-1-0.dll\0api-ms-win-security-lsapolicy-l1-1-1.dll\0",
        "LsaAddAccountRights\0LsaClose\0LsaCreateSecret\0LsaEnumerateAccountRights\0"
        "LsaEnumerateAccountsWithUserRight\0LsaFreeMemory\0LsaICLookupNames\0LsaICLookupNamesWithCreds\0"
        "LsaICLookupSids\0LsaICLookupSidsWithCreds\0LsaLookupNames2\0LsaLookupSids\0LsaLookupSids2\0"
        "LsaOpenPolicy\0LsaOpenSecret\0LsaQueryInformationPolicy\0LsaQuerySecret\0LsaRemoveAccountRights\0"
        "LsaRetrievePrivateData\0LsaSetInformationPolicy\0LsaSetSecret\0LsaStorePrivateData\0"
    },
    {
        "advapi32.dll",
        "api-MS-Win-Security-SDDL-L1-1-0.dll\0",
        "ConvertSecurityDescriptorToStringSecurityDescriptorW\0ConvertSidToStringSidW\0"
        "ConvertStringSecurityDescriptorToSecurityDescriptorW\0ConvertStringSidToSidW\0"
    },
    {
        "advapi32.dll",
        "api-MS-Win-Service-Core-L1-1-0.dll\0",
        "RegisterServiceCtrlHandlerExW\0SetServiceStatus\0StartServiceCtrlDispatcherW\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-service-core-l1-1-1.dll\0api-ms-win-service-core-l1-1-4.dll\0",
        "EnumDependentServicesW\0EnumServicesStatusExW\0GetServiceDirectory\0GetServiceDisplayNameW\0"
        "GetServiceKeyNameW\0GetServiceRegistryStateKey\0QueryServiceDynamicInformation\0"
        "RegisterServiceCtrlHandlerExW\0SetServiceStatus\0StartServiceCtrlDispatcherW\0"
    },
    {
        "advapi32.dll",
        "api-MS-Win-Service-Management-L1-1-0.dll\0",
        "CloseServiceHandle\0ControlServiceExW\0CreateServiceW\0DeleteService\0OpenSCManagerW\0OpenServiceW\0"
        "StartServiceW\0"
    },
    {
        "advapi32.dll",
        "api-MS-Win-Service-Management-L2-1-0.dll\0",
        "ChangeServiceConfig2W\0ChangeServiceConfigW\0NotifyServiceStatusChangeW\0QueryServiceConfig2W\0"
        "QueryServiceConfigW\0QueryServiceObjectSecurity\0QueryServiceStatusEx\0SetServiceObjectSecurity\0"
    },
    {
        "advapi32.dll",
        "api-ms-win-service-private-l1-1-0.dll\0api-ms-win-service-private-l1-1-1.dll\0"
        "api-ms-win-service-private-l1-1-5.dll\0",
        "I_QueryTagInformation\0I_ScBroadcastServiceControlMessage\0I_ScIsSecurityProcess\0"
        "I_ScPnPGetServiceName\0I_ScQueryServiceConfig\0I_ScRegisterDeviceNotification\0"
        "I_ScRegisterPreshutdownRestart\0I_ScRpcBindA\0I_ScRpcBindW\0I_ScSendPnPMessage\0I_ScSendTSMessage\0"
        "I_ScSetServiceBitsA\0I_ScSetServiceBitsW\0I_ScUnregisterDeviceNotification\0I_ScValidatePnPService\0"
        "SubscribeServiceChangeNotifications\0UnsubscribeServiceChangeNotifications\0WaitServiceState\0"
    },
    {
        "advapi32.dll",
        "api-MS-Win-Service-winsvc-L1-1-0.dll\0",
        "ChangeServiceConfig2A\0ChangeServiceConfigA\0ControlService\0ControlServiceExA\0CreateServiceA\0"
        "I_QueryTagInformation\0I_ScBroadcastServiceControlMessage\0I_ScIsSecurityProcess\0"
        "I_ScPnPGetServiceName\0I_ScQueryServiceConfig\0I_ScRpcBindA\0I_ScRpcBindW\0I_ScSendPnPMessage\0"
        "I_ScSendTSMessage\0I_ScValidatePnPService\0NotifyServiceStatusChangeA\0OpenSCManagerA\0"
        "OpenServiceA\0QueryServiceConfig2A\0QueryServiceConfigA\0QueryServiceStatus\0"
        "RegisterServiceCtrlHandlerA\0RegisterServiceCtrlHandlerExA\0RegisterServiceCtrlHandlerW\0"
        "StartServiceA\0StartServiceCtrlDispatcherA\0"
    },
    {
        "shlwapi.dll",
        "api-ms-win-shcore-registry-l1-1-0.dll\0api-ms-win-shcore-registry-l1-1-1.dll\0",
        "SHCopyKeyA\0SHCopyKeyW\0SHDeleteEmptyKeyA\0SHDeleteEmptyKeyW\0SHDeleteKeyA\0SHDeleteKeyW\0"
        "SHDeleteValueA\0SHDeleteValueW\0SHEnumKeyExA\0SHEnumKeyExW\0SHEnumValueA\0SHEnumValueW\0"
        "SHGetValueA\0SHGetValueW\0SHQueryInfoKeyA\0SHQueryInfoKeyW\0SHQueryValueExA\0SHQueryValueExW\0"
        "SHRegDuplicateHKey\0SHRegGetIntW\0SHRegGetPathA\0SHRegGetPathW\0SHRegGetValueA\0"
        "SHRegGetValueFromHKCUHKLM\0SHRegGetValueW\0SHRegSetPathA\0SHRegSetPathW\0SHSetValueA\0SHSetValueW\0"
    },
    {
        "shlwapi.dll",
        "api-ms-win-shcore-stream-l1-1-0.dll\0",
        "IStream_Copy\0IStream_Read\0IStream_ReadStr\0IStream_Reset\0IStream_Size\0IStream_Write\0"
        "IStream_WriteStr\0SHCreateMemStream\0SHCreateStreamOnFileA\0SHCreateStreamOnFileEx\0"
        "SHCreateStreamOnFileW\0SHOpenRegStream2A\0SHOpenRegStream2W\0SHOpenRegStreamA\0SHOpenRegStreamW\0"
    },
    {
        "shlwapi.dll",
        "api-ms-win-shcore-unicodeansi-l1-1-0.dll\0",
        "SHAnsiToAnsi\0SHAnsiToUnicode\0SHUnicodeToAnsi\0SHUnicodeToUnicode\0"
    },
    {
        "shcore.dll",
        "api-ms-win-shcore-obsolete-l1-1-0.dll\0",
        "CommandLineToArgvW\0SHStrDupA\0SHStrDupW\0"
    },
    {
        "shcore.dll",
        "api-ms-win-shcore-stream-winrt-l1-1-0.dll\0",
        "CreateRandomAccessStreamOnFile\0CreateRandomAccessStreamOverStream\0CreateStreamOverRandomAccessStream\0"
    },
    {
        "shcore.dll",
        "api-ms-win-shcore-sysinfo-l1-1-0.dll\0",
        "GetCurrentProcessExplicitAppUserModelID\0IsOS\0SetCurrentProcessExplicitAppUserModelID\0"
    },
    {
        "shcore.dll",
        "api-ms-win-shcore-scaling-l1-1-0.dll\0api-ms-win-shcore-scaling-l1-1-1.dll\0api-ms-win-shcore-scaling-l1-1-2.dll\0",
        "GetDpiForMonitor\0GetProcessDpiAwareness\0GetScaleFactorForDevice\0GetScaleFactorForMonitor\0"
        "RegisterScaleChangeEvent\0RegisterScaleChangeNotifications\0RevokeScaleChangeNotifications\0"
        "SetProcessDpiAwareness\0UnregisterScaleChangeEvent\0"
    },
    {
        "user32.dll",
        "api-ms-win-rtcore-ntuser-dpi-l1-1-0.dll\0ext-ms-win-rtcore-ntuser-dpi-l1-1-0.dll\0"
        "ext-ms-win-rtcore-ntuser-dpi-l1-2-2.dll\0",
        "AdjustWindowRectExForDpi\0AreDpiAwarenessContextsEqual\0EnableNonClientDpiScaling\0"
        "GetAwarenessFromDpiAwarenessContext\0GetDpiForSystem\0GetDpiForWindow\0GetSystemMetricsForDpi\0"
        "GetThreadDpiAwarenessContext\0GetWindowDpiAwarenessContext\0IsProcessDPIAware\0"
        "IsValidDpiAwarenessContext\0SetProcessDpiAwarenessContext\0SetThreadDpiAwarenessContext\0"
        "SystemParametersInfoForDpi\0"
    },
    {
        "shell32.dll",
        "api-ms-win-shell-namespace-l1-1-0.dll\0",
        "SHCreateItemFromParsingName\0SHGetIDListFromObject\0"
    },
    {
        "shell32.dll",
        "ext-ms-win-shell32-shellfolders-l1-1-1.dll\0",
        "SHGetFileInfoW\0"
    },
    {
        "usp10.dll",
        "ext-ms-win-usp10-l1-1-0.dll\0",
        "ScriptBreak\0ScriptFreeCache\0ScriptGetCMap\0ScriptGetLogicalWidths\0ScriptGetProperties\0"
        "ScriptItemize\0ScriptPlace\0ScriptShape\0ScriptStringAnalyse\0ScriptStringFree\0"
        "ScriptStringValidate\0ScriptString_pLogAttr\0ScriptSubstituteSingleGlyph\0ScriptTextOut\0"
    },
};

static unsigned int g_Checked;

static void
test_contract(const CONTRACT_DATA *Data)
{
    const char *Contract, *Function;
    HMODULE Home = LoadLibraryA(Data->Home);

    if (!Home)
    {
        if (is_reactos())
            ok(0, "%s did not load: %lu\n", Data->Home, GetLastError());
        else
            skip("%s is not present\n", Data->Home);
        return;
    }

    for (Contract = Data->Contracts; *Contract; Contract += strlen(Contract) + 1)
    {
        HMODULE Api = LoadLibraryA(Contract);

        if (!Api)
        {
            if (is_reactos())
                ok(0, "%s did not load: %lu\n", Contract, GetLastError());
            else
                skip("%s is not present\n", Contract);
            continue;
        }

        for (Function = Data->Functions; *Function; Function += strlen(Function) + 1)
        {
            /* The home module does not export what the export level withholds */
            if (!GetProcAddress(Home, Function))
                continue;

            g_Checked++;
            ok(GetProcAddress(Api, Function) != NULL, "%s does not export %s, but %s does\n", Contract, Function, Data->Home);
        }
        FreeLibrary(Api);
    }
    FreeLibrary(Home);
}

/*
 * Windows 10 hosts every contract of the shcore family in shcore.dll (measured with LoadLibrary and GetModuleFileName on
 * Windows 10 22H2, x64 and WOW64). The contracts below have functions in ReactOS shcore, so they go there too. The
 * families path and taskpool are not listed: no ReactOS module exports their functions by the names Windows 10 shcore
 * uses (shlwapi has the path functions, shcore has only ordinal stubs).
 */
static const char *const g_ShcoreHosted[] =
{
    "api-ms-win-shcore-comhelpers-l1-1-0.dll",
    "api-ms-win-shcore-obsolete-l1-1-0.dll",
    "api-ms-win-shcore-scaling-l1-1-0.dll",
    "api-ms-win-shcore-scaling-l1-1-1.dll",
    "api-ms-win-shcore-scaling-l1-1-2.dll",
    "api-ms-win-shcore-stream-winrt-l1-1-0.dll",
    "api-ms-win-shcore-sysinfo-l1-1-0.dll",
    "api-ms-win-shcore-thread-l1-1-0.dll",
};

static BOOL
is_windows_10_or_later(void)
{
    typedef LONG (WINAPI *FN_RtlGetVersion)(OSVERSIONINFOW *);
    FN_RtlGetVersion pRtlGetVersion = (FN_RtlGetVersion)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW Info = { sizeof(Info) };

    return pRtlGetVersion && pRtlGetVersion(&Info) == 0 && Info.dwMajorVersion >= 10;
}

static void
test_shcore_hosts(void)
{
    size_t n;
    HMODULE Shcore = LoadLibraryA("shcore.dll");

    if (!Shcore)
    {
        skip("shcore.dll is not present\n");
        return;
    }

    for (n = 0; n < RTL_NUMBER_OF(g_ShcoreHosted); ++n)
    {
        HMODULE Api = LoadLibraryA(g_ShcoreHosted[n]);

        if (!Api)
        {
            if (is_reactos())
                ok(0, "%s did not load: %lu\n", g_ShcoreHosted[n], GetLastError());
            else
                skip("%s is not present\n", g_ShcoreHosted[n]);
            continue;
        }

        if (is_reactos() || is_windows_10_or_later())
            ok(Api == Shcore, "%s is hosted in %p, not in shcore.dll (%p)\n", g_ShcoreHosted[n], (void *)Api, (void *)Shcore);
        else
            skip("%s: host not measured on this Windows\n", g_ShcoreHosted[n]);
        FreeLibrary(Api);
    }
    FreeLibrary(Shcore);
}

START_TEST(contracts)
{
    size_t n;

    for (n = 0; n < RTL_NUMBER_OF(g_Data); ++n)
        test_contract(&g_Data[n]);

    test_shcore_hosts();

    trace("Checked %u functions\n", g_Checked);
}
