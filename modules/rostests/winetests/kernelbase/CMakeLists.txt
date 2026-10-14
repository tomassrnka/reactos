
include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine)
remove_definitions(-DWINVER=0x502 -D_WIN32_WINNT=0x502)
add_definitions(-DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000000)

list(APPEND SOURCE
    path.c)

list(APPEND PCH_SKIP_SOURCE
    testlist.c)

add_executable(kernelbase_winetest
    ${SOURCE}
    ${PCH_SKIP_SOURCE})

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(kernelbase_winetest PRIVATE
        -Wno-format
        -Wno-unused-function)
endif()

set_module_type(kernelbase_winetest win32cui)
add_importlibs(kernelbase_winetest msvcrt kernel32)
add_rostests_file(TARGET kernelbase_winetest)
