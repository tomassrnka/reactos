
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x601 -DWINVER=0x601)

get_property(INCLUDE_DIRS DIRECTORY . PROPERTY INCLUDE_DIRECTORIES)
list(REMOVE_ITEM INCLUDE_DIRS "${REACTOS_SOURCE_DIR}/sdk/include/crt")
set_property(DIRECTORY . PROPERTY INCLUDE_DIRECTORIES ${INCLUDE_DIRS})
include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/ucrt ../msvcp90)

add_definitions(
    -D__WINESRC__
    -D_CRTIMP=
    -D_CRT_DECLARE_NONSTDC_NAMES=1
    -D_CRT_NONSTDC_NO_DEPRECATE
    -D_beginthreadex_start_routine_t=_beginthreadex_proc_type)

if(ARCH STREQUAL "i386")
    add_definitions(-D__ASM_USE_THISCALL_WRAPPER)
endif()

spec2def(msvcp140_1.dll msvcp140_1.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    msvcp140_1.c
    msvcp140_1.rc
    ${CMAKE_CURRENT_BINARY_DIR}/msvcp140_1_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/msvcp140_1.def)

if(NOT MSVC)
    # The UCRT headers declare the exec functions differently from the compiler builtins
    add_compile_options(-Wno-builtin-declaration-mismatch)
endif()

add_library(msvcp140_1 MODULE ${SOURCE})
set_module_type(msvcp140_1 win32dll ENTRYPOINT DllMain 12)
target_link_libraries(msvcp140_1 wine)
add_importlibs(msvcp140_1 vcruntime140 ucrtbase kernel32 ntdll)
add_dependencies(msvcp140_1 psdk)
if(NOT MSVC)
    # binutils write their own version (2.36) into the header; programs such as PHP compare the linker
    # version of the runtime that they find with the one that built them. The version is the one that Wine declares.
    add_custom_command(TARGET msvcp140_1 POST_BUILD
        COMMAND native-pefixup --linkerversion=14.50 $<TARGET_FILE:msvcp140_1>)
    set_property(TARGET msvcp140_1 APPEND PROPERTY LINK_DEPENDS $<TARGET_PROPERTY:native-pefixup,IMPORTED_LOCATION>)
endif()
add_cd_file(TARGET msvcp140_1 DESTINATION reactos/system32 FOR all)
