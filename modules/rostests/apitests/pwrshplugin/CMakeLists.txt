list(APPEND SOURCE
    pwrshplugin.c
    testlist.c)

add_executable(pwrshplugin_apitest ${SOURCE})
set_module_type(pwrshplugin_apitest win32cui)
add_importlibs(pwrshplugin_apitest version msvcrt kernel32)
add_rostests_file(TARGET pwrshplugin_apitest)
