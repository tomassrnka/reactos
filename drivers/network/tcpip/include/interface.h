#pragma once

#define IFENT_SOFTWARE_LOOPBACK 24 /* This is an SNMP constant from rfc1213 */

NTSTATUS GetInterfaceIPv4Address( PIP_INTERFACE Interface,
				  ULONG Type,
				  PULONG Address );
UINT CountInterfaces(VOID);
UINT CountInterfaceAddresses( PIP_INTERFACE Interface );
NTSTATUS GetInterfaceSpeed( PIP_INTERFACE Interface, PUINT Speed );
NTSTATUS GetInterfaceName( PIP_INTERFACE Interface, PCHAR NameBuffer,
			   UINT NameMaxLen );
VOID GetInterfaceConnectionStatus( PIP_INTERFACE Interface, PULONG OperStatus );
PIP_INTERFACE FindOnLinkInterface(PIP_ADDRESS Address);
PIP_INTERFACE GetDefaultInterface(VOID);
PIP_INTERFACE IPReferenceDefaultInterface(VOID);

/* tcpip/addrchange.c: wake the IOCTL_IP_ADDCHANGE_NOTIFY_REQUEST waiters */
VOID AddrChangeInitialize(VOID);
VOID AddrChangeNotify(VOID);
VOID RouteChangeNotify(VOID);
NTSTATUS DispRouteChangeNotifyRequest(PIRP Irp);
NTSTATUS DispAddrChangeNotifyRequest(PIRP Irp);
NTSTATUS DispAddrChangeCancelRequest(PIRP Irp);
NTSTATUS NTAPI TiDispatchCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp);
