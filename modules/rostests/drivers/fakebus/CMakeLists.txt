
add_library(fakebus MODULE fakebus.c)
set_module_type(fakebus kernelmodedriver)
add_importlibs(fakebus ntoskrnl hal)
add_rostests_file(TARGET fakebus)
