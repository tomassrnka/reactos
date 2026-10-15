
spec2def(explorerframe.dll explorerframe.spec)

list(APPEND SOURCE
    ebrowser.c
    explorerframe_main.c
    nstc.c
    explorerframe_main.h)

add_library(explorerframe MODULE
    ${SOURCE}
    explorerframe.rc
    ${CMAKE_CURRENT_BINARY_DIR}/explorerframe.def)

set_module_type(explorerframe win32dll)
add_idl_reg_scripts(explorerframe registry explorerframe_classes.idl)
target_link_libraries(explorerframe uuid wine wine_dll_register oldnames)
add_importlibs(explorerframe comctl32 shell32 shlwapi user32 gdi32 ole32 msvcrt kernel32 ntdll)
add_dependencies(explorerframe psdk)
add_cd_file(TARGET explorerframe DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(explorerframe) # CORE-5743: No ARRAY_SIZE and CONST_VTABLE
