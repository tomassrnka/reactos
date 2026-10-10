
#define STANDALONE
#include <apitest.h>

extern void func_abortiveclose(void);
extern void func_accept(void);
extern void func_backlog(void);
extern void func_backlog_listen(void);
extern void func_backlog_open(void);
extern void func_backlog_replace(void);
extern void func_bind(void);
extern void func_broadcast(void);
extern void func_close(void);
extern void func_getaddrinfo(void);
extern void func_gethostname(void);
extern void func_getnameinfo(void);
extern void func_getpeername(void);
extern void func_getservbyname(void);
extern void func_getservbyport(void);
extern void func_ioctlsocket(void);
extern void func_listen(void);
extern void func_nonblocking(void);
extern void func_nostartup(void);
extern void func_open_osfhandle(void);
extern void func_recv(void);
extern void func_send(void);
extern void func_udpbindretry(void);
extern void func_udprecv(void);
extern void func_timewait(void);
extern void func_WSAAsync(void);
extern void func_WSAIoctl(void);
extern void func_WSARecv(void);
extern void func_WSARecvFrom(void);
extern void func_WSAStartup(void);

const struct test winetest_testlist[] =
{
    { "abortiveclose", func_abortiveclose },
    { "accept", func_accept },
    { "backlog", func_backlog },
    { "backlog_listen", func_backlog_listen },
    { "backlog_open", func_backlog_open },
    { "backlog_replace", func_backlog_replace },
    { "bind", func_bind },
    { "broadcast", func_broadcast },
    { "close", func_close },
    { "getaddrinfo", func_getaddrinfo },
    { "gethostname", func_gethostname },
    { "getnameinfo", func_getnameinfo },
    { "getpeername", func_getpeername },
    { "getservbyname", func_getservbyname },
    { "getservbyport", func_getservbyport },
    { "ioctlsocket", func_ioctlsocket },
    { "listen", func_listen },
    { "nonblocking", func_nonblocking },
    { "nostartup", func_nostartup },
    { "open_osfhandle", func_open_osfhandle },
    { "recv", func_recv },
    { "send", func_send },
    { "udpbindretry", func_udpbindretry },
    { "udprecv", func_udprecv },
    { "timewait", func_timewait },
    { "WSAAsync", func_WSAAsync },
    { "WSAIoctl", func_WSAIoctl },
    { "WSARecv", func_WSARecv },
    { "WSARecvFrom", func_WSARecvFrom },
    { "WSAStartup", func_WSAStartup },
    { 0, 0 }
};

