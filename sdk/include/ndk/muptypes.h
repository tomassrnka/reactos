#ifndef _MUPTYPES_H
#define _MUPTYPES_H

//
// Dependencies
//
#include <umtypes.h>

//
// FSCTL sent to MUP for UNC provider registration
//
#define FSCTL_MUP_REGISTER_PROVIDER CTL_CODE(FILE_DEVICE_MULTI_UNC_PROVIDER, 1, METHOD_BUFFERED, FILE_ANY_ACCESS)

//
// FSCTL sent by FsRtlRegisterUncProviderEx to register a provider by device object
// (kernel mode only: the buffer carries a pointer)
//
#define FSCTL_MUP_REGISTER_PROVIDER_EX CTL_CODE(FILE_DEVICE_MULTI_UNC_PROVIDER, 2, METHOD_BUFFERED, FILE_ANY_ACCESS)

//
// Input buffer for FSCTL_MUP_REGISTER_PROVIDER
//
typedef struct _MUP_PROVIDER_REGISTRATION_INFO
{
    ULONG RedirectorDeviceNameOffset;
    ULONG RedirectorDeviceNameLength;
    ULONG Reserved[2];
    BOOLEAN MailslotsSupported;
} MUP_PROVIDER_REGISTRATION_INFO, *PMUP_PROVIDER_REGISTRATION_INFO;

//
// Flags of FsRtlRegisterUncProviderEx, hidden by older NTDDI_VERSIONs
//
#ifndef FSRTL_UNC_PROVIDER_FLAGS_MAILSLOTS_SUPPORTED
#define FSRTL_UNC_PROVIDER_FLAGS_MAILSLOTS_SUPPORTED 0x00000001
#endif

//
// Input buffer for FSCTL_MUP_REGISTER_PROVIDER_EX
//
typedef struct _MUP_PROVIDER_REGISTRATION_INFO_EX
{
    ULONG RedirectorDeviceNameOffset;
    ULONG RedirectorDeviceNameLength;
    PDEVICE_OBJECT DeviceObject;
    ULONG Flags; /* FSRTL_UNC_PROVIDER_FLAGS_* */
} MUP_PROVIDER_REGISTRATION_INFO_EX, *PMUP_PROVIDER_REGISTRATION_INFO_EX;

#endif // _MUPTYPES_H
