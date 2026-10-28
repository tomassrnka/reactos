
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl
    ${REACTOS_SOURCE_DIR}/sdk/include/wine
    ${REACTOS_BINARY_DIR}/sdk/include/wine)
add_rpc_files(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/atsvc.idl schrpc.idl)
spec2def(schedsvc.dll schedsvc.spec ADD_IMPORTLIB)

add_library(schedsvc MODULE
    job.c
    rpcserver.c
    schedsvc.c
    schrpc.c
    taskrun.c
    schedsvc.rc
    ${CMAKE_CURRENT_BINARY_DIR}/atsvc_s.c
    ${CMAKE_CURRENT_BINARY_DIR}/schrpc_s.c
    ${CMAKE_CURRENT_BINARY_DIR}/schedsvc.def)

set_module_type(schedsvc win32dll UNICODE)
target_link_libraries(schedsvc uuid wine pseh)
add_dependencies(schedsvc wineheaders) # schrpc.h
add_importlibs(schedsvc advapi32 rpcrt4 ole32 oleaut32 xmllite userenv msvcrt kernel32_vista kernel32 ntdll)
add_cd_file(TARGET schedsvc DESTINATION reactos/system32 FOR all)
