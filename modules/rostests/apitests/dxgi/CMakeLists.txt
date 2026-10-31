
list(APPEND SOURCE
    EnumWarpAdapter.c
    testlist.c)

add_executable(dxgi_apitest ${SOURCE})
target_link_libraries(dxgi_apitest wine uuid dxguid dx10guid)
set_module_type(dxgi_apitest win32cui)
add_importlibs(dxgi_apitest ole32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET dxgi_apitest)
