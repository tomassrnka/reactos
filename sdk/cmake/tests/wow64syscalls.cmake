cmake_minimum_required(VERSION 3.17)
if(NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "Set TEST_DIR to a scratch directory")
endif()
get_filename_component(generator "${CMAKE_CURRENT_LIST_DIR}/../wow64syscalls.cmake" ABSOLUTE)
file(REMOVE_RECURSE "${TEST_DIR}/out")
file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/nt.h" "#ifndef SYSFUNCS_NT6_ONLY\nSVC_(First, 1)\n#endif\n#ifndef SYSFUNCS_NT5_ONLY\nSVC_(Appended, 2)\n#endif\n")
file(WRITE "${TEST_DIR}/32.h" "// comment SVC_(Ignored, 0)\n SVC_(Alpha, 2)\n SVC_(Beta, 4)\n SVC_(Only32, 0)\n")
file(WRITE "${TEST_DIR}/64.h" "SVC_(Beta, 3) // reordered and different\nSVC_(Alpha, 2) /* same slots */\nSVC_(Only64, 1)\n")

function(run_generator expected)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DNT_SERVICES=${TEST_DIR}/nt.h"
        "-DWIN32_SERVICES_32=${TEST_DIR}/32.h"
        "-DWIN32_SERVICES_64=${TEST_DIR}/64.h"
        "-DOUTPUT_DIR=${TEST_DIR}/out" ${ARGN} -P "${generator}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected STREQUAL "pass" AND NOT result EQUAL 0)
        message(FATAL_ERROR "Generator failed: ${output}\n${error}")
    elseif(expected STREQUAL "fail" AND result EQUAL 0)
        message(FATAL_ERROR "Generator accepted invalid input")
    endif()
    # message() wraps long lines
    string(REGEX REPLACE "[ \n]+" " " normalized "${output} ${error}")
    set(generator_output "${normalized}" PARENT_SCOPE)
endfunction()

function(expect_in path text)
    file(READ "${path}" contents)
    string(FIND "${contents}" "${text}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${path}: missing ${text}")
    endif()
endfunction()

function(expect file text)
    expect_in("${TEST_DIR}/out/${file}" "${text}")
endfunction()

