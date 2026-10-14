@ stdcall -private DllCanUnloadNow()
@ stdcall -private DllGetClassObject(ptr ptr ptr)
@ stdcall -private DllRegisterServer()
@ stdcall -private DllUnregisterServer()
@ stub NdfRunDllDiagnoseIncident
@ stub NdfRunDllDiagnoseNetConnectionIncident
@ stub NdfRunDllDiagnoseWithAnswerFile
@ stub NdfRunDllDuplicateIPDefendingSystem
@ stub NdfRunDllDuplicateIPOffendingSystem
@ stub NdfRunDllHelpTopic
@ stdcall NdfCancelIncident(ptr)
@ stdcall NdfCloseIncident(ptr)
@ stdcall NdfCreateConnectivityIncident(ptr)
@ stdcall NdfCreateDNSIncident(wstr long ptr)
@ stdcall NdfCreateGroupingIncident(wstr wstr wstr ptr wstr ptr)
@ stub NdfCreateInboundIncident
@ stdcall NdfCreateIncident(wstr long ptr ptr)
@ stdcall NdfCreateNetConnectionIncident(ptr int128)
@ stdcall NdfCreatePnrpIncident(wstr wstr long wstr ptr)
@ stdcall NdfCreateSharingIncident(wstr ptr)
@ stdcall NdfCreateWebIncident(wstr ptr)
@ stdcall NdfCreateWebIncidentEx(wstr long wstr ptr)
@ stdcall NdfCreateWinSockIncident(ptr wstr long wstr ptr ptr)
@ stdcall NdfDiagnoseIncident(ptr ptr ptr long long)
@ stdcall NdfExecuteDiagnosis(ptr ptr)
@ stdcall NdfGetTraceFile(ptr ptr)
@ stdcall NdfRepairIncident(ptr ptr long)
@ stdcall NdfRepairIncidentEx(ptr ptr long long)
