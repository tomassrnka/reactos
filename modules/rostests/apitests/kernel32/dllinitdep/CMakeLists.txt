
# Parent helper: always initializes (no exports needed).
add_library(t116_dllinit_parent MODULE parentmod.c)
set_module_type(t116_dllinit_parent win32dll ENTRYPOINT DllMain 12 IMAGEBASE default)
add_importlibs(t116_dllinit_parent kernel32 ntdll)
add_dependencies(t116_dllinit_parent psdk)
add_rostests_file(TARGET t116_dllinit_parent)

# Child helper: initializes only when the parent is already loaded.
spec2def(t116_dllinit_child.dll childmod.spec ADD_IMPORTLIB)

list(APPEND CHILD_SRC
    childmod.c
    ${CMAKE_CURRENT_BINARY_DIR}/t116_dllinit_child_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/t116_dllinit_child.def)

add_library(t116_dllinit_child MODULE ${CHILD_SRC})
set_module_type(t116_dllinit_child win32dll ENTRYPOINT DllMain 12 IMAGEBASE default)
add_importlibs(t116_dllinit_child kernel32 ntdll)
add_dependencies(t116_dllinit_child psdk)
add_rostests_file(TARGET t116_dllinit_child)