function(expect_missing_in path text)
    file(READ "${path}" contents)
    string(FIND "${contents}" "${text}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "${path}: unexpected ${text}")
    endif()
endfunction()

# Without prototypes only counts, missing rows and conditionals are known
run_generator(pass)
expect(nt32to64.inc "WOW64_SYSCALL(NtAppended, 0x1, 0x1, 2, 2, 0)")
expect(win32k32to64.inc "WOW64_SYSCALL(NtAlpha, 0x1000, 0x1001, 2, 2, 0)")
expect(win32k32to64.inc "WOW64_SYSCALL(NtBeta, 0x1001, 0x1000, 4, 3, 2)")
expect(win32k32to64.inc "WOW64_SYSCALL(NtOnly32, 0x1002, 0xffff, 0, 0, 1)")
expect(win32k64to32.inc "WOW64_SYSCALL(NtOnly64, 0x1002, 0xffff, 1, 0, 1)")
expect(win32k32to64.csv "NtBeta,0x1001,0x1000,4,3,2")
if(EXISTS "${TEST_DIR}/out/calls.csv")
    message(FATAL_ERROR "calls.csv written without prototypes")
endif()
file(SHA256 "${TEST_DIR}/out/win32k32to64.inc" before)
run_generator(pass)
file(SHA256 "${TEST_DIR}/out/win32k32to64.inc" after)
if(NOT before STREQUAL after)
    message(FATAL_ERROR "Output is not deterministic")
endif()
run_generator(fail -DSTRICT=ON)

# Conditional rows: flag 4 on both sides, also when the conditional side is not built
file(APPEND "${TEST_DIR}/32.h" "#ifdef _WIN64\nSVC_(WideOnly, 1)\n#endif\n#if DBG\nSVC_(DebugOnly, 1)\n#endif\nSVC_(Tail, 1)\n")
file(APPEND "${TEST_DIR}/64.h" "SVC_(DebugOnly, 1)\nSVC_(WideOnly, 1)\n")
run_generator(pass -DDBG=OFF)
expect(win32k32to64.inc "WOW64_SYSCALL(NtTail, 0x1003, 0xffff, 1, 0, 1)")
expect(win32k64to32.inc "WOW64_SYSCALL(NtDebugOnly, 0x1003, 0xffff, 1, 0, 5)")
expect(win32k64to32.inc "WOW64_SYSCALL(NtWideOnly, 0x1004, 0xffff, 1, 0, 5)")
run_generator(pass -DDBG=ON)
expect(win32k32to64.inc "WOW64_SYSCALL(NtDebugOnly, 0x1003, 0x1003, 1, 1, 4)")
expect(win32k32to64.inc "WOW64_SYSCALL(NtTail, 0x1004, 0xffff, 1, 0, 1)")
expect(win32k64to32.inc "WOW64_SYSCALL(NtDebugOnly, 0x1003, 0x1003, 1, 1, 4)")
file(APPEND "${TEST_DIR}/32.h" "SVC_(Alpha, 2)\n")
run_generator(fail)
file(WRITE "${TEST_DIR}/32.h" "SVC_(Bad, nope)\n")
run_generator(fail)
file(WRITE "${TEST_DIR}/32.h" "#ifdef UNKNOWN\nSVC_(Conditional, 1)\n#endif\n")
run_generator(fail)
file(WRITE "${TEST_DIR}/32.h" "SVC_(TooMany, 32)\n")
run_generator(fail)
file(WRITE "${TEST_DIR}/32.h" "")
run_generator(fail)

# The prototype reader needs the host tool
if(NOT DEFINED WOW64PROTOS)
    message(STATUS "WoW64 syscall generator tests passed (no prototype tool given)")
    return()
endif()

file(WRITE "${TEST_DIR}/types.txt" [=[
# fixture types
DWORD int 4 4
LONG int 4 4
unsigned_long int 4 4
ULONGLONG int 8 8
HWND handle 4 8
HANDLE handle 4 8
HMONITOR handle 4 8
NTSTATUS int 4 4
BOOL int 4 4
PVOID pointer 4 8
PULONG pointer 4 8
SIZE_T ptrint 4 8
POINT struct 8 8
RECT struct 16 16
BLENDFUNCTION struct 4 4
__int64 int 8 8
unsigned___int64 int 8 8
_Bool int 1 1
]=])
file(WRITE "${TEST_DIR}/protos.h" [=[
#ifndef FIXTURE_H
#define FIXTURE_H
#define VARIANT_B
/* NtCommented(DWORD a, DWORD b); */
BOOL NTAPI NtUserDrag(_In_ HWND hWnd, _In_ POINT pt);
HMONITOR NTAPI NtUserRect(_In_ RECT rc, _In_ DWORD dwFlags);
NTSTATUS NTAPI NtQuery(_In_ HANDLE Handle,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG_UNKNOWN Length,
    _When_(a != 0, _In_) _Out_opt_ PULONG ReturnLength OPTIONAL);
VOID NTAPI NtNoArgs(VOID);
VOID NTAPI NtEmpty();
NTSTATUS NTAPI NtUnnamed(HANDLE, unsigned long, const struct tagFOO *, PVOID Array[], ULONGLONG Big);
NTSTATUS NTAPI NtCallback(_In_ PVOID (NTAPI *Routine)(PVOID), _In_ DWORD x);
#ifdef VARIANT_A
NTSTATUS NTAPI NtVariant(HANDLE a);
#else
NTSTATUS NTAPI NtVariant(HANDLE a, HANDLE b);
#endif
#if defined(VARIANT_B) && (WINVER >= 0x502)
NTSTATUS NTAPI NtDecided(HANDLE a);
#endif
#ifdef NEVER_DEFINED
NTSTATUS NTAPI NtInactive(HANDLE a);
#endif
#if SOMETHING_UNKNOWN > 3
NTSTATUS NTAPI NtMaybe(HANDLE a);
#endif
#ifdef _WIN64
NTSTATUS NTAPI NtWide(HANDLE a);
#endif
NTSTATUS NTAPI NtMultiline(
    _In_ DWORD a,
    _In_ DWORD b); // trailing
NTSTATUS NTAPI NtBlend(BLENDFUNCTION bf, LONG x);
NTSTATUS NTAPI NtCondParam(
#if 0
    POINT pt,
#endif
    LONG x);
NTSTATUS NTAPI NtInt64(__int64, unsigned __int64 u);
NTSTATUS NTAPI NtGroup(POINT (pt));
#ifdef _WIN64
#define HAS_CALL 1
#endif
#ifdef HAS_CALL
NTSTATUS NTAPI NtHasCall(HANDLE a);
#endif
#if 2 == 1 < 1
NTSTATUS NTAPI NtPrecedenceFalse(HANDLE a);
#endif
#define SCALE 1 + 2
#if SCALE * 3 == 7
NTSTATUS NTAPI NtScaleTrue(HANDLE a);
#endif
#if 0 && ((-9223372036854775807 - 1) / -1)
NTSTATUS NTAPI NtShortCircuit(HANDLE a);
#endif
#if 0x80000000 == 0x7fffffff
NTSTATUS NTAPI NtHexWidth(HANDLE a);
#endif
NTSTATUS NTAPI NtParamUnknown(
#ifdef _WIN64
    POINT pt,
#else
    LONG pt,
#endif
    LONG x);
#endif
]=])
string(REPEAT "_In_ " 300 many_annotations)
string(REPEAT "L" 600 long_name)
file(APPEND "${TEST_DIR}/protos.h" "NTSTATUS NTAPI NtManyAnnotations(${many_annotations} POINT p);\n")
file(APPEND "${TEST_DIR}/protos.h" "NTSTATUS NTAPI NtLongType(${long_name} ${long_name} x);\n")
string(REPEAT "A" 127 macro_name)
file(APPEND "${TEST_DIR}/protos.h" [=[
#if -1 < 1U
NTSTATUS NTAPI NtUnsigned(LONG x);
#else
NTSTATUS NTAPI NtUnsigned(POINT p);
#endif
#ifdef OUTER
NTSTATUS NTAPI NtNested(
#ifdef INNER
    POINT p,
#endif
    LONG x);
#endif
NTSTATUS NTAPI NtClosing(LONG x
#ifdef _WIN64
);
#else
, POINT p);
#endif
NTSTATUS NTAPI NtBool(_Bool);
#if (1 ? -1 : 0U) < 0
NTSTATUS NTAPI NtTernary(LONG x);
#else
NTSTATUS NTAPI NtTernary(POINT p);
#endif
/\
* #define HAS_SPLIT 1 */
#ifdef HAS_SPLIT
NTSTATUS NTAPI NtSplitComment(POINT p);
#else
NTSTATUS NTAPI NtSplitComment(LONG x);
#endif
// Disabled definition \
#define HAS_POINT 1
#ifdef HAS_POINT
NTSTATUS NTAPI NtSplice(POINT p);
#else
NTSTATUS NTAPI NtSplice(LONG x);
#endif
]=])
file(APPEND "${TEST_DIR}/protos.h" "#define ${macro_name} 1\n#ifdef ${macro_name}B\nNTSTATUS NTAPI NtLongMacro(LONG x);\n#else\nNTSTATUS NTAPI NtLongMacro(POINT p);\n#endif\n")
file(WRITE "${TEST_DIR}/proto-calls.txt" [=[
NtUserDrag
NtUserRect
NtQuery
NtNoArgs
NtEmpty
NtUnnamed
NtCallback
NtVariant
NtDecided
NtInactive
NtMaybe
NtWide
NtMultiline
NtBlend
NtCondParam
NtInt64
NtGroup
NtHasCall
NtPrecedenceFalse
NtScaleTrue
NtShortCircuit
NtHexWidth
NtParamUnknown
NtManyAnnotations
NtLongType
NtUnsigned
NtNested
NtClosing
NtBool
NtTernary
NtSplitComment
NtSplice
NtLongMacro
NtAbsent
NtCommented
]=])
execute_process(COMMAND "${WOW64PROTOS}" -DWINVER=0x502 -U_WIN64 -UOUTER -UINNER
    -t "${TEST_DIR}/types.txt" -c "${TEST_DIR}/proto-calls.txt" -o "${TEST_DIR}/protos.csv" "${TEST_DIR}/protos.h"
    RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "wow64protos failed: ${error}")
