project(appcompat)

list(APPEND SOURCE
    dispmode.c
    forcedxsetup.c
    genral_hooks.c
    globmemstatus.c
    ignorefreelib.c
    ignoreloadlib.c
    layer_hooks.c
    versionconflict.c
    sysmain.c
    manifestcompat.c
    versionlie.c
    verifyversion.c
    testlist.c
    appshim_apitest.h)

add_executable(appshim_apitest ${SOURCE})
set_module_type(appshim_apitest win32cui)
add_importlibs(appshim_apitest advapi32 version msvcrt kernel32 ntdll)
add_rostests_file(TARGET appshim_apitest)

add_executable(appshim_sysmain_probe sysmain_probe.c sysmain_probe.rc)
set_module_type(appshim_sysmain_probe win32cui)
add_importlibs(appshim_sysmain_probe advapi32 msvcrt kernel32)
add_rostests_file(TARGET appshim_sysmain_probe)

add_executable(appshim_sysmain_other sysmain_probe.c sysmain_other.rc)
set_module_type(appshim_sysmain_other win32cui)
add_importlibs(appshim_sysmain_other advapi32 msvcrt kernel32)
add_rostests_file(TARGET appshim_sysmain_other)

add_executable(appshim_sysmain_firefox158 sysmain_probe.c sysmain_firefox158.rc)
set_module_type(appshim_sysmain_firefox158 win32cui)
add_importlibs(appshim_sysmain_firefox158 advapi32 msvcrt kernel32)
add_rostests_file(TARGET appshim_sysmain_firefox158)

add_executable(appshim_sysmain_firefox115 sysmain_probe.c sysmain_firefox115.rc)
set_module_type(appshim_sysmain_firefox115 win32cui)
add_importlibs(appshim_sysmain_firefox115 advapi32 msvcrt kernel32)
add_rostests_file(TARGET appshim_sysmain_firefox115)

add_executable(appshim_manifestver_none manifestver_probe.c manifestver_none.rc)
set_module_type(appshim_manifestver_none win32cui)
add_importlibs(appshim_manifestver_none msvcrt kernel32 ntdll)
add_rostests_file(TARGET appshim_manifestver_none)

add_executable(appshim_manifestver_vista manifestver_probe.c manifestver_vista.rc)
set_module_type(appshim_manifestver_vista win32cui)
add_importlibs(appshim_manifestver_vista msvcrt kernel32 ntdll)
add_rostests_file(TARGET appshim_manifestver_vista)

add_executable(appshim_manifestver_span manifestver_probe.c manifestver_span.rc)
set_module_type(appshim_manifestver_span win32cui)
add_importlibs(appshim_manifestver_span msvcrt kernel32 ntdll)
add_rostests_file(TARGET appshim_manifestver_span)

add_executable(appshim_manifestver_vista_win10 manifestver_probe.c manifestver_vista_win10.rc)
set_module_type(appshim_manifestver_vista_win10 win32cui)
add_importlibs(appshim_manifestver_vista_win10 msvcrt kernel32 ntdll)
add_rostests_file(TARGET appshim_manifestver_vista_win10)

add_dependencies(appshim_apitest appshim_sysmain_probe appshim_sysmain_other
                 appshim_sysmain_firefox158 appshim_sysmain_firefox115
                 appshim_manifestver_none appshim_manifestver_vista
                 appshim_manifestver_span appshim_manifestver_vista_win10)
