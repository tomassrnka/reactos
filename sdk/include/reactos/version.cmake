option(NO_REACTOS_BUILDNO "If true, disables the generation of buildno.h and version.h for each configure" OFF)

if (NOT NO_REACTOS_BUILDNO)
    string(TIMESTAMP KERNEL_VERSION_BUILD %Y%m%d UTC)
else()
    set(KERNEL_VERSION_BUILD "custom")
endif()

set(KERNEL_VERSION_MAJOR "0")
set(KERNEL_VERSION_MINOR "4")
set(KERNEL_VERSION_PATCH_LEVEL "17")
set(COPYRIGHT_YEAR "2026")

# KERNEL_VERSION_BUILD_TYPE is "dev" for Git builds
# or "RC1", "RC2", "" for releases.
set(KERNEL_VERSION_BUILD_TYPE "dev")

set(KERNEL_VERSION "${KERNEL_VERSION_MAJOR}.${KERNEL_VERSION_MINOR}.${KERNEL_VERSION_PATCH_LEVEL}-${WINARCH}")
if(NOT KERNEL_VERSION_BUILD_TYPE STREQUAL "")
    set(KERNEL_VERSION "${KERNEL_VERSION}-${KERNEL_VERSION_BUILD_TYPE}")
endif()

math(EXPR REACTOS_DLL_VERSION_MAJOR "${KERNEL_VERSION_MAJOR}+42")
set(DLL_VERSION_STR "${REACTOS_DLL_VERSION_MAJOR}.${KERNEL_VERSION_MINOR}.${KERNEL_VERSION_PATCH_LEVEL}")
if(NOT KERNEL_VERSION_BUILD_TYPE STREQUAL "")
    set(DLL_VERSION_STR "${DLL_VERSION_STR}-${KERNEL_VERSION_BUILD_TYPE}")
endif()

# Get Git revision through "git describe"
set(COMMIT_HASH "unknown-hash")
set(REVISION "unknown-revision")

if((EXISTS "${REACTOS_SOURCE_DIR}/.git") AND (NOT NO_REACTOS_BUILDNO))
    find_package(Git)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY ${REACTOS_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            RESULT_VARIABLE GIT_CALL_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(GIT_CALL_RESULT EQUAL 0)
            set(COMMIT_HASH "${GIT_COMMIT_HASH}")
        endif()

        # Only build tags (<YYYY>.<MM>.<DD>-<build>) name a revision; other tags never show up in it
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=7 --long
                    --match "[0-9][0-9][0-9][0-9].[0-9][0-9].[0-9][0-9]-[0-9]*"
                    --exclude "????.??.??-*[!0-9]*"
            WORKING_DIRECTORY ${REACTOS_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_DESCRIBE_REVISION
            RESULT_VARIABLE GIT_CALL_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(GIT_CALL_RESULT EQUAL 0)
            set(REVISION "${GIT_DESCRIBE_REVISION}")
        else()
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                WORKING_DIRECTORY ${REACTOS_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_SHORT_HASH
                RESULT_VARIABLE GIT_CALL_RESULT
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(GIT_CALL_RESULT EQUAL 0)
                set(REVISION "g${GIT_SHORT_HASH}")
            endif()
        endif()
    endif()
endif()

# nitro identification, shown in the OS UI next to the ReactOS version (desktop watermark, About/winver).
# Cosmetic only: no API-visible version number depends on it.
set(NITRO_VERSION "dev" CACHE STRING "nitro release version shown in the OS UI")
set(NITRO_SHA "" CACHE STRING "nitro short git SHA shown in the OS UI (default: HEAD of the source tree)")
if(NITRO_SHA STREQUAL "")
    set(NITRO_SHA "nogit")
    if((EXISTS "${REACTOS_SOURCE_DIR}/.git") AND (NOT NO_REACTOS_BUILDNO) AND GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
            WORKING_DIRECTORY ${REACTOS_SOURCE_DIR}
            OUTPUT_VARIABLE NITRO_GIT_SHA
            RESULT_VARIABLE GIT_CALL_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(GIT_CALL_RESULT EQUAL 0)
            set(NITRO_SHA "${NITRO_GIT_SHA}")
        endif()
    endif()
endif()

foreach(_nitro_var NITRO_VERSION NITRO_SHA)
    if(NOT ${_nitro_var} MATCHES "^[A-Za-z0-9._+-]+$")
        message(FATAL_ERROR "${_nitro_var} must only contain letters, digits and . _ + - (got \"${${_nitro_var}}\")")
    endif()
endforeach()

# The whole display string is one template: reorder or reword it here (or with -DNITRO_TITLE_TEMPLATE=...).
# @NITRO_ID@ is "dev <sha>" for dev builds and NITRO_VERSION for releases.
if(NITRO_VERSION STREQUAL "dev")
    set(NITRO_ID "dev ${NITRO_SHA}")
else()
    set(NITRO_ID "${NITRO_VERSION}")
endif()
set(NITRO_TITLE_TEMPLATE "ReactOS (Nitro) - @NITRO_ID@" CACHE STRING "OS UI title: desktop watermark first line and About/winver")
set(NITRO_NOTICE "Unofficial build, not supported by the ReactOS project" CACHE STRING "OS UI notice shown in About/winver")
string(CONFIGURE "${NITRO_TITLE_TEMPLATE}" NITRO_TITLE @ONLY)
foreach(_nitro_var NITRO_TITLE NITRO_NOTICE)
    if(${_nitro_var} MATCHES "[\"\\\\]")
        message(FATAL_ERROR "${_nitro_var} must not contain quotes or backslashes")
    endif()
endforeach()

configure_file(sdk/include/reactos/version.h.cmake ${REACTOS_BINARY_DIR}/sdk/include/reactos/version.h)
configure_file(sdk/include/reactos/buildno.h.cmake ${REACTOS_BINARY_DIR}/sdk/include/reactos/buildno.h)
