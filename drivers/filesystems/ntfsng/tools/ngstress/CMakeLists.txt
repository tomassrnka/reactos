
add_executable(ngstress ngstress.c)
set_module_type(ngstress win32cui)
add_importlibs(ngstress msvcrt kernel32)
