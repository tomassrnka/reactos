
include_directories(${REACTOS_SOURCE_DIR}/base/applications/mscutils/diskmgmt)

list(APPEND SOURCE
    DiskModel.c
    ${REACTOS_SOURCE_DIR}/base/applications/mscutils/diskmgmt/diskmodel.c
    ${REACTOS_SOURCE_DIR}/base/applications/mscutils/diskmgmt/diskcollect.c)

list(APPEND PCH_SKIP_SOURCE
    testlist.c)

add_executable(diskmgmt_apitest
    ${SOURCE}
    ${PCH_SKIP_SOURCE})

target_link_libraries(diskmgmt_apitest wine ${PSEH_LIB})
set_module_type(diskmgmt_apitest win32cui)
add_importlibs(diskmgmt_apitest advapi32 msvcrt kernel32)
add_rostests_file(TARGET diskmgmt_apitest)
