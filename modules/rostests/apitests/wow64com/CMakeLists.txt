list(APPEND SOURCE
    Registrations.c
    testlist.c)

add_executable(wow64com_apitest ${SOURCE})
set_module_type(wow64com_apitest win32cui)
target_link_libraries(wow64com_apitest uuid)
if(DLL_EXPORT_VERSION GREATER_EQUAL 0x600)
    target_compile_definitions(wow64com_apitest PRIVATE TEST_D3D11)
endif()
add_importlibs(wow64com_apitest shlwapi user32 ole32 msvcrt kernel32)
add_dependencies(wow64com_apitest psdk)
add_rostests_file(TARGET wow64com_apitest)
