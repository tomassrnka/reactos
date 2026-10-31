
add_executable(opengl32_apitest dc_data_leak.c dc_data_stress.c icd_default.c make_current.c sw_extensions.c sw_pixelformat.c sw_render.c testlist.c)
target_link_libraries(opengl32_apitest wine)
set_module_type(opengl32_apitest win32cui)
add_importlibs(opengl32_apitest opengl32 gdi32 user32 msvcrt kernel32)
add_rostests_file(TARGET opengl32_apitest)
