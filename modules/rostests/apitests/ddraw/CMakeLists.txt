
add_executable(ddraw_apitest NoDriverNotice.c VideoMemory.c testlist.c)
target_link_libraries(ddraw_apitest wine uuid dxguid)
set_module_type(ddraw_apitest win32cui)
add_importlibs(ddraw_apitest user32 gdi32 msvcrt kernel32)
add_rostests_file(TARGET ddraw_apitest)
