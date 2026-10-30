
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(
    -D__WINESRC__
    -D_WIN32_WINNT=0x603
    -DWINVER=0x603)

spec2def(mfplay.dll mfplay.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    player.c)

add_library(mfplay MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/mfplay.def
    ${CMAKE_CURRENT_BINARY_DIR}/mfplay_stubs.c)

set_module_type(mfplay win32dll)
target_link_libraries(mfplay wine wine_dll_register wine_dll_canunload mfuuid strmiids uuid oldnames)
add_importlibs(mfplay mfplat mf ole32 user32 msvcrt kernel32_vista kernel32 ntdll)
add_dependencies(mfplay psdk wineheaders d3d_idl_headers)
add_cd_file(TARGET mfplay DESTINATION reactos/system32 FOR all)
set_wine_module(mfplay)
