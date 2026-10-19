
# Built from the msvcp90 sources of Wine, like Wine does (PARENTSRC), with the version 14.0 behaviour

remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x601 -DWINVER=0x601)

# Replace the global CRT include directory with the UCRT include directory
get_property(INCLUDE_DIRS DIRECTORY . PROPERTY INCLUDE_DIRECTORIES)
list(REMOVE_ITEM INCLUDE_DIRS "${REACTOS_SOURCE_DIR}/sdk/include/crt")
set_property(DIRECTORY . PROPERTY INCLUDE_DIRECTORIES ${INCLUDE_DIRS})
include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/ucrt ../msvcp90)

add_definitions(
    -D__WINESRC__
    -D_MSVCP_VER=140
    -D_CRT_RAND_S
    -D_CRTIMP=
    -D_CRT_DECLARE_NONSTDC_NAMES=1
    -D_CRT_NONSTDC_NO_DEPRECATE
    # the Wine headers call the type of a thread start routine differently
    -D_beginthreadex_start_routine_t=_beginthreadex_proc_type)

if(ARCH STREQUAL "i386")
    add_definitions(-D__ASM_USE_THISCALL_WRAPPER)
endif()

spec2def(msvcp140.dll msvcp140.spec ADD_IMPORTLIB)

list(APPEND WINE_SOURCE
    ../msvcp90/details.c
    ../msvcp90/exception.c
    ../msvcp90/ios.c
    ../msvcp90/locale.c
    ../msvcp90/math.c
    ../msvcp90/memory.c
    ../msvcp90/misc.c
    ../msvcp90/msvcp_main.c
    ../msvcp90/string.c)

# The Wine sources expect these declarations from their headers
set_source_files_properties(${WINE_SOURCE} PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/reactos_compat.h")

list(APPEND SOURCE
    ${WINE_SOURCE}
    msvcp140.rc
    ${CMAKE_CURRENT_BINARY_DIR}/msvcp140_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/msvcp140.def)

if(NOT MSVC)
    # The UCRT headers declare the exec functions differently from the compiler builtins
    add_compile_options(-Wno-builtin-declaration-mismatch)
endif()

add_library(msvcp140 MODULE ${SOURCE})
set_module_type(msvcp140 win32dll ENTRYPOINT DllMain 12)
target_link_libraries(msvcp140 wine)
add_importlibs(msvcp140 vcruntime140 ucrtbase kernel32 kernel32_vista ntdll)
add_dependencies(msvcp140 psdk)
if(NOT MSVC)
    # binutils write their own version (2.36) into the header; programs such as PHP compare the linker
    # version of the runtime that they find with the one that built them. The version is the one that Wine declares.
    add_custom_command(TARGET msvcp140 POST_BUILD
        COMMAND native-pefixup --linkerversion=14.50 $<TARGET_FILE:msvcp140>)
    set_property(TARGET msvcp140 APPEND PROPERTY LINK_DEPENDS $<TARGET_PROPERTY:native-pefixup,IMPORTED_LOCATION>)
endif()
add_cd_file(TARGET msvcp140 DESTINATION reactos/system32 FOR all)
