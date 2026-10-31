
add_executable(wevtutil_apitest wevtutil.c testlist.c)
target_link_libraries(wevtutil_apitest wine)
set_module_type(wevtutil_apitest win32cui)
add_importlibs(wevtutil_apitest wevtapi advapi32 msvcrt kernel32)
add_rostests_file(TARGET wevtutil_apitest)
