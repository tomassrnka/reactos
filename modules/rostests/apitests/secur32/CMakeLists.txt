
list(APPEND SOURCE
    LsaLogonContext.c
    SchannelProtocols.c
    SchannelServerCert.c
    testlist.c)

add_executable(secur32_apitest ${SOURCE})
target_link_libraries(secur32_apitest wine)
set_module_type(secur32_apitest win32cui)
add_importlibs(secur32_apitest secur32 crypt32 ws2_32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET secur32_apitest)
