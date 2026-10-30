
add_executable(icacls main.c icacls.rc)
set_module_type(icacls win32cui UNICODE)
target_link_libraries(icacls wine)
add_importlibs(icacls advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET icacls DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(icacls) # CORE-5743: No ARRAY_SIZE macro
