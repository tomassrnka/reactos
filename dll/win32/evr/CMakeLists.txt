
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(
    -D__WINESRC__
    -D_WIN32_WINNT=0x603
    -DWINVER=0x603)

spec2def(evr.dll evr.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    evr.c
    main.c
    mixer.c
    presenter.c
    sample.c)

add_library(evr MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/evr.def
    ${CMAKE_CURRENT_BINARY_DIR}/evr_stubs.c)

add_idl_headers(evr_idlheader evr_classes.idl)
add_idl_reg_scripts(evr registry evr_classes.idl)

set_module_type(evr win32dll)
target_link_libraries(evr wine wine_dll_register wine_dll_canunload strmbase strmiids mfuuid dxguid uuid oldnames)
add_delay_importlibs(evr mfplat)
add_importlibs(evr ole32 oleaut32 user32 d3d9 dxva2 msvcrt kernel32_vista kernel32 ntdll)
add_dependencies(evr psdk wineheaders d3d_idl_headers evr_idlheader)
add_cd_file(TARGET evr DESTINATION reactos/system32 FOR all)
set_wine_module(evr)
