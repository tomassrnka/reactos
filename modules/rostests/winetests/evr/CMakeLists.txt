
add_definitions(
    -DUSE_WINE_TODOS
    -DUSE_WIN_SKIP
    -DWINETEST_USE_DBGSTR_LONGLONG
    -D__WINESRC__)

list(APPEND SOURCE
    evr.c
    resource.rc)

add_executable(evr_winetest ${SOURCE})
set_module_type(evr_winetest win32cui)
target_link_libraries(evr_winetest winetestlib mfuuid strmiids uuid dxguid)
add_importlibs(evr_winetest dxva2 mfplat mf ole32 oleaut32 evr d3d9 user32 msvcrt kernel32 ntdll)
add_dependencies(evr_winetest psdk wineheaders d3d_idl_headers)
add_rostests_file(TARGET evr_winetest)
