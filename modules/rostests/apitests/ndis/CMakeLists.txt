
list(APPEND SOURCE
    FakeMiniport.c
    testlist.c)

include_directories(${REACTOS_SOURCE_DIR}/modules/rostests/drivers/fakemp)
add_executable(ndis_apitest ${SOURCE})
target_link_libraries(ndis_apitest wine)
set_module_type(ndis_apitest win32cui)
add_importlibs(ndis_apitest setupapi shlwapi advapi32 msvcrt kernel32 ntdll)
add_dependencies(ndis_apitest fakemps fakempd)
add_rostests_file(TARGET ndis_apitest)
