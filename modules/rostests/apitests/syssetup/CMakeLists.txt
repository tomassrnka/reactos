add_executable(syssetup_apitest ProgramFiles.c testlist.c)
target_link_libraries(syssetup_apitest wine ${PSEH_LIB})
set_module_type(syssetup_apitest win32cui)
add_importlibs(syssetup_apitest msvcrt kernel32 ntdll)
# Compile the optional setup path even when testing a default amd64 image.
if(ARCH STREQUAL "amd64")
    target_compile_definitions(syssetup_apitest PRIVATE BUILD_WOW64_ENABLED)
endif()
add_rostests_file(TARGET syssetup_apitest)
