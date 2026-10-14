
# amd64 only: the x64 exception frame handler format 4

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
    -D_CRTIMP=
    -DI64_MIN=_I64_MIN
    -DI64_MAX=_I64_MAX
    -DUI64_MAX=_UI64_MAX)

spec2def(vcruntime140_1.dll vcruntime140_1.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    main.c
    ../msvcrt/except_x86_64.c
    ../msvcrt/handler4.c
    vcruntime140_1.rc
    ${CMAKE_CURRENT_BINARY_DIR}/vcruntime140_1_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/vcruntime140_1.def)

# handler4.c of the msvcrt directory expects the Windows headers before wine/exception.h
set_source_files_properties(../msvcrt/handler4.c PROPERTIES COMPILE_OPTIONS "-include;windef.h;-include;winbase.h;-include;winternl.h")

add_library(vcruntime140_1 MODULE ${SOURCE})
set_module_type(vcruntime140_1 win32dll ENTRYPOINT DllMain 12)
target_link_libraries(vcruntime140_1 wine ${PSEH_LIB})
add_importlibs(vcruntime140_1 vcruntime140 ucrtbase kernel32 ntdll)
add_dependencies(vcruntime140_1 psdk)
if(NOT MSVC)
    # binutils write their own version (2.36) into the header; programs such as PHP compare the linker
    # version of the runtime that they find with the one that built them. The version is the one that Wine declares.
    add_custom_command(TARGET vcruntime140_1 POST_BUILD
        COMMAND native-pefixup --linkerversion=14.50 $<TARGET_FILE:vcruntime140_1>)
    set_property(TARGET vcruntime140_1 APPEND PROPERTY LINK_DEPENDS $<TARGET_PROPERTY:native-pefixup,IMPORTED_LOCATION>)
endif()
add_cd_file(TARGET vcruntime140_1 DESTINATION reactos/system32 FOR all)
