@ stdcall EvtArchiveExportedLog(ptr wstr long long)
@ stdcall EvtCancel(ptr)
@ stdcall EvtClearLog(ptr wstr wstr long)
@ stdcall EvtClose(ptr)
@ stdcall EvtCreateBookmark(wstr)
@ stdcall EvtCreateRenderContext(long ptr long)
@ stdcall EvtExportLog(ptr wstr wstr wstr long)
@ stdcall EvtFormatMessage(ptr ptr long long ptr long long ptr ptr)
@ stdcall EvtGetChannelConfigProperty(ptr long long long ptr ptr)
@ stdcall EvtGetEventInfo(ptr long long ptr ptr)
@ stdcall EvtGetEventMetadataProperty(ptr long long long ptr ptr)
@ stdcall EvtGetExtendedStatus(long ptr ptr)
@ stdcall EvtGetLogInfo(ptr long long ptr ptr)
@ stdcall EvtGetObjectArrayProperty(ptr long long long long ptr ptr)
@ stdcall EvtGetObjectArraySize(ptr ptr)
@ stdcall EvtGetPublisherMetadataProperty(ptr long long long ptr ptr)
@ stdcall EvtGetQueryInfo(ptr long long ptr ptr)
@ stub EvtIntAssertConfig
@ stub EvtIntCreateBinXMLFromCustomXML
@ stub EvtIntCreateLocalLogfile
@ stub EvtIntGetClassicLogDisplayName
@ stub EvtIntRenderResourceEventTemplate
@ stub EvtIntReportAuthzEventAndSourceAsync
@ stub EvtIntReportEventAndSourceAsync
@ stub EvtIntRetractConfig
@ stub EvtIntSysprepCleanup
@ stub EvtIntWriteXmlEventToLocalLogfile
@ stdcall EvtNext(ptr long ptr long long ptr)
@ stdcall EvtNextChannelPath(ptr long ptr ptr)
@ stdcall EvtNextEventMetadata(ptr long)
@ stdcall EvtNextPublisherId(ptr long ptr ptr)
@ stdcall EvtOpenChannelConfig(ptr wstr long)
@ stdcall EvtOpenChannelEnum(ptr long)
@ stdcall EvtOpenEventMetadataEnum(ptr long)
@ stdcall EvtOpenLog(ptr wstr long)
@ stdcall EvtOpenPublisherEnum(ptr long)
@ stdcall EvtOpenPublisherMetadata(ptr wstr wstr long long)
@ stdcall EvtOpenSession(long ptr long long)
@ stdcall EvtQuery(ptr wstr wstr long)
@ stdcall EvtRender(ptr ptr long long ptr ptr ptr)
@ stdcall EvtSaveChannelConfig(ptr long)
@ stdcall EvtSeek(ptr int64 ptr long long)
@ stdcall EvtSetChannelConfigProperty(ptr long long ptr)
@ stdcall EvtSetObjectArrayProperty(ptr long long long ptr)
@ stdcall EvtSubscribe(ptr ptr wstr wstr ptr ptr ptr long)
@ stdcall EvtUpdateBookmark(ptr ptr)
