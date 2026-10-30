@ stub HttpAddFragmentToCache
@ stdcall -stub HttpAddUrl(ptr wstr ptr)
@ stdcall -stub HttpAddUrlToUrlGroup(int64 wstr int64 long)
@ stub HttpCreateAppPool
@ stub HttpCreateConfigGroup
@ stub HttpCreateFilter
@ stdcall -stub HttpCreateHttpHandle(ptr long)
@ stdcall HttpCreateRequestQueue(long wstr ptr long ptr)
@ stdcall HttpCreateServerSession(long ptr long)
@ stdcall HttpCreateUrlGroup(int64 ptr long)
@ stub HttpDeleteConfigGroup
@ stdcall -stub HttpDeleteServiceConfiguration(ptr long ptr long ptr)
@ stub HttpFilterAccept
@ stub HttpFilterAppRead
@ stub HttpFilterAppWrite
@ stub HttpFilterAppWriteAndRawRead
@ stub HttpFilterClose
@ stub HttpFilterRawRead
@ stub HttpFilterRawWrite
@ stub HttpFilterRawWriteAndAppRead
@ stub HttpFlushResponseCache
@ stub HttpGetCounters
@ stdcall -stub HttpInitialize(long long ptr)
@ stub HttpOpenAppPool
@ stub HttpOpenControlChannel
@ stub HttpOpenFilter
@ stub HttpQueryAppPoolInformation
@ stub HttpQueryConfigGroupInformation
@ stub HttpQueryControlChannelInformation
@ stdcall -stub HttpQueryServiceConfiguration(ptr long ptr long ptr long ptr ptr)
@ stub HttpReadFragmentFromCache
@ stub HttpReceiveClientCertificate
@ stdcall -stub HttpReceiveHttpRequest(ptr int64 long ptr long ptr ptr)
@ stdcall -stub HttpReceiveRequestEntityBody(ptr int64 long ptr long ptr ptr)
@ stub HttpRemoveAllUrlsFromConfigGroup
@ stdcall -stub HttpRemoveUrl(ptr wstr)
@ stdcall -stub HttpRemoveUrlFromUrlGroup(int64 wstr long)
@ stdcall -stub HttpSendHttpResponse(ptr int64 long ptr ptr ptr ptr long ptr ptr)
@ stub HttpSetAppPoolInformation
@ stub HttpSetConfigGroupInformation
@ stub HttpSetControlChannelInformation
@ stdcall -stub HttpSetServiceConfiguration(ptr long ptr long ptr)
@ stub HttpShutdownAppPool
@ stub HttpShutdownFilter
@ stdcall -stub HttpTerminate(long ptr)
@ stub HttpWaitForDemandStart
@ stub HttpWaitForDisconnect
@ stub HttpCancelHttpRequest
@ stdcall HttpCloseRequestQueue(ptr)
@ stdcall HttpCloseServerSession(int64)
@ stdcall HttpCloseUrlGroup(int64)
@ stub HttpControlService
@ stub HttpQueryRequestQueueProperty
@ stub HttpQueryServerSessionProperty
@ stub HttpQueryUrlGroupProperty
@ stdcall -stub HttpSendResponseEntityBody(ptr int64 long long ptr ptr ptr long ptr ptr)
@ stdcall HttpSetRequestQueueProperty(ptr long ptr long long ptr)
@ stdcall HttpSetServerSessionProperty(int64 long ptr long)
@ stdcall HttpSetUrlGroupProperty(int64 long ptr long)
@ stdcall HttpShutdownRequestQueue(ptr)
