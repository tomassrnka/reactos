
add_definitions(
    -DUSE_WINE_TODOS
    -DUSE_WIN_SKIP
    -DWINETEST_USE_DBGSTR_LONGLONG
    -D__WINESRC__)

list(APPEND SOURCE
    mfplat.c
    resource.rc)

add_executable(mfreadwrite_winetest ${SOURCE})
set_module_type(mfreadwrite_winetest win32cui)
target_link_libraries(mfreadwrite_winetest winetestlib mfuuid uuid)
add_importlibs(mfreadwrite_winetest ole32 user32 d3d11 d3d9 dxva2 mfplat mf mfreadwrite propsys msvcrt kernel32 ntdll)
add_dependencies(mfreadwrite_winetest psdk wineheaders d3d_idl_headers)
add_rostests_file(TARGET mfreadwrite_winetest)
