
add_executable(ngedge ngedge.c)
set_module_type(ngedge win32cui)
add_importlibs(ngedge msvcrt kernel32 advapi32 ntdll)
