
#define STANDALONE
#include <apitest.h>

extern void func_AddMandatoryAce(void);
extern void func_ConvertStringSidToSid(void);
extern void func_CreateService(void);
extern void func_CloseTrace(void);
extern void func_DuplicateTokenEx(void);
extern void func_EnableTraceEx2(void);
extern void func_EncryptFile(void);
extern void func_eventlog_supp(void);
extern void func_EventRegister(void);
extern void func_EventProvider(void);
extern void func_EventSetInformation(void);
extern void func_EventSetInformationFault(void);
extern void func_Hash(void);
extern void func_HKEY_CLASSES_ROOT(void);
extern void func_IsTextUnicode(void);
extern void func_LockServiceDatabase(void);
extern void func_LogonUser(void);
extern void func_QueryServiceConfig2(void);
extern void func_RegConcurrentWrite(void);
extern void func_RegCreateKeyEx(void);
extern void func_RegCreateKeyTransacted(void);
extern void func_RegEnumKey(void);
extern void func_RegEnumKeyClass(void);
extern void func_RegEnumValueW(void);
extern void func_RegFlushKey(void);
extern void func_RegLoadKeyBadBinSize(void);
extern void func_RegLoadKeyBadSecondBin(void);
extern void func_RegLoadKeyZeroBin(void);
extern void func_RegLoadKeyZeroCell(void);
extern void func_RegOpenKeyExW(void);
extern void func_RegQueryInfoKey(void);
extern void func_RegQueryValueExW(void);
extern void func_RegGetValue(void);
extern void func_RestrictedToken(void);
extern void func_RtlEncryptMemory(void);
extern void func_SaferIdentifyLevel(void);
extern void func_TokenIntegrity(void);
extern void func_ServiceArgs(void);
extern void func_ServiceEnv(void);
extern void func_ServiceFailureActions(void);
extern void func_ServiceNetwork(void);
extern void func_SetEntriesInAclOld(void);
extern void func_SetNamedSecurityInfoInherit(void);
extern void func_SddlRights(void);

extern void func_RegWow64(void);
extern void func_RegWow64Unicode(void);
extern void func_RegWow64Views(void);
extern void func_RegWow64Matrix(void);
extern void func_RegWow64LongName(void);
extern void func_RegWow64SharedLongName(void);
extern void func_RegWow64Volatile(void);
extern void func_RegWow64SharedCreate(void);
extern void func_RegWow64Aliases(void);

const struct test winetest_testlist[] =
{
    { "AddMandatoryAce", func_AddMandatoryAce },
    { "ConvertStringSidToSid", func_ConvertStringSidToSid },
    { "RegWow64", func_RegWow64 },
    { "RegWow64Unicode", func_RegWow64Unicode },
    { "RegWow64Views", func_RegWow64Views },
    { "RegWow64SharedCreate", func_RegWow64SharedCreate },
    { "RegWow64Matrix", func_RegWow64Matrix },
    { "RegWow64LongName", func_RegWow64LongName },
    { "RegWow64SharedLongName", func_RegWow64SharedLongName },
    { "RegWow64Volatile", func_RegWow64Volatile },
    { "RegWow64Aliases", func_RegWow64Aliases },
    { "CreateService", func_CreateService },
    { "CloseTrace", func_CloseTrace },
    { "DuplicateTokenEx", func_DuplicateTokenEx },
    { "EnableTraceEx2", func_EnableTraceEx2 },
    { "EncryptFile", func_EncryptFile },
    { "eventlog_supp", func_eventlog_supp },
    { "EventRegister", func_EventRegister },
    { "EventProvider", func_EventProvider },
    { "EventSetInformation", func_EventSetInformation },
    { "EventSetInformationFault", func_EventSetInformationFault },
    { "Hash", func_Hash },
    { "HKEY_CLASSES_ROOT", func_HKEY_CLASSES_ROOT },
    { "IsTextUnicode" , func_IsTextUnicode },
    { "LockServiceDatabase" , func_LockServiceDatabase },
    { "LogonUser", func_LogonUser },
    { "QueryServiceConfig2", func_QueryServiceConfig2 },
    { "RegConcurrentWrite", func_RegConcurrentWrite },
    { "RegCreateKeyEx", func_RegCreateKeyEx },
    { "RegCreateKeyTransacted", func_RegCreateKeyTransacted },
    { "RegEnumKey", func_RegEnumKey },
    { "RegEnumKeyClass", func_RegEnumKeyClass },
    { "RegEnumValueW", func_RegEnumValueW },
    { "RegFlushKey", func_RegFlushKey },
    { "RegLoadKeyBadBinSize", func_RegLoadKeyBadBinSize },
    { "RegLoadKeyBadSecondBin", func_RegLoadKeyBadSecondBin },
    { "RegLoadKeyZeroBin", func_RegLoadKeyZeroBin },
    { "RegLoadKeyZeroCell", func_RegLoadKeyZeroCell },
    { "RegQueryInfoKey", func_RegQueryInfoKey },
    { "RegOpenKeyExW", func_RegOpenKeyExW },
    { "RegQueryValueExW", func_RegQueryValueExW },
    { "RegGetValue", func_RegGetValue },
    { "RestrictedToken", func_RestrictedToken },
    { "RtlEncryptMemory", func_RtlEncryptMemory },
    { "SaferIdentifyLevel", func_SaferIdentifyLevel },
    { "TokenIntegrity", func_TokenIntegrity },
    { "ServiceArgs", func_ServiceArgs },
    { "ServiceEnv", func_ServiceEnv },
    { "ServiceFailureActions", func_ServiceFailureActions },
    { "ServiceNetwork", func_ServiceNetwork },
    { "SetEntriesInAclOld", func_SetEntriesInAclOld },
    { "SetNamedSecurityInfoInherit", func_SetNamedSecurityInfoInherit },
    { "SddlRights", func_SddlRights },
    { 0, 0 }
};

