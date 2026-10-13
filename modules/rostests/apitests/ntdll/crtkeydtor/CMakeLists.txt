
spec2def(crtkeydtor.dll crtkeydtor.spec ADD_IMPORTLIB)

add_library(crtkeydtor MODULE
    crtkeydtor.c
    ${CMAKE_CURRENT_BINARY_DIR}/crtkeydtor.def)
set_module_type(crtkeydtor win32dll IMAGEBASE default)
add_importlibs(crtkeydtor msvcrt kernel32 ntdll)
add_dependencies(crtkeydtor psdk)

add_executable(crtkeydtor_static crtkeydtor_static.c)
set_module_type(crtkeydtor_static win32cui)
add_importlibs(crtkeydtor_static crtkeydtor msvcrt kernel32)
add_dependencies(crtkeydtor_static psdk)
