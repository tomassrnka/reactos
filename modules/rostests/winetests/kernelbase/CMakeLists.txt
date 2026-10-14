
include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine)
add_definitions(-D__WINESRC__ -DWINETEST_USE_DBGSTR_LONGLONG)

add_executable(kernelbase_winetest sync.c testlist.c)
set_module_type(kernelbase_winetest win32cui)
add_importlibs(kernelbase_winetest msvcrt kernel32 ntdll)
add_rostests_file(TARGET kernelbase_winetest)
