add_definitions(-D__WINESRC__)
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x601)

spec2def(prntvpt.dll prntvpt.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    main.c
    ticket.c
    ${CMAKE_CURRENT_BINARY_DIR}/prntvpt_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/prntvpt.def)

add_library(prntvpt MODULE ${SOURCE})
set_module_type(prntvpt win32dll UNICODE)
target_link_libraries(prntvpt wine uuid)
add_importlibs(prntvpt gdi32 winspool ole32 oleaut32 msvcrt kernel32 ntdll)
add_dependencies(prntvpt psdk)
add_cd_file(TARGET prntvpt DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(prntvpt)
