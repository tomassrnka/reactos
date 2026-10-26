
include_directories(include)

add_definitions(-D_CRTBLD)

include(except/except.cmake)
include(float/float.cmake)
include(math/math.cmake)
include(mem/mem.cmake)
include(misc/misc.cmake)
include(printf/printf.cmake)
include(search/search.cmake)
include(startup/startup.cmake)
include(stdlib/stdlib.cmake)
include(string/string.cmake)
include(wstring/wstring.cmake)

include(crtmath.cmake)
include(libcntpr.cmake)
include(msvcrtex.cmake)
include(oldnames.cmake)

add_library(user32_wsprintf
    printf/streamout.c
    printf/wstreamout.c
    printf/wsprintfA.c
    printf/wsprintfW.c
    printf/wvsprintfA.c
    printf/wvsprintfW.c
    printf/wvsnprintfA.c
    printf/wvsnprintfW.c
    string/strnlen.c
    string/wcsnlen.c
    string/mbstowcs_nt.c
    string/wcstombs_nt.c)

add_dependencies(user32_wsprintf psdk)
target_compile_definitions(user32_wsprintf PRIVATE _USER32_WSPRINTF)

# The sprintf family as exported by ntdll: libcntpr's code plus the NT6+ argument
# validation (kept out of libcntpr itself, which the kernel and drivers share)
add_library(ntdll_printf_nt6
    printf/_snprintf.c
    printf/_snwprintf.c
    printf/_swprintf.c
    printf/_vsnprintf.c
    printf/_vsnwprintf.c
    printf/sprintf.c
    printf/vsprintf.c)

add_dependencies(ntdll_printf_nt6 psdk asm)
target_compile_definitions(ntdll_printf_nt6 PRIVATE
    NO_RTL_INLINES
    _NTSYSTEM_
    _NTDLLBUILD_
    _LIBCNT_
    __CRT__NO_INLINE
    CRTDLL
    _NTDLL_NT6_PRINTF)

if(MSVC AND ARCH STREQUAL "i386")
    add_asm_files(ftol2_asm
        math/i386/ftol2_asm.s
        math/i386/ftoul2_legacy_asm.s
    )
    add_library(ftol2 ${ftol2_asm})
    set_target_properties(ftol2 PROPERTIES LINKER_LANGUAGE "C")
    add_dependencies(ftol2 asm)
endif()

add_library(dbgrpt misc/dbgrpt.cpp)
target_link_libraries(dbgrpt chkstk ${PSEH_LIB})
target_compile_definitions(dbgrpt PRIVATE
    _MT
    _DEBUG)
add_dependencies(dbgrpt psdk asm)
