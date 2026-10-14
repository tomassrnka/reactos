
add_executable(uiprobe uiprobe.c)
set_module_type(uiprobe win32cui UNICODE)
add_importlibs(uiprobe msvcrt kernel32)
add_rostests_file(TARGET uiprobe SUBDIR suppl)
