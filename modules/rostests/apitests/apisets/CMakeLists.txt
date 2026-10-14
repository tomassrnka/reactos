
add_subdirectory(localdll)
include_directories($<TARGET_FILE_DIR:apisets_localdll>)
add_rc_deps(testdata.rc ${CMAKE_CURRENT_BINARY_DIR}/localdll/apisets_localdll.dll)

add_executable(apisets_apitest apisets.c contracts.c loader.c testlist.c testdata.rc winrt.c)
set_module_type(apisets_apitest win32cui)
target_link_libraries(apisets_apitest apisets)
add_importlibs(apisets_apitest msvcrt advapi32 kernel32 ntdll)
add_dependencies(apisets_apitest apisets_localdll)
add_rostests_file(TARGET apisets_apitest)
