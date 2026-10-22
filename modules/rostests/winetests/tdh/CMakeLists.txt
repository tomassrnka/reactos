
add_executable(tdh_winetest tdh.c)
target_compile_definitions(tdh_winetest PRIVATE STANDALONE __WINESRC__)
set_module_type(tdh_winetest win32cui)
add_importlibs(tdh_winetest tdh msvcrt kernel32)
add_rostests_file(TARGET tdh_winetest)
