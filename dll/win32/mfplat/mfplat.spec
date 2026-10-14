@ stub FormatTagFromWfx
@ stub MFCreateGuid
@ stub MFGetIoPortHandle
@ stub MFGetPlatformVersion
@ stub MFGetRandomNumber
@ stub MFIsFeatureEnabled
@ stub MFIsQueueThread
@ stub MFPlatformBigEndian
@ stub MFPlatformLittleEndian
@ stub ValidateWaveFormat
@ stub CopyPropVariant
@ stub CreatePropVariant
@ stdcall CreatePropertyStore(ptr)
@ stub DestroyPropVariant
@ stub GetAMSubtypeFromD3DFormat
@ stub GetD3DFormatFromMFSubtype
@ stub LFGetGlobalPool
@ stdcall -version=0x603+ MFAddPeriodicCallback(ptr ptr ptr) rtworkq.RtwqAddPeriodicCallback
@ stdcall -version=0x601-0x602 MFAddPeriodicCallback(ptr ptr ptr) RtwqAddPeriodicCallback
@ stdcall -version=0x603+ MFAllocateSerialWorkQueue(long ptr) rtworkq.RtwqAllocateSerialWorkQueue
@ stdcall -version=0x601-0x602 MFAllocateSerialWorkQueue(long ptr) RtwqAllocateSerialWorkQueue
@ stdcall MFAllocateWorkQueue(ptr)
@ stdcall -version=0x603+ MFAllocateWorkQueueEx(long ptr) rtworkq.RtwqAllocateWorkQueue
@ stdcall -version=0x601-0x602 MFAllocateWorkQueueEx(long ptr) RtwqAllocateWorkQueue
@ stub MFAppendCollection
@ stdcall MFAverageTimePerFrameToFrameRate(int64 ptr ptr)
@ stdcall MFBeginCreateFile(long long long wstr ptr ptr ptr)
@ stub MFBeginGetHostByName
@ stdcall MFBeginRegisterWorkQueueWithMMCSS(long wstr long ptr ptr)
@ stdcall -version=0x603+ MFBeginRegisterWorkQueueWithMMCSSEx(long wstr long long ptr ptr) rtworkq.RtwqBeginRegisterWorkQueueWithMMCSS
@ stdcall -version=0x601-0x602 MFBeginRegisterWorkQueueWithMMCSSEx(long wstr long long ptr ptr) RtwqBeginRegisterWorkQueueWithMMCSS
@ stdcall -version=0x603+ MFBeginUnregisterWorkQueueWithMMCSS(long ptr ptr) rtworkq.RtwqBeginUnregisterWorkQueueWithMMCSS
@ stdcall -version=0x601-0x602 MFBeginUnregisterWorkQueueWithMMCSS(long ptr ptr) RtwqBeginUnregisterWorkQueueWithMMCSS
@ stub MFBlockThread
@ stub MFCalculateBitmapImageSize
@ stdcall MFCalculateImageSize(ptr long long ptr)
@ stdcall MFCancelCreateFile(ptr)
@ stdcall -version=0x603+ MFCancelWorkItem(int64) rtworkq.RtwqCancelWorkItem
@ stdcall -version=0x601-0x602 MFCancelWorkItem(int64) RtwqCancelWorkItem
@ stdcall MFCompareFullToPartialMediaType(ptr ptr)
@ stub MFCompareSockaddrAddresses
@ stub MFConvertColorInfoFromDXVA
@ stdcall MFConvertColorInfoToDXVA(ptr ptr)
@ stub MFConvertFromFP16Array
@ stub MFConvertToFP16Array
@ stdcall MFCopyImage(ptr long ptr long long long)
@ stdcall MFCreate2DMediaBuffer(long long long long ptr)
@ stdcall MFCreateAMMediaTypeFromMFMediaType(ptr int128 ptr)
@ stdcall MFCreateAlignedMemoryBuffer(long long ptr)
@ stdcall -version=0x603+ MFCreateAsyncResult(ptr ptr ptr ptr) rtworkq.RtwqCreateAsyncResult
@ stdcall -version=0x601-0x602 MFCreateAsyncResult(ptr ptr ptr ptr) RtwqCreateAsyncResult
@ stdcall MFCreateAttributes(ptr long)
@ stdcall MFCreateAudioMediaType(ptr ptr)
@ stdcall MFCreateCollection(ptr)
@ stdcall MFCreateD3D12SynchronizationObject(ptr ptr ptr)
@ stdcall MFCreateDXGIDeviceManager(ptr ptr)
@ stdcall MFCreateDXGISurfaceBuffer(ptr ptr long long ptr)
@ stdcall MFCreateDXSurfaceBuffer(ptr ptr long ptr)
@ stdcall MFCreateEventQueue(ptr)
@ stdcall MFCreateFile(long long long wstr ptr)
@ stdcall MFCreateLegacyMediaBufferOnMFMediaBuffer(ptr ptr long ptr)
@ stdcall MFCreateMFByteStreamOnStream(ptr ptr)
@ stdcall MFCreateMFByteStreamOnStreamEx(ptr ptr)
@ stdcall MFCreateMFByteStreamWrapper(ptr ptr)
@ stdcall MFCreateMFVideoFormatFromMFMediaType(ptr ptr ptr)
@ stdcall MFCreateMediaBufferFromMediaType(ptr int64 long long ptr)
@ stub MFCreateMediaBufferWrapper
@ stdcall MFCreateMediaEvent(long ptr long ptr ptr)
@ stdcall MFCreateMediaType(ptr)
@ stdcall MFCreateMediaTypeFromRepresentation(int128 ptr ptr)
@ stdcall MFCreateMemoryBuffer(long ptr)
@ stub MFCreateMemoryStream
@ stdcall MFCreatePathFromURL(wstr ptr)
@ stdcall MFCreatePresentationDescriptor(long ptr ptr)
@ stdcall MFCreateSample(ptr)
@ stub MFCreateSocket
@ stub MFCreateSocketListener
@ stdcall MFCreateSourceResolver(ptr)
@ stdcall MFCreateStreamDescriptor(long long ptr ptr)
@ stdcall MFCreateSystemTimeSource(ptr)
@ stub MFCreateSystemUnderlyingClock
@ stdcall MFCreateTempFile(long long long ptr)
@ stdcall MFCreateTrackedSample(ptr)
@ stdcall MFCreateTransformActivate(ptr)
@ stub MFCreateURLFromPath
@ stub MFCreateUdpSockets
@ stdcall MFCreateVideoMediaType(ptr ptr)
@ stub MFCreateVideoMediaTypeFromBitMapInfoHeader
@ stub MFCreateVideoMediaTypeFromBitMapInfoHeaderEx
@ stdcall MFCreateVideoMediaTypeFromSubtype(ptr ptr)
@ stub MFCreateVideoMediaTypeFromVideoInfoHeader2
@ stdcall MFCreateVideoMediaTypeFromVideoInfoHeader(ptr long long long long int64 ptr ptr)
@ stdcall MFCreateVideoSampleAllocatorEx(ptr ptr)
@ stdcall MFCreateWaveFormatExFromMFMediaType(ptr ptr ptr long)
@ stdcall MFDeserializeAttributesFromStream(ptr long ptr)
@ stub MFDeserializeEvent
@ stub MFDeserializeMediaTypeFromStream
@ stub MFDeserializePresentationDescriptor
@ stdcall MFEndCreateFile(ptr ptr)
@ stub MFEndGetHostByName
@ stdcall -version=0x603+ MFEndRegisterWorkQueueWithMMCSS(ptr ptr) rtworkq.RtwqEndRegisterWorkQueueWithMMCSS
@ stdcall -version=0x601-0x602 MFEndRegisterWorkQueueWithMMCSS(ptr ptr) RtwqEndRegisterWorkQueueWithMMCSS
@ stdcall -version=0x603+ MFEndUnregisterWorkQueueWithMMCSS(ptr) rtworkq.RtwqEndUnregisterWorkQueueWithMMCSS
@ stdcall -version=0x601-0x602 MFEndUnregisterWorkQueueWithMMCSS(ptr) RtwqEndUnregisterWorkQueueWithMMCSS
@ stdcall MFFrameRateToAverageTimePerFrame(long long ptr)
@ stub MFFreeAdaptersAddresses
@ stub MFGetAdaptersAddresses
@ stdcall MFGetAttributesAsBlob(ptr ptr long)
@ stdcall MFGetAttributesAsBlobSize(ptr ptr)
@ stub MFGetConfigurationDWORD
@ stub MFGetConfigurationPolicy
@ stub MFGetConfigurationStore
@ stub MFGetConfigurationString
@ stub MFGetMFTMerit
@ stub MFGetNumericNameFromSockaddr
@ stdcall MFGetPlaneSize(long long long ptr)
@ stub MFGetPlatform
@ stdcall MFGetPluginControl(ptr)
@ stub MFGetPrivateWorkqueues
@ stub MFGetSockaddrFromNumericName
@ stdcall MFGetStrideForBitmapInfoHeader(long long ptr)
@ stdcall MFGetSystemTime()
@ stdcall MFGetTimerPeriodicity(ptr)
@ stub MFGetUncompressedVideoFormat
@ stdcall -version=0x603+ MFGetWorkQueueMMCSSClass(long ptr ptr) rtworkq.RtwqGetWorkQueueMMCSSClass
@ stdcall -version=0x601-0x602 MFGetWorkQueueMMCSSClass(long ptr ptr) RtwqGetWorkQueueMMCSSClass
@ stdcall -version=0x603+ MFGetWorkQueueMMCSSTaskId(long ptr) rtworkq.RtwqGetWorkQueueMMCSSTaskId
@ stdcall -version=0x601-0x602 MFGetWorkQueueMMCSSTaskId(long ptr) RtwqGetWorkQueueMMCSSTaskId
@ stdcall -version=0x603+ MFGetWorkQueueMMCSSPriority(long ptr) rtworkq.RtwqGetWorkQueueMMCSSPriority
@ stdcall -version=0x601-0x602 MFGetWorkQueueMMCSSPriority(long ptr) RtwqGetWorkQueueMMCSSPriority
@ stdcall MFHeapAlloc(long long str long long)
@ stdcall MFHeapFree(ptr)
@ stdcall MFInitAMMediaTypeFromMFMediaType(ptr int128 ptr)
@ stdcall MFInitAttributesFromBlob(ptr ptr long)
@ stdcall MFInitMediaTypeFromAMMediaType(ptr ptr)
@ stdcall MFInitMediaTypeFromMFVideoFormat(ptr ptr long)
@ stdcall MFInitMediaTypeFromMPEG1VideoInfo(ptr ptr long ptr)
@ stdcall MFInitMediaTypeFromMPEG2VideoInfo(ptr ptr long ptr)
@ stdcall MFInitMediaTypeFromVideoInfoHeader2(ptr ptr long ptr)
@ stdcall MFInitMediaTypeFromVideoInfoHeader(ptr ptr long ptr)
@ stdcall MFInitMediaTypeFromWaveFormatEx(ptr ptr long)
@ stub MFInitVideoFormat
@ stdcall MFInitVideoFormat_RGB(ptr long long long)
@ stdcall MFInvokeCallback(ptr)
@ stub MFJoinIoPort
@ stdcall -version=0x603+ MFJoinWorkQueue(long long ptr) rtworkq.RtwqJoinWorkQueue
@ stdcall -version=0x601-0x602 MFJoinWorkQueue(long long ptr) RtwqJoinWorkQueue
@ stdcall MFLockDXGIDeviceManager(ptr ptr)
@ stdcall -version=0x603+ MFLockPlatform() rtworkq.RtwqLockPlatform
@ stdcall -version=0x601-0x602 MFLockPlatform() RtwqLockPlatform
@ stdcall -version=0x603+ MFLockSharedWorkQueue(wstr long ptr ptr) rtworkq.RtwqLockSharedWorkQueue
@ stdcall -version=0x601-0x602 MFLockSharedWorkQueue(wstr long ptr ptr) RtwqLockSharedWorkQueue
@ stdcall -version=0x603+ MFLockWorkQueue(long) rtworkq.RtwqLockWorkQueue
@ stdcall -version=0x601-0x602 MFLockWorkQueue(long) RtwqLockWorkQueue
@ stdcall MFMapDX9FormatToDXGIFormat(long)
@ stdcall MFMapDXGIFormatToDX9Format(long)
@ stdcall -version=0x603+ MFPutWaitingWorkItem(long long ptr ptr) rtworkq.RtwqPutWaitingWorkItem
@ stdcall -version=0x601-0x602 MFPutWaitingWorkItem(long long ptr ptr) RtwqPutWaitingWorkItem
@ stdcall MFPutWorkItem(long ptr ptr)
@ stdcall MFPutWorkItem2(long long ptr ptr)
@ stdcall MFPutWorkItemEx(long ptr)
@ stdcall MFPutWorkItemEx2(long long ptr)
@ stub MFRecordError
@ stdcall MFRegisterLocalByteStreamHandler(wstr wstr ptr)
@ stdcall MFRegisterLocalSchemeHandler(wstr ptr)
@ stdcall -version=0x603+ MFRegisterPlatformWithMMCSS(wstr ptr long) rtworkq.RtwqRegisterPlatformWithMMCSS
@ stdcall -version=0x601-0x602 MFRegisterPlatformWithMMCSS(wstr ptr long) RtwqRegisterPlatformWithMMCSS
@ stdcall -version=0x603+ MFRemovePeriodicCallback(long) rtworkq.RtwqRemovePeriodicCallback
@ stdcall -version=0x601-0x602 MFRemovePeriodicCallback(long) RtwqRemovePeriodicCallback
@ stdcall MFScheduleWorkItem(ptr ptr int64 ptr)
@ stdcall -version=0x603+ MFScheduleWorkItemEx(ptr int64 ptr) rtworkq.RtwqScheduleWorkItem
@ stdcall -version=0x601-0x602 MFScheduleWorkItemEx(ptr int64 ptr) RtwqScheduleWorkItem
@ stdcall MFSerializeAttributesToStream(ptr long ptr)
@ stub MFSerializeEvent
@ stub MFSerializeMediaTypeToStream
@ stub MFSerializePresentationDescriptor
@ stub MFSetSockaddrAny
@ stdcall MFShutdown()
@ stdcall MFStartup(long long)
@ stub MFStreamDescriptorProtectMediaType
@ stdcall MFTEnum(int128 long ptr ptr ptr ptr ptr)
@ stdcall MFTEnum2(int128 long ptr ptr ptr ptr ptr)
@ stdcall MFTEnumEx(int128 long ptr ptr ptr ptr)
@ stdcall MFTGetInfo(int128 ptr ptr ptr ptr ptr ptr)
@ stdcall MFTRegister(int128 int128 wstr long long ptr long ptr ptr)
@ stdcall MFTRegisterLocal(ptr ptr wstr long long ptr long ptr)
@ stdcall MFTRegisterLocalByCLSID(ptr ptr wstr long long ptr long ptr)
@ stdcall MFTUnregister(int128)
@ stdcall MFTUnregisterLocal(ptr)
@ stdcall MFTUnregisterLocalByCLSID(int128)
@ stdcall -version=0x603+ MFUnregisterPlatformFromMMCSS() rtworkq.RtwqUnregisterPlatformFromMMCSS
@ stdcall -version=0x601-0x602 MFUnregisterPlatformFromMMCSS() RtwqUnregisterPlatformFromMMCSS
@ stub MFTraceError
@ stub MFTraceFuncEnter
@ stub MFUnblockThread
@ stdcall -version=0x603+ MFUnjoinWorkQueue(long long) rtworkq.RtwqUnjoinWorkQueue
@ stdcall -version=0x601-0x602 MFUnjoinWorkQueue(long long) RtwqUnjoinWorkQueue
@ stdcall MFUnlockDXGIDeviceManager()
@ stdcall -version=0x603+ MFUnlockPlatform() rtworkq.RtwqUnlockPlatform
@ stdcall -version=0x601-0x602 MFUnlockPlatform() RtwqUnlockPlatform
@ stdcall -version=0x603+ MFUnlockWorkQueue(long) rtworkq.RtwqUnlockWorkQueue
@ stdcall -version=0x601-0x602 MFUnlockWorkQueue(long) RtwqUnlockWorkQueue
@ stdcall MFUnwrapMediaType(ptr ptr)
@ stub MFValidateMediaTypeSize
@ stdcall MFWrapMediaType(ptr ptr ptr ptr)
@ stdcall -ret64 MFllMulDiv(int64 int64 int64 int64)
@ stub PropVariantFromStream
@ stub PropVariantToStream
