
add_subdirectory(mc)

add_definitions(-D_MIDL_USE_GUIDDEF_)

list(APPEND SOURCE
    msctf_undoc.idl)

add_idl_headers(reactos_idls ${SOURCE})
add_dependencies(reactos_idls psdk)

# Metadata for the future user-mode WoW64 thunks. Both architectures use the
# same NT kernel table; win32k has independently ordered architecture tables.
set(wow64_tables_dir "${CMAKE_CURRENT_BINARY_DIR}/wow64")
set(wow64_tables)
foreach(table nt32to64 nt64to32 win32k32to64 win32k64to32)
    list(APPEND wow64_tables "${wow64_tables_dir}/${table}.inc" "${wow64_tables_dir}/${table}.csv")
endforeach()
add_custom_command(OUTPUT ${wow64_tables}
    COMMAND "${CMAKE_COMMAND}"
        "-DNT_SERVICES=${REACTOS_SOURCE_DIR}/ntoskrnl/include/sysfuncs.h"
        "-DWIN32_SERVICES_32=${REACTOS_SOURCE_DIR}/win32ss/w32ksvc32.h"
        "-DWIN32_SERVICES_64=${REACTOS_SOURCE_DIR}/win32ss/w32ksvc64.h"
        "-DOUTPUT_DIR=${wow64_tables_dir}"
        "-DDBG=${DBG}"
        -P "${REACTOS_SOURCE_DIR}/sdk/cmake/wow64syscalls.cmake"
    DEPENDS "${REACTOS_SOURCE_DIR}/sdk/cmake/wow64syscalls.cmake"
        "${REACTOS_SOURCE_DIR}/ntoskrnl/include/sysfuncs.h"
        "${REACTOS_SOURCE_DIR}/win32ss/w32ksvc32.h"
        "${REACTOS_SOURCE_DIR}/win32ss/w32ksvc64.h"
    VERBATIM)
add_custom_target(wow64_syscall_tables ALL DEPENDS ${wow64_tables})
add_custom_target(wow64_syscall_tables_test
    COMMAND "${CMAKE_COMMAND}" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/wow64-tests"
        -P "${REACTOS_SOURCE_DIR}/sdk/cmake/tests/wow64syscalls.cmake"
    VERBATIM)
