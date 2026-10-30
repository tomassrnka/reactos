
spec2def(windows.system.profile.platformdiagnosticsandusagedatasettings.dll windows.system.profile.platformdiagnosticsandusagedatasettings.spec)

list(APPEND SOURCE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/windows.system.profile.platformdiagnosticsandusagedatasettings_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/windows.system.profile.platformdiagnosticsandusagedatasettings.def)

add_library(windows.system.profile.platformdiagnosticsandusagedatasettings MODULE ${SOURCE})
set_module_type(windows.system.profile.platformdiagnosticsandusagedatasettings win32dll UNICODE ENTRYPOINT DllMain 12)
target_link_libraries(windows.system.profile.platformdiagnosticsandusagedatasettings uuid wine)
add_importlibs(windows.system.profile.platformdiagnosticsandusagedatasettings combase ole32 msvcrt kernel32 ntdll)
add_dependencies(windows.system.profile.platformdiagnosticsandusagedatasettings psdk)
add_cd_file(TARGET windows.system.profile.platformdiagnosticsandusagedatasettings DESTINATION reactos/system32 FOR all)
