
add_executable(ngvol ngvol.c)
set_module_type(ngvol win32cui)
add_importlibs(ngvol msvcrt kernel32)
