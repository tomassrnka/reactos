#pragma once

/* The lwIP hooks of lwipopts.h, implemented in ip/lwip_glue/tcp.c */

#include <lwip/ip_addr.h>

struct pbuf;
struct netif;

int LibTCPReopenTimeWait(struct pbuf *p, struct netif *inp);
u32_t LibTCPNextIss(const ip_addr_t *LocalIp, u16_t LocalPort, const ip_addr_t *RemoteIp, u16_t RemotePort);
