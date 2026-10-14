
# The Concurrency Runtime of Wine: concrt140.c and details.c here, concurrency.c and
# exception_ptr.c of the msvcrt directory compiled as the 14.0 runtime (_MSVCR_VER=140)

remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x601 -DWINVER=0x601)

get_property(INCLUDE_DIRS DIRECTORY . PROPERTY INCLUDE_DIRECTORIES)
list(REMOVE_ITEM INCLUDE_DIRS "${REACTOS_SOURCE_DIR}/sdk/include/crt")
set_property(DIRECTORY . PROPERTY INCLUDE_DIRECTORIES ${INCLUDE_DIRS})
include_directories(../msvcrt/include)
include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine ../msvcrt)

add_definitions(
    -D__WINESRC__
    -D_MSVCR_VER=140
    -D_CONCRT=
    -D_CRTIMP=
    -DI64_MIN=_I64_MIN
    -DI64_MAX=_I64_MAX
    -DUI64_MAX=_UI64_MAX)

if(ARCH STREQUAL "i386")
    add_definitions(-D__ASM_USE_THISCALL_WRAPPER)
endif()

spec2def(concrt140.dll concrt140.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    concrt140.c
    details.c
    ../msvcrt/concurrency.c
    ../msvcrt/exception_ptr.c
    concrt140.rc
    ${CMAKE_CURRENT_BINARY_DIR}/concrt140_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/concrt140.def)

# The Wine sources expect these headers from their callers
set_source_files_properties(details.c PROPERTIES COMPILE_OPTIONS "-include;windef.h;-include;winbase.h")

add_library(concrt140 MODULE ${SOURCE})
set_module_type(concrt140 win32dll ENTRYPOINT DllMain 12)
target_link_libraries(concrt140 wine)
add_importlibs(concrt140 vcruntime140 ucrtbase kernel32 kernel32_vista ntdll_vista ntdll)
add_dependencies(concrt140 psdk)
if(NOT MSVC)
    # binutils write their own version (2.36) into the header; programs such as PHP compare the linker
    # version of the runtime that they find with the one that built them. The version is the one that Wine declares.
    add_custom_command(TARGET concrt140 POST_BUILD
        COMMAND native-pefixup --linkerversion=14.50 $<TARGET_FILE:concrt140>)
    set_property(TARGET concrt140 APPEND PROPERTY LINK_DEPENDS $<TARGET_PROPERTY:native-pefixup,IMPORTED_LOCATION>)
endif()
add_cd_file(TARGET concrt140 DESTINATION reactos/system32 FOR all)
