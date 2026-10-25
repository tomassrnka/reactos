
add_executable(bchooser browserchooser.c browserchooser.rc)
set_module_type(bchooser win32gui UNICODE)
add_importlibs(bchooser advapi32 user32 shell32 shlwapi comctl32 msvcrt kernel32)
add_cd_file(TARGET bchooser DESTINATION reactos/system32 FOR all)
