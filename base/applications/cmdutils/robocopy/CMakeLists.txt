
add_executable(robocopy main.c robocopy.rc)
set_module_type(robocopy win32cui UNICODE)
target_link_libraries(robocopy wine)
add_importlibs(robocopy msvcrt kernel32 ntdll)
add_cd_file(TARGET robocopy DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(robocopy) # CORE-5743: No ARRAY_SIZE macro
