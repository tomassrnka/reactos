
# NTFS checker: ntfsck of ntfsprogs-plus (ntfsprogs-plus/, see README.txt) with the libntfs
# subset it uses, on NT native I/O. Built for ntfsck.dll.
#
# Two libraries, because the imported headers (types.h, debug.h, security.h, ...) clash with
# the NT and ReactOS headers: ntfscklib holds the imported sources and the files that use
# their types, ntfsckros the files that use the NT API. They meet in ntfsck_lib.h and ntio.h.

list(APPEND UPSTREAM_SOURCE
    ntfsprogs-plus/libntfs/acls.c
    ntfsprogs-plus/libntfs/attrib.c
    ntfsprogs-plus/libntfs/attrlist.c
    ntfsprogs-plus/libntfs/bitmap.c
    ntfsprogs-plus/libntfs/bootsect.c
    ntfsprogs-plus/libntfs/cache.c
    ntfsprogs-plus/libntfs/collate.c
    ntfsprogs-plus/libntfs/compress.c
    ntfsprogs-plus/libntfs/device.c
    ntfsprogs-plus/libntfs/dir.c
    ntfsprogs-plus/libntfs/ea.c
    ntfsprogs-plus/libntfs/fsck.c
    ntfsprogs-plus/libntfs/index.c
    ntfsprogs-plus/libntfs/inode.c
    ntfsprogs-plus/libntfs/lcnalloc.c
    ntfsprogs-plus/libntfs/lib_utils.c
    ntfsprogs-plus/libntfs/logfile.c
    ntfsprogs-plus/libntfs/logging.c
    ntfsprogs-plus/libntfs/mft.c
    ntfsprogs-plus/libntfs/misc.c
    ntfsprogs-plus/libntfs/mst.c
    ntfsprogs-plus/libntfs/object_id.c
    ntfsprogs-plus/libntfs/problem.c
    ntfsprogs-plus/libntfs/realpath.c
    ntfsprogs-plus/libntfs/reparse.c
    ntfsprogs-plus/libntfs/runlist.c
    ntfsprogs-plus/libntfs/security.c
    ntfsprogs-plus/libntfs/unistr.c
    ntfsprogs-plus/libntfs/volume.c
    ntfsprogs-plus/src/ntfsck.c
    ntfsprogs-plus/src/utils.c)

add_library(ntfscklib
    ${UPSTREAM_SOURCE}
    ntdevice.c
    stats.c)
target_include_directories(ntfscklib BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ntfsprogs-plus/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ntfsprogs-plus/src
    ${CMAKE_CURRENT_SOURCE_DIR}/posix)
target_compile_definitions(ntfscklib PRIVATE HAVE_CONFIG_H _CRT_DECLARE_NONSTDC_NAMES=1)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Upstream code as it is: these warnings are its style, not defects of the port.
    set_source_files_properties(${UPSTREAM_SOURCE} PROPERTIES COMPILE_OPTIONS
        "-Wno-pointer-arith;-Wno-unused-variable;-Wno-unused-function;-Wno-unused-but-set-variable;-Wno-missing-braces;-Wno-pointer-sign;-Wno-sign-compare;-Wno-unused-parameter;-Wno-unused-label")
endif()
add_dependencies(ntfscklib psdk xdk)

add_library(ntfsckros
    compat.c
    ntfschk.c
    ntio.c)
target_include_directories(ntfsckros PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ntfsckros PRIVATE _CRT_DECLARE_NONSTDC_NAMES=1)
target_link_libraries(ntfsckros ntfscklib ${PSEH_LIB})
add_dependencies(ntfsckros psdk xdk)
