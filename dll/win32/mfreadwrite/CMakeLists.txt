
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(
    -D__WINESRC__
    -D_WIN32_WINNT=0x603
    -DWINVER=0x603)

spec2def(mfreadwrite.dll mfreadwrite.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    reader.c
    writer.c)

add_library(mfreadwrite MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/mfreadwrite.def
    ${CMAKE_CURRENT_BINARY_DIR}/mfreadwrite_stubs.c)

add_idl_reg_scripts(mfreadwrite registry mf_classes.idl)

set_module_type(mfreadwrite win32dll)
target_link_libraries(mfreadwrite wine wine_dll_register wine_dll_canunload mfuuid uuid pathcch_static oldnames)
add_delay_importlibs(mfreadwrite mf)
add_importlibs(mfreadwrite mfplat ole32 msvcrt kernel32_vista kernel32 ntdll)
add_dependencies(mfreadwrite psdk wineheaders d3d_idl_headers)
add_cd_file(TARGET mfreadwrite DESTINATION reactos/system32 FOR all)
set_wine_module(mfreadwrite)
