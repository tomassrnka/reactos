
list(APPEND HAL_APIC_ASM_SOURCE
    apic/apictrap.S
    apic/tsccal.S)

list(APPEND HAL_APIC_SOURCE
    apic/apic.c
    apic/apictimer.c
    apic/halinit.c
    apic/processor.c
    apic/rtctimer.c
    apic/tsc.c)

add_asm_files(lib_hal_apic_asm ${HAL_APIC_ASM_SOURCE})
add_library(lib_hal_apic OBJECT ${HAL_APIC_SOURCE} ${lib_hal_apic_asm})
add_dependencies(lib_hal_apic asm)

# The same component for SMP HALs: per-processor state must be reached through FS
add_asm_files(lib_hal_apic_smp_asm ${HAL_APIC_ASM_SOURCE})
add_library(lib_hal_apic_smp OBJECT ${HAL_APIC_SOURCE} ${lib_hal_apic_smp_asm})
add_dependencies(lib_hal_apic_smp asm)
target_compile_definitions(lib_hal_apic_smp PRIVATE CONFIG_SMP)
