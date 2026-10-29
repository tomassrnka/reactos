
spec2def(mi.dll mi.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    guid.c
    instance.c
    main.c
    session.c)

add_library(mi MODULE
    ${SOURCE}
    mi.rc
    ${CMAKE_CURRENT_BINARY_DIR}/mi.def)

set_module_type(mi win32dll UNICODE)
target_link_libraries(mi wine uuid)
add_importlibs(mi ole32 oleaut32 msvcrt kernel32 ntdll)
add_cd_file(TARGET mi DESTINATION reactos/system32 FOR all)
