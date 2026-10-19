
add_definitions(
    -DUSE_WINE_TODOS
    -DUSE_WIN_SKIP
    -DWINETEST_USE_DBGSTR_LONGLONG
    -D__WINESRC__)

list(APPEND SOURCE
    mfplat.c
    resource.rc)

add_executable(mfplat_winetest ${SOURCE})
set_module_type(mfplat_winetest win32cui)
target_link_libraries(mfplat_winetest winetestlib mfuuid strmiids uuid pathcch_static)
add_importlibs(mfplat_winetest ole32 mfplat user32 d3d9 dxva2 propsys msvcrt kernel32 ntdll)
add_dependencies(mfplat_winetest psdk wineheaders d3d_idl_headers)
add_rostests_file(TARGET mfplat_winetest)
