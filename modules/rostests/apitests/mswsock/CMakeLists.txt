
add_executable(mswsock_apitest AcceptEx.c testlist.c)
set_module_type(mswsock_apitest win32cui)
add_importlibs(mswsock_apitest mswsock ws2_32 msvcrt kernel32)
add_rostests_file(TARGET mswsock_apitest)
