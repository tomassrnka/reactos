
add_executable(ngfail ngfail.c)
set_module_type(ngfail win32cui)
add_importlibs(ngfail msvcrt kernel32 ntdll)
