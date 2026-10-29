spec2def(windows.data.pdf.dll windows.data.pdf.spec)

list(APPEND SOURCE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/windows.data.pdf_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/windows.data.pdf.def)

add_library(windows.data.pdf MODULE ${SOURCE})
set_module_type(windows.data.pdf win32dll UNICODE ENTRYPOINT DllMain 12)
target_link_libraries(windows.data.pdf wine)
add_importlibs(windows.data.pdf msvcrt kernel32 ntdll)
add_dependencies(windows.data.pdf psdk)
add_cd_file(TARGET windows.data.pdf DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(windows.data.pdf)
