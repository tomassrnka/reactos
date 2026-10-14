# Console-only selections of the existing API tests. Compile the original
# sources without changing their assertions; use identical PE32 executables
# for the native i386 and WoW64 comparison. The full suites also import GUI and
# network libraries, whose WoW64 support is a separate gate.
function(add_wow64_apitest library)
    set(sources)
    set(testlist "#define STANDALONE\n#include <apitest.h>\n")
    foreach(test ${ARGN})
        if(NOT test MATCHES "^Wow64(ProgramFiles|EnvironmentBlock)$")
            list(APPEND sources ../${library}/${test}.c)
        endif()
        string(APPEND testlist "extern void func_${test}(void);\n")
    endforeach()
    string(APPEND testlist "const struct test winetest_testlist[] = {\n")
    foreach(test ${ARGN})
        string(APPEND testlist "    {\"${test}\", func_${test}},\n")
    endforeach()
    string(APPEND testlist "    {0, 0}\n};\n")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${library}_testlist.c "${testlist}")
    set(target ${library}_wow64_apitest)
    add_executable(${target} ${sources} ${CMAKE_CURRENT_BINARY_DIR}/${library}_testlist.c)
    target_link_libraries(${target} wine ${PSEH_LIB})
    set_module_type(${target} win32cui)
    add_importlibs(${target} advapi32 msvcrt kernel32 ntdll)
    add_rostests_file(TARGET ${target})
endfunction()

add_wow64_apitest(kernel32
    GetCurrentDirectory GetEnvironmentVariable GetDriveType GetCPInfo
    IsDBCSLeadByteEx lstrlen Wow64Environment Wow64ProgramFiles Wow64EnvironmentBlock)
add_wow64_apitest(ntdll
    Wow64Continue NtOpenKey NtQueryKey NtQueryValueKey
    RtlDetermineDosPathNameType RtlUnicodeStringToAnsiString)
add_wow64_apitest(advapi32
    RegCreateKeyEx RegOpenKeyExW RegQueryValueExW RegEnumValueW)

if(ARCH STREQUAL "i386")
    add_asm_files(wow64_continue_asm ../ntdll/i386/Wow64Continue.S)
    target_sources(ntdll_wow64_apitest PRIVATE ${wow64_continue_asm})
elseif(ARCH STREQUAL "amd64")
    add_asm_files(wow64_continue_asm ../ntdll/amd64/Wow64Continue.S)
    target_sources(ntdll_wow64_apitest PRIVATE ${wow64_continue_asm})
endif()
