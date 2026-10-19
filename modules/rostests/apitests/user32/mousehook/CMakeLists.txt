spec2def(user32_mousehook.dll mousehook.spec)

list(APPEND SOURCE
    mousehook.c
    ${CMAKE_CURRENT_BINARY_DIR}/user32_mousehook_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/user32_mousehook.def)

add_library(user32_mousehook MODULE ${SOURCE})
set_module_type(user32_mousehook win32dll ENTRYPOINT 0 IMAGEBASE default)
add_importlibs(user32_mousehook msvcrt user32 kernel32)
add_rostests_file(TARGET user32_mousehook)
