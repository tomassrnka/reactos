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
@ stub NdfCreateGroupingIncident
@ stub NdfCreateInboundIncident
@ stub NdfCreateIncident
@ stub NdfCreateNetConnectionIncident
@ stub NdfCreatePnrpIncident
@ stdcall NdfCreateSharingIncident(wstr ptr)
@ stdcall NdfCreateWebIncident(wstr ptr)
@ stdcall NdfCreateWebIncidentEx(wstr long wstr ptr)
@ stdcall NdfCreateWinSockIncident(ptr wstr long wstr ptr ptr)
@ stdcall NdfDiagnoseIncident(ptr ptr ptr long long)
@ stdcall NdfExecuteDiagnosis(ptr ptr)
@ stdcall NdfGetTraceFile(ptr ptr)
@ stub NdfRepairIncident
@ stub NdfRepairIncidentEx
