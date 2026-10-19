
add_subdirectory(mc)

add_definitions(-D_MIDL_USE_GUIDDEF_)

list(APPEND SOURCE
    msctf_undoc.idl)

add_idl_headers(reactos_idls ${SOURCE})
add_dependencies(reactos_idls psdk)

# Metadata for the future user-mode WoW64 thunks. Both architectures use the
# same NT kernel table; win32k has independently ordered architecture tables.
# wow64protos reads the prototypes of every table row from the headers, so the
# generator can describe each argument and check the table slot counts.
option(WOW64_SYSCALL_CHECK "Fail the build when a syscall table slot count disagrees with its prototype" ON)
set(wow64_tables_dir "${CMAKE_CURRENT_BINARY_DIR}/wow64")
set(wow64_nt_services "${REACTOS_SOURCE_DIR}/ntoskrnl/include/sysfuncs.h")
set(wow64_w32k_services_32 "${REACTOS_SOURCE_DIR}/win32ss/w32ksvc32.h")
set(wow64_w32k_services_64 "${REACTOS_SOURCE_DIR}/win32ss/w32ksvc64.h")
set(wow64_types "${REACTOS_SOURCE_DIR}/sdk/cmake/wow64types.txt")
set(wow64_headers
    "${REACTOS_SOURCE_DIR}/win32ss/include/ntuser.h"
    "${REACTOS_SOURCE_DIR}/sdk/include/psdk/ntgdi.h"
    "${REACTOS_SOURCE_DIR}/win32ss/include/ntgdibad.h"
    "${REACTOS_SOURCE_DIR}/win32ss/gdi/ntgdi/intgdi.h")
foreach(funcs cm dbgk ex io kd ke lpc mm ob po ps se)
    list(APPEND wow64_headers "${REACTOS_SOURCE_DIR}/sdk/include/ndk/${funcs}funcs.h")
endforeach()
set(wow64_prototypes "${wow64_tables_dir}/prototypes.csv")
# The values win32k, gdi32 and the NT headers are built with; _WIN64 stays undecided to describe both architectures
add_custom_command(OUTPUT "${wow64_prototypes}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${wow64_tables_dir}"
    COMMAND native-wow64protos
        -D__REACTOS__=1 -DLANGPACK=1 -DWINVER=0x502 -D_WIN32_WINNT=0x502 -D_WIN32_WINNT_WIN7=0x0601 -U_WIN64
        -t "${wow64_types}"
        -s "${wow64_nt_services}" -s "${wow64_w32k_services_32}" -s "${wow64_w32k_services_64}"
        -o "${wow64_prototypes}" ${wow64_headers}
    DEPENDS native-wow64protos "${wow64_types}" "${wow64_nt_services}"
        "${wow64_w32k_services_32}" "${wow64_w32k_services_64}" ${wow64_headers}
    VERBATIM)
set(wow64_tables "${wow64_tables_dir}/calls.csv" "${wow64_tables_dir}/args.csv" "${wow64_tables_dir}/args.inc")
foreach(table nt32to64 nt64to32 win32k32to64 win32k64to32)
    list(APPEND wow64_tables "${wow64_tables_dir}/${table}.inc" "${wow64_tables_dir}/${table}.csv")
endforeach()
add_custom_command(OUTPUT ${wow64_tables}
    COMMAND "${CMAKE_COMMAND}"
        "-DNT_SERVICES=${wow64_nt_services}"
        "-DWIN32_SERVICES_32=${wow64_w32k_services_32}"
        "-DWIN32_SERVICES_64=${wow64_w32k_services_64}"
        "-DPROTOTYPES=${wow64_prototypes}"
        "-DOUTPUT_DIR=${wow64_tables_dir}"
        "-DDBG=${DBG}"
        "-DCHECK=${WOW64_SYSCALL_CHECK}"
        -P "${REACTOS_SOURCE_DIR}/sdk/cmake/wow64syscalls.cmake"
    DEPENDS "${REACTOS_SOURCE_DIR}/sdk/cmake/wow64syscalls.cmake" "${wow64_prototypes}"
        "${wow64_nt_services}" "${wow64_w32k_services_32}" "${wow64_w32k_services_64}"
    VERBATIM)
# Consumers in the WoW64 runtime depend on this target explicitly. A native
# default build must not depend on the translation generator's input grammar.
add_custom_target(wow64_syscall_tables DEPENDS ${wow64_tables})
add_custom_target(wow64_syscall_tables_test
    COMMAND "${CMAKE_COMMAND}" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/wow64-tests"
        "-DWOW64PROTOS=$<TARGET_FILE:native-wow64protos>"
        -P "${REACTOS_SOURCE_DIR}/sdk/cmake/tests/wow64syscalls.cmake"
    DEPENDS native-wow64protos
    VERBATIM)
