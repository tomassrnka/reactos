
get_property(INCLUDE_DIRS DIRECTORY . PROPERTY INCLUDE_DIRECTORIES)
list(REMOVE_ITEM INCLUDE_DIRS "${REACTOS_SOURCE_DIR}/sdk/include/crt")
set_property(DIRECTORY . PROPERTY INCLUDE_DIRECTORIES ${INCLUDE_DIRS})
include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/ucrt)

add_definitions(-D__WINESRC__ -D_CRTIMP= -D_CRT_DECLARE_NONSTDC_NAMES=1 -D_CRT_NONSTDC_NO_DEPRECATE)

spec2def(msvcp140_2.dll msvcp140_2.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    math.c
    msvcp140_2.rc
    ${CMAKE_CURRENT_BINARY_DIR}/msvcp140_2_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/msvcp140_2.def)

if(NOT MSVC)
    # The UCRT headers declare the exec functions differently from the compiler builtins
    add_compile_options(-Wno-builtin-declaration-mismatch)
endif()

add_library(msvcp140_2 MODULE ${SOURCE})
set_module_type(msvcp140_2 win32dll ENTRYPOINT 0)
target_link_libraries(msvcp140_2 wine)
add_importlibs(msvcp140_2 ucrtbase kernel32 ntdll)
add_dependencies(msvcp140_2 psdk)
if(NOT MSVC)
    # binutils write their own version (2.36) into the header; programs such as PHP compare the linker
    # version of the runtime that they find with the one that built them. The version is the one that Wine declares.
    add_custom_command(TARGET msvcp140_2 POST_BUILD
        COMMAND native-pefixup --linkerversion=14.50 $<TARGET_FILE:msvcp140_2>)
    set_property(TARGET msvcp140_2 APPEND PROPERTY LINK_DEPENDS $<TARGET_PROPERTY:native-pefixup,IMPORTED_LOCATION>)
endif()
add_cd_file(TARGET msvcp140_2 DESTINATION reactos/system32 FOR all)
