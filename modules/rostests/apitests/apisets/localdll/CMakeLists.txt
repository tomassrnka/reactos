
add_library(apisets_localdll MODULE localdll.c)
set_module_type(apisets_localdll win32dll ENTRYPOINT DllMain 12)
add_importlibs(apisets_localdll kernel32)
add_dependencies(apisets_localdll psdk)