endif()
string(FIND "${error}" "unresolved type ULONG_UNKNOWN" found)
if(found EQUAL -1)
    message(FATAL_ERROR "wow64protos did not report the unknown type: ${error}")
endif()
set(protos "${TEST_DIR}/protos.csv")
expect_in("${protos}" "P,NtUserDrag,protos.h:5,2,1,-\n")
expect_in("${protos}" "A,NtUserDrag,0,hWnd,HWND,handle,4,8,1,1,-\n")
expect_in("${protos}" "A,NtUserDrag,1,pt,POINT,struct,8,8,2,1,reg\n")
expect_in("${protos}" "A,NtUserRect,0,rc,RECT,struct,16,16,4,1,ref\n")
expect_in("${protos}" "P,NtQuery,protos.h:7,4,1,-\n")
expect_in("${protos}" "A,NtQuery,1,Buffer,PVOID,pointer,4,8,1,1,-\n")
expect_in("${protos}" "A,NtQuery,2,Length,ULONG_UNKNOWN,unresolved,-tbd-,-tbd-,-tbd-,-tbd-,-\n")
expect_in("${protos}" "A,NtQuery,3,ReturnLength,PULONG,pointer,4,8,1,1,-\n")
expect_in("${protos}" "P,NtNoArgs,protos.h:11,0,1,-\n")
expect_in("${protos}" "P,NtEmpty,protos.h:12,0,1,-\n")
expect_in("${protos}" "A,NtUnnamed,0,-,HANDLE,handle,4,8,1,1,-\n")
expect_in("${protos}" "A,NtUnnamed,1,-,unsigned_long,int,4,4,1,1,-\n")
expect_in("${protos}" "A,NtUnnamed,2,-,tagFOO*,pointer,4,8,1,1,-\n")
expect_in("${protos}" "A,NtUnnamed,3,Array,PVOID*,pointer,4,8,1,1,-\n")
expect_in("${protos}" "A,NtUnnamed,4,Big,ULONGLONG,int64,8,8,2,1,-\n")
expect_in("${protos}" "A,NtCallback,0,Routine,function*,pointer,4,8,1,1,-\n")
expect_in("${protos}" "A,NtCallback,1,x,DWORD,int,4,4,1,1,-\n")
expect_in("${protos}" "P,NtVariant,protos.h:18,2,1,-\n")
expect_in("${protos}" "P,NtDecided,protos.h:21,1,1,-\n")
expect_in("${protos}" "M,NtInactive,inactive: defined(NEVER_DEFINED)\n")
expect_in("${protos}" "P,NtMaybe,protos.h:27,1,1,SOMETHING_UNKNOWN > 3\n")
expect_in("${protos}" "P,NtWide,protos.h:30,1,1,defined(_WIN64)\n")
expect_in("${protos}" "P,NtMultiline,protos.h:32,2,1,-\n")
expect_in("${protos}" "A,NtBlend,0,bf,BLENDFUNCTION,struct,4,4,1,1,reg\n")
expect_in("${protos}" "P,NtCondParam,protos.h:36,1,1,-\n")
expect_in("${protos}" "A,NtCondParam,0,x,LONG,int,4,4,1,1,-\n")
expect_in("${protos}" "A,NtInt64,0,-,__int64,int64,8,8,2,1,-\n")
expect_in("${protos}" "A,NtInt64,1,u,unsigned___int64,int64,8,8,2,1,-\n")
expect_in("${protos}" "A,NtGroup,0,-,-tbd-,unresolved,-tbd-,-tbd-,-tbd-,-tbd-,-\n")
expect_in("${protos}" "P,NtHasCall,protos.h:47,1,1,defined(HAS_CALL)\n")
expect_in("${protos}" "M,NtPrecedenceFalse,inactive: 2 == 1 < 1\n")
expect_in("${protos}" "P,NtScaleTrue,protos.h:54,1,1,-\n")
expect_in("${protos}" "M,NtShortCircuit,inactive: 0 && ((-9223372036854775807 - 1) / -1)\n")
expect_in("${protos}" "M,NtHexWidth,inactive: 0x80000000 == 0x7fffffff\n")
expect_in("${protos}" "P,NtParamUnknown,protos.h:62,3,2,-\n")
expect_in("${protos}" "A,NtManyAnnotations,0,p,POINT,struct,8,8,2,1,reg\n")
expect_in("${protos}" "A,NtLongType,0,x,${long_name}_${long_name},unresolved,")
expect_in("${protos}" "P,NtUnsigned,protos.h:73,1,2,-1 < 1U\n")
expect_in("${protos}" "P,NtNested,protos.h:78,2,2,defined(OUTER)\n")
expect_in("${protos}" "P,NtClosing,protos.h:84,1,2,-\n")
expect_in("${protos}" "A,NtBool,0,-,_Bool,int,1,1,1,1,-\n")
expect_in("${protos}" "P,NtTernary,protos.h:92,1,2,(1 ? -1 : 0U) < 0\n")
expect_in("${protos}" "P,NtSplitComment,protos.h:101,1,1,-\n")
expect_in("${protos}" "A,NtSplitComment,0,x,LONG,int,4,4,1,1,-\n")
expect_in("${protos}" "A,NtSplice,0,x,LONG,int,4,4,1,1,-\n")
expect_in("${protos}" "A,NtLongMacro,0,p,POINT,struct,8,8,2,1,reg\n")
expect_in("${protos}" "M,NtAbsent,no prototype\n")
expect_in("${protos}" "M,NtCommented,no prototype\n")
file(SHA256 "${protos}" first_hash)
execute_process(COMMAND "${WOW64PROTOS}" -DWINVER=0x502 -U_WIN64 -UOUTER -UINNER
    -t "${TEST_DIR}/types.txt" -c "${TEST_DIR}/proto-calls.txt" -o "${TEST_DIR}/protos.csv" "${TEST_DIR}/protos.h"
    RESULT_VARIABLE result ERROR_QUIET)
