
list(APPEND SOURCE
    common.c
    main.c
    mapping.c
    monitor.c
    poolchurn.c
    pressure.c
    procs.c
    probes.c
    vmapi.c)

add_executable(mmtorture ${SOURCE})
target_link_libraries(mmtorture ${PSEH_LIB})
set_module_type(mmtorture win32cui)
add_importlibs(mmtorture advapi32 ws2_32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET mmtorture SUBDIR suppl)
