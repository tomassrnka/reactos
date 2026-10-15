
add_definitions(
    -DUSE_WINE_TODOS
    -DUSE_WIN_SKIP
    -DWINETEST_USE_DBGSTR_LONGLONG
    -D__WINESRC__)

list(APPEND SOURCE
    nstc.c)

add_executable(explorerframe_winetest ${SOURCE})
set_module_type(explorerframe_winetest win32cui)
target_link_libraries(explorerframe_winetest winetestlib uuid oldnames)
add_importlibs(explorerframe_winetest shell32 ole32 user32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET explorerframe_winetest)
