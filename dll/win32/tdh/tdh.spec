@ stdcall TdhEnumerateProviderFieldInformation(ptr long ptr ptr)
@ stdcall -version=0x601+ TdhEnumerateProviderFilters(ptr long ptr ptr ptr ptr)
@ stdcall TdhEnumerateProviders(ptr ptr)
@ stub TdhEnumerateRemoteWBEMProviderFieldInformation
@ stub TdhEnumerateRemoteWBEMProviders
@ stdcall TdhFormatProperty(ptr ptr long long long long long ptr ptr ptr ptr)
@ stub TdhGetAllEventsInformation
@ stdcall TdhGetEventInformation(ptr long ptr ptr ptr)
@ stdcall TdhGetEventMapInformation(ptr wstr ptr ptr)
@ stdcall TdhGetProperty(ptr long ptr long ptr long ptr)
@ stub TdhGetPropertyOffsetAndSize
@ stdcall TdhGetPropertySize(ptr long ptr long ptr ptr)
@ stdcall TdhLoadManifest(wstr)
@ stdcall TdhQueryProviderFieldInformation(ptr int64 long ptr ptr)
@ stub TdhQueryRemoteWBEMProviderFieldInformation
@ stdcall TdhUnloadManifest(wstr)
