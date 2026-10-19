
add_executable(ngchk ngchk.c)
set_module_type(ngchk win32cui UNICODE)
add_importlibs(ngchk msvcrt kernel32 ntdll)
