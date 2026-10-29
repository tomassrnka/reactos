/*
 * PROJECT:     ReactOS TDI extensions
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private TDI queries between afd.sys and tcpip.sys
 */

#ifndef _REACTOS_TDIEXT_H
#define _REACTOS_TDIEXT_H

/* TDI_QUERY_INFORMATION type: the buffer holds a TA_IP_ADDRESS, in the destination,
 * out the address of the interface a datagram sent to it leaves from. */
#define TDI_QUERY_ROS_SOURCE_ADDRESS 0x00008001

#endif /* _REACTOS_TDIEXT_H */
