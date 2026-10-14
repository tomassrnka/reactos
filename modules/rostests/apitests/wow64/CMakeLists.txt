# Console-only selections of the existing API tests. Compile the original
# sources without changing their assertions; use identical PE32 executables
# for the native i386 and WoW64 comparison. The full suites also import GUI and
# network libraries, whose WoW64 support is a separate gate.
function(add_wow64_apitest library)
    set(sources)
    set(testlist "#define STANDALONE\n#include <apitest.h>\n")
    foreach(test ${ARGN})
        if(NOT test MATCHES "^(RegWow64(Unicode|Views|Matrix|LongName|SharedLongName|Volatile|SharedCreate|Aliases)|Wow64ProgramFiles|Wow64EnvironmentBlock)$")
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
    RegCreateKeyEx RegOpenKeyExW RegQueryValueExW RegEnumValueW RegWow64 RegWow64Unicode RegWow64Views RegWow64SharedCreate RegWow64Matrix RegWow64LongName RegWow64SharedLongName RegWow64Volatile RegWow64Aliases)

target_sources(advapi32_wow64_apitest PRIVATE ../advapi32/RegWow64Review.c)

if(ARCH STREQUAL "i386")
    # Explicit smoke-test target; do not change default image contents.
    add_executable(wow64_gui_load EXCLUDE_FROM_ALL gui_load.c)
    set_module_type(wow64_gui_load win32cui)
    add_importlibs(wow64_gui_load msvcrt kernel32)

    add_asm_files(wow64_continue_asm ../ntdll/i386/Wow64Continue.S)
    target_sources(ntdll_wow64_apitest PRIVATE ${wow64_continue_asm})
elseif(ARCH STREQUAL "amd64")
    add_asm_files(wow64_continue_asm ../ntdll/amd64/Wow64Continue.S)
    target_sources(ntdll_wow64_apitest PRIVATE ${wow64_continue_asm})
endif()

# Explicit, uninstalled unit fixture for wide guest readers. It compiles the
# production accessors and selected DLL sources, and uses the test-only native
# bridge until guest GUI syscall/callback dispatch is available.
if(ARCH STREQUAL "i386" AND NOT MSVC)
    add_asm_files(gueststate_asm i386/ClientPfn.S i386/GuestState.S)
    add_executable(wow64_guest_state EXCLUDE_FROM_ALL
        GuestState.c GuestStateUser.c GuestStateGdi.c GuestStateImm.c
        gueststate_testlist.c ${gueststate_asm})
    target_compile_definitions(wow64_guest_state PRIVATE W32K_WIDE)
    target_compile_options(wow64_guest_state PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(wow64_guest_state PRIVATE -Wl,--gc-sections)
    target_include_directories(wow64_guest_state PRIVATE
        ${REACTOS_SOURCE_DIR}/win32ss/include
        ${REACTOS_SOURCE_DIR}/win32ss/user/user32/include
        ${REACTOS_SOURCE_DIR}/win32ss/gdi/gdi32/include
        ${REACTOS_SOURCE_DIR}/sdk/include/reactos/subsys
        ${REACTOS_SOURCE_DIR}/sdk/include/wine)
    target_link_libraries(wow64_guest_state wine ${PSEH_LIB})
    set_module_type(wow64_guest_state win32cui)
    add_importlibs(wow64_guest_state msvcrt kernel32 ntdll)
    add_dependencies(wow64_guest_state wow64_syscall_tables)
endif()

# The native syscall bridge is deliberately test-only, and uses GNU .code64
# in a PE32 fixture until wow64win provides the guest GUI entry points.
if(ARCH STREQUAL "amd64" OR (ARCH STREQUAL "i386" AND NOT MSVC))
    if(ARCH STREQUAL "i386")
        add_asm_files(clientpfn_asm i386/ClientPfn.S)
    endif()
    add_executable(win32k_wow64_apitest ClientPfn.c clientpfn_testlist.c ${clientpfn_asm})
    target_include_directories(win32k_wow64_apitest PRIVATE ${REACTOS_SOURCE_DIR}/win32ss/include)
    target_link_libraries(win32k_wow64_apitest wine ${PSEH_LIB})
    set_module_type(win32k_wow64_apitest win32cui)
    add_importlibs(win32k_wow64_apitest msvcrt kernel32 ntdll)
    add_dependencies(win32k_wow64_apitest wow64_syscall_tables)
    add_rostests_file(TARGET win32k_wow64_apitest)
endif()
