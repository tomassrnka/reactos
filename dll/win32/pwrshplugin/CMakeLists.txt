spec2def(pwrshplugin.dll pwrshplugin.spec)

list(APPEND SOURCE
    pwrshplugin.c
    pwrshplugin.rc
    ${CMAKE_CURRENT_BINARY_DIR}/pwrshplugin.def)

add_library(pwrshplugin MODULE ${SOURCE})
set_module_type(pwrshplugin win32dll)
add_importlibs(pwrshplugin kernel32 ntdll)
add_cd_file(TARGET pwrshplugin DESTINATION reactos/system32 FOR all)
