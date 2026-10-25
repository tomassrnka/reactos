
list(APPEND SOURCE afdstub.c)

add_library(afdstub MODULE ${SOURCE})
set_module_type(afdstub kernelmodedriver)
add_importlibs(afdstub ntoskrnl hal)
add_rostests_file(TARGET afdstub)
