
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

spec2def(taskschd.dll taskschd.spec)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/wine)
add_rpc_files(client schrpc.idl)

list(APPEND SOURCE
    folder.c
    folder_collection.c
    regtask.c
    task.c
    taskschd.c
    ${CMAKE_CURRENT_BINARY_DIR}/schrpc_c.c
    ${CMAKE_CURRENT_BINARY_DIR}/taskschd_stubs.c)

add_library(taskschd MODULE
    ${SOURCE}
    ${CMAKE_CURRENT_BINARY_DIR}/taskschd.def)

set_module_type(taskschd win32dll)
add_idl_reg_scripts(taskschd regtypelib taskschd_tlb.idl)
add_dependencies(taskschd stdole2 wineheaders) # taskschd_tlb.tlb needs stdole2.tlb, schrpc.h
target_link_libraries(taskschd uuid wine wine_dll_register wine_dll_canunload oldnames)
add_importlibs(taskschd oleaut32 ole32 advapi32 xmllite rpcrt4 msvcrt kernel32_vista kernel32 ntdll)
add_cd_file(TARGET taskschd DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(taskschd) # CORE-5743: No CONST_VTABLE
