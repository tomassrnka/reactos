
add_executable(psapi_apitest EnumProcessModulesEx.c psapi.c testlist.c)
set_module_type(psapi_apitest win32cui)
add_importlibs(psapi_apitest psapi msvcrt kernel32)
add_rostests_file(TARGET psapi_apitest)
