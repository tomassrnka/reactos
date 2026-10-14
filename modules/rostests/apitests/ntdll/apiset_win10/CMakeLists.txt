
add_executable(apiset_win10 apiset_win10.c apiset_win10.rc)
set_module_type(apiset_win10 win32cui)
add_importlibs(apiset_win10 msvcrt kernel32 ntdll)
add_dependencies(apiset_win10 psdk)
add_rostests_file(TARGET apiset_win10)