file(SHA256 "${protos}" second_hash)
if(NOT first_hash STREQUAL second_hash)
    message(FATAL_ERROR "wow64protos output is not deterministic")
endif()
execute_process(COMMAND "${WOW64PROTOS}" -DVARIANT_A -t "${TEST_DIR}/types.txt" -c "${TEST_DIR}/proto-calls.txt"
    -o "${TEST_DIR}/protos-a.csv" "${TEST_DIR}/protos.h" RESULT_VARIABLE result ERROR_QUIET)
expect_in("${TEST_DIR}/protos-a.csv" "P,NtVariant,protos.h:16,1,1,-\n")
execute_process(COMMAND "${WOW64PROTOS}" -t "${TEST_DIR}/types.txt" -c "${TEST_DIR}/proto-calls.txt"
    -o "${TEST_DIR}/protos-none.csv" "${TEST_DIR}/missing.h" RESULT_VARIABLE result ERROR_QUIET)
if(result EQUAL 0)
    message(FATAL_ERROR "wow64protos accepted a missing header")
endif()

# Tables and prototypes together: the by-value cases and the check
file(WRITE "${TEST_DIR}/32.h" "SVC_(UserDrag, 3)\nSVC_(UserRect, 5)\nSVC_(Blend, 2)\nSVC_(Variant, 2)\nSVC_(Multiline, 2)\n")
file(WRITE "${TEST_DIR}/64.h" "SVC_(UserDrag, 3)\nSVC_(UserRect, 2)\nSVC_(Blend, 2)\nSVC_(Variant, 2)\nSVC_(Multiline, 2)\n")
file(WRITE "${TEST_DIR}/nt.h" "SVC_(Query, 4)\nSVC_(Unnamed, 6)\nSVC_(Absent, 1)\n")
file(WRITE "${TEST_DIR}/catalog.tsv" "name\ti386_args\tamd64_args\targs\tclass\tstructs\nNtUserDrag\t3\t3\tx\tC\t-\nNtUserRect\t5\t2\tx\tC+S\t-\n")
file(APPEND "${TEST_DIR}/64.h" "SVC_(ParamUnknown, 3)\n#if DBG\nSVC_(DbgOnly64, 1)\n#endif\n")
file(APPEND "${TEST_DIR}/32.h" "SVC_(ParamUnknown, 3)\n")
run_generator(pass "-DPROTOTYPES=${TEST_DIR}/protos.csv" "-DCATALOG=${TEST_DIR}/catalog.tsv" -DDBG=ON)
expect(win32k32to64.inc "WOW64_SYSCALL(NtUserDrag, 0x1000, 0x1000, 3, 3, 40)")
expect(win32k32to64.inc "WOW64_SYSCALL(NtUserRect, 0x1001, 0x1001, 5, 2, 26)")
expect(win32k32to64.inc "WOW64_SYSCALL(NtBlend, 0x1002, 0x1002, 2, 2, 8)")
expect(win32k32to64.inc "WOW64_SYSCALL(NtVariant, 0x1003, 0x1003, 2, 2, 0)")
expect(win32k64to32.inc "WOW64_SYSCALL(NtUserRect, 0x1001, 0x1001, 2, 5, 26)")
expect(nt32to64.inc "WOW64_SYSCALL(NtQuery, 0x0, 0x0, 4, 4, 64)")
expect(nt32to64.inc "WOW64_SYSCALL(NtAbsent, 0x2, 0x2, 1, 1, 64)")
expect(calls.csv "NtUserDrag,win32k,0x1000,0x1000,3,3,3,2,2,1,-,C,protos.h:5,table-mismatch\n")
expect(calls.csv "NtUserRect,win32k,0x1001,0x1001,5,2,5,2,2,1,-,C+S,protos.h:6,by-value\n")
expect(calls.csv "NtBlend,win32k,0x1002,0x1002,2,2,2,2,2,1,-,-,protos.h:35,by-value\n")
expect(calls.csv "NtQuery,nt,0x0,0x0,4,4,-,-,4,0,-,-,protos.h:7,unresolved\n")
expect(calls.csv "NtAbsent,nt,0x2,0x2,1,1,-,-,-,-,-,-,no prototype,no-prototype\n")
expect(calls.csv "NtParamUnknown,win32k,0x1005,0x1005,3,3,-,-,3,1,-,-,protos.h:62,ambiguous\n")
expect(calls.csv "NtDbgOnly64,win32k,-,0x1006,-,1,-,-,-,-,DBG,-,-,no-prototype\n")
expect(args.csv "NtUserDrag,win32k,1,pt,POINT,struct,8,8,2,1,reg,C\n")
expect(args.csv "NtUserRect,win32k,0,rc,RECT,struct,16,16,4,1,ref,C+S\n")
expect(args.inc "WOW64_ARG(NtUserDrag, 1, \"pt\", \"POINT\", WOW64_ARG_STRUCT, 8, 8, 2, 1)")
expect(args.inc "WOW64_ARG(NtQuery, 2, \"Length\", \"ULONG_UNKNOWN\", WOW64_ARG_UNRESOLVED, 0, 0, 0, 0)")
string(FIND "${generator_output}" "NtUserDrag: the amd64 table says 3 slots, the prototype (protos.h:5) needs 2" found)
if(found EQUAL -1)
    message(FATAL_ERROR "The wrong amd64 count of NtUserDrag was not reported: ${generator_output}")
