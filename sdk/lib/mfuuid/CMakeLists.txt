
add_library(mfuuid mfplay.c mfuuid.c)
target_compile_definitions(mfuuid PRIVATE __WINESRC__)
add_dependencies(mfuuid psdk wineheaders d3d_idl_headers)
