
# win32k.h here is a user mode stand-in, so that the DIB engine source compiles into a test
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${REACTOS_SOURCE_DIR}/modules/rostests/apitests/include)

add_executable(win32kdib_unittest
    StretchBlt.c
    ${REACTOS_SOURCE_DIR}/win32ss/gdi/dib/stretchblt.c
    testlist.c)

set_module_type(win32kdib_unittest win32cui)
add_importlibs(win32kdib_unittest msvcrt kernel32 ntdll)
add_rostests_file(TARGET win32kdib_unittest)
