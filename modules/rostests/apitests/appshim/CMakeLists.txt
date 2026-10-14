project(appcompat)

list(APPEND SOURCE
    dispmode.c
    forcedxsetup.c
    genral_hooks.c
    globmemstatus.c
    ignorefreelib.c
    ignoreloadlib.c
    layer_hooks.c
    sysmain.c
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

add_dependencies(appshim_apitest appshim_sysmain_probe appshim_sysmain_other)
