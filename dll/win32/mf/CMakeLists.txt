
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(
    -D__WINESRC__
    -D_WIN32_WINNT=0x603
    -DWINVER=0x603)

spec2def(mf.dll mf.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    clock.c
    copier.c
    evr.c
    main.c
    quality.c
    sac.c
    samplegrabber.c
    sar.c
    scheme_handler.c
    session.c
    topology.c
    topology_loader.c)

add_library(mf MODULE
    ${SOURCE}
    mf.rc
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/mf.def
    ${CMAKE_CURRENT_BINARY_DIR}/mf_stubs.c)

add_idl_reg_scripts(mf registry mf.idl)

set_module_type(mf win32dll)
target_link_libraries(mf wine wine_dll_register wine_dll_canunload mfuuid strmiids uuid pathcch_static oldnames)
add_delay_importlibs(mf evr urlmon user32)
add_importlibs(mf advapi32 mfplat ole32 shlwapi msvcrt kernel32_vista kernel32 ntdll)
add_dependencies(mf psdk wineheaders d3d_idl_headers)
add_cd_file(TARGET mf DESTINATION reactos/system32 FOR all)
set_wine_module(mf)
