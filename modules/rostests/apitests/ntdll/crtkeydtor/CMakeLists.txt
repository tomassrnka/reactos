
spec2def(crtkeydtor.dll crtkeydtor.spec ADD_IMPORTLIB)
spec2def(crtkeydtor_tls.dll crtkeydtor_tls.spec ADD_IMPORTLIB)

# Default CRT startup, no TLS directory
add_library(crtkeydtor MODULE
    crtkeydtor.c
    ${CMAKE_CURRENT_BINARY_DIR}/crtkeydtor.def)
set_module_type(crtkeydtor win32dll IMAGEBASE default)
add_importlibs(crtkeydtor msvcrt kernel32 ntdll)
add_dependencies(crtkeydtor psdk)

# The same with a TLS directory (it references _tls_index)
add_library(crtkeydtor_tls MODULE
    crtkeydtor.c
    ${CMAKE_CURRENT_BINARY_DIR}/crtkeydtor_tls.def)
set_module_type(crtkeydtor_tls win32dll IMAGEBASE default)
target_compile_definitions(crtkeydtor_tls PRIVATE WITH_TLS_DIRECTORY)
add_importlibs(crtkeydtor_tls msvcrt kernel32 ntdll)
add_dependencies(crtkeydtor_tls psdk)

# EXEs that import them, so they are loaded at process start
add_executable(crtkeydtor_static crtkeydtor_static.c)
set_module_type(crtkeydtor_static win32cui)
add_importlibs(crtkeydtor_static crtkeydtor msvcrt kernel32)
add_dependencies(crtkeydtor_static psdk)

add_executable(crtkeydtor_tls_static crtkeydtor_static.c)
set_module_type(crtkeydtor_tls_static win32cui)
add_importlibs(crtkeydtor_tls_static crtkeydtor_tls msvcrt kernel32)
add_dependencies(crtkeydtor_tls_static psdk)
