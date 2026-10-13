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

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --abbrev=7 --long --always
            WORKING_DIRECTORY ${REACTOS_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_DESCRIBE_REVISION
            RESULT_VARIABLE GIT_CALL_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(GIT_CALL_RESULT EQUAL 0)
            set(REVISION "${GIT_DESCRIBE_REVISION}")
        endif()

        # AjarOS build number: commits not in upstream's 0.4.17-dev tag (pinned by sha)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD ^129ef359f0fab07c788e4921d7236c0be5cf3c57
            WORKING_DIRECTORY ${REACTOS_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_BRAND_BUILD
            RESULT_VARIABLE GIT_CALL_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(GIT_CALL_RESULT EQUAL 0 AND GIT_BRAND_BUILD MATCHES "^[0-9]+$")
            set(BRAND_BUILD "${GIT_BRAND_BUILD}")
        endif()

        # A cut is a date tag <YYYY>.<MM>.<DD>-<build> on exactly this commit
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD
            WORKING_DIRECTORY ${REACTOS_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_BRAND_TAGS
            RESULT_VARIABLE GIT_CALL_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(GIT_CALL_RESULT EQUAL 0 AND NOT GIT_BRAND_TAGS STREQUAL "")
            string(REPLACE "\n" ";" GIT_BRAND_TAGS "${GIT_BRAND_TAGS}")
            foreach(GIT_BRAND_TAG ${GIT_BRAND_TAGS})
                if(GIT_BRAND_TAG MATCHES "^([0-9][0-9][0-9][0-9]\\.[0-9][0-9]\\.[0-9][0-9])-([0-9]+)$")
                    if(NOT DEFINED BRAND_DATE OR CMAKE_MATCH_2 STREQUAL "${GIT_BRAND_BUILD}")
                        set(BRAND_DATE "${CMAKE_MATCH_1}")
                        set(BRAND_TAG_BUILD "${CMAKE_MATCH_2}")
                    endif()
                endif()
            endforeach()
            if(DEFINED BRAND_TAG_BUILD)
                set(BRAND_BUILD "${BRAND_TAG_BUILD}")
            endif()
        endif()
    endif()
endif()

# Product version shown to users. REVISION, BuildLab and the NT version numbers that programs read are unchanged.
string(SUBSTRING "${COMMIT_HASH}" 0 7 BRAND_COMMIT_SHORT)
if(NOT DEFINED BRAND_BUILD OR BRAND_BUILD STREQUAL "")
    set(BRAND_BUILD "unknown")
    message(WARNING "AjarOS build number unavailable (no git, NO_REACTOS_BUILDNO, or a shallow clone without commit 129ef359f0); the version reads 'build unknown'")
endif()
if(DEFINED BRAND_DATE)
    set(BRAND_VERSION_MAIN "${BRAND_DATE}")
    set(BRAND_VERSION_DETAIL "build ${BRAND_BUILD}")
elseif(COMMIT_HASH STREQUAL "unknown-hash")
    set(BRAND_VERSION_MAIN "build ${BRAND_BUILD}")
    set(BRAND_VERSION_DETAIL "dev")
else()
    set(BRAND_VERSION_MAIN "build ${BRAND_BUILD}")
    set(BRAND_VERSION_DETAIL "dev, g${BRAND_COMMIT_SHORT}")
endif()
set(BRAND_VERSION "${BRAND_VERSION_MAIN} (${BRAND_VERSION_DETAIL})")

configure_file(sdk/include/reactos/version.h.cmake ${REACTOS_BINARY_DIR}/sdk/include/reactos/version.h)
configure_file(sdk/include/reactos/buildno.h.cmake ${REACTOS_BINARY_DIR}/sdk/include/reactos/buildno.h)
