
add_executable(ngfeat ngfeat.c)
set_module_type(ngfeat win32cui)
add_importlibs(ngfeat msvcrt kernel32)
