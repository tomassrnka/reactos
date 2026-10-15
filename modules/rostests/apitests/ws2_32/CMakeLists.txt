
list(APPEND SOURCE
    abortiveclose.c
    accept.c
    backlog.c
    bind.c
    broadcast.c
    close.c
    closerace.c
    connectex.c
    getaddrinfo.c
    GetHostNameW.c
    gethostname.c
    getnameinfo.c
    getpeername.c
    getservbyname.c
    getservbyport.c
    helpers.c
    iocpskip.c
    ioctlsocket.c
    listen.c
    nonblocking.c
    nostartup.c
    open_osfhandle.c
    recv.c
    WSASendMsg.c
    send.c
    sendcancel.c
    sendfail.c
    udpbindretry.c
    udprecv.c
    timewait.c
    WSAAsync.c
    WSAIoctl.c
    WSALookupService.c
    WSARecv.c
    WSARecvFrom.c
    WSAStartup.c)

list(APPEND PCH_SKIP_SOURCE
    testlist.c)

add_executable(ws2_32_apitest
    ${SOURCE}
    ${PCH_SKIP_SOURCE})

target_link_libraries(ws2_32_apitest wine ${PSEH_LIB})
set_module_type(ws2_32_apitest win32cui)
add_importlibs(ws2_32_apitest advapi32 iphlpapi user32 ws2_32 msvcrt kernel32 ntdll)
add_pch(ws2_32_apitest ws2_32.h "${PCH_SKIP_SOURCE}")
add_rostests_file(TARGET ws2_32_apitest)