endif()
string(FIND "${generator_output}" "POINT pt is passed by value and takes 2 slots on i386 and 1 on amd64" found)
if(found EQUAL -1)
    message(FATAL_ERROR "The by-value argument was not named: ${generator_output}")
endif()
run_generator(fail "-DPROTOTYPES=${TEST_DIR}/protos.csv" -DCHECK=ON)
# With the amd64 count fixed the check passes; a wrong i386 count fails it
file(WRITE "${TEST_DIR}/64.h" "SVC_(UserDrag, 2)\nSVC_(UserRect, 2)\nSVC_(Blend, 2)\nSVC_(Variant, 2)\nSVC_(Multiline, 2)\n")
run_generator(pass "-DPROTOTYPES=${TEST_DIR}/protos.csv" -DCHECK=ON)
expect(win32k32to64.inc "WOW64_SYSCALL(NtUserDrag, 0x1000, 0x1000, 3, 2, 26)")
file(WRITE "${TEST_DIR}/32.h" "SVC_(UserDrag, 2)\nSVC_(UserRect, 5)\nSVC_(Blend, 2)\nSVC_(Variant, 2)\nSVC_(Multiline, 2)\n")
run_generator(fail "-DPROTOTYPES=${TEST_DIR}/protos.csv" -DCHECK=ON)
string(FIND "${generator_output}" "NtUserDrag: the i386 table says 2 slots, the prototype (protos.h:5) needs 3" found)
if(found EQUAL -1)
    message(FATAL_ERROR "The wrong i386 count was not reported: ${generator_output}")
endif()
message(STATUS "WoW64 syscall generator tests passed")
