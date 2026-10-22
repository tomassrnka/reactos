add_executable(ngbench ngbench.c)
set_module_type(ngbench win32cui)
add_importlibs(ngbench msvcrt kernel32)
