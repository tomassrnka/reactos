
add_executable(d3d12_apitest d3d12.c testlist.c)
target_link_libraries(d3d12_apitest wine)
set_module_type(d3d12_apitest win32cui)
add_importlibs(d3d12_apitest msvcrt kernel32)
add_rostests_file(TARGET d3d12_apitest)
