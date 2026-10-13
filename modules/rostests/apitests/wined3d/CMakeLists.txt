
add_executable(wined3d_apitest AdapterHandles.c testlist.c)
target_link_libraries(wined3d_apitest wine uuid dxguid)
set_module_type(wined3d_apitest win32cui)
add_importlibs(wined3d_apitest user32 gdi32 msvcrt kernel32)
add_rostests_file(TARGET wined3d_apitest)
