spec2def(pwrshsip.dll pwrshsip.spec)

list(APPEND SOURCE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/pwrshsip.def)

add_library(pwrshsip MODULE ${SOURCE})
set_module_type(pwrshsip win32dll)
target_link_libraries(pwrshsip wine)
add_importlibs(pwrshsip crypt32 advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET pwrshsip DESTINATION reactos/system32 FOR all)
