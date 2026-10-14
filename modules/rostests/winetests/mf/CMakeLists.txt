
add_definitions(
    -DUSE_WINE_TODOS
    -DUSE_WIN_SKIP
    -DWINETEST_USE_DBGSTR_LONGLONG
    -D__WINESRC__)

list(APPEND SOURCE
    mf.c
    resource.rc
    topology.c
    transform.c)

add_executable(mf_winetest ${SOURCE})
set_module_type(mf_winetest win32cui)
target_link_libraries(mf_winetest winetestlib wmcodecdspuuid mfuuid dmoguids strmiids uuid)
add_importlibs(mf_winetest mf mfplat ole32 user32 propsys msdmo d3d9 d3d11 dxva2 msvfw32 oleaut32 winmm msvcrt kernel32 ntdll)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(mf_winetest PRIVATE -Wno-old-style-declaration)
endif()

add_dependencies(mf_winetest psdk wineheaders d3d_idl_headers)
add_rostests_file(TARGET mf_winetest)
