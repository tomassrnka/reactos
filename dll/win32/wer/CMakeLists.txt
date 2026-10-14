spec2def(wer.dll wer.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/wer_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/wer.def)

add_library(wer MODULE ${SOURCE})
set_module_type(wer win32dll UNICODE ENTRYPOINT DllMain 12)
target_link_libraries(wer wine)
add_importlibs(wer advapi32 msvcrt kernel32 ntdll)
add_dependencies(wer psdk)
add_cd_file(TARGET wer DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(wer)
