/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Guest interface of a Microsoft-compatible hypervisor
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 * REFERENCES:  Microsoft, "Hypervisor Top Level Functional Specification",
 *              version 6.0b (TLFS). Section numbers below refer to it.
 */

#pragma once

/* Hypervisor discovery (2.2): CPUID.01h:ECX bit 31 */
#define HV_CPUID_HYPERVISOR_PRESENT_BIT         31

/* Hypervisor CPUID leaves (2.3, 2.4) */
#define HV_CPUID_VENDOR_AND_MAX_FUNCTIONS       0x40000000
#define HV_CPUID_INTERFACE                      0x40000001
#define HV_CPUID_FEATURES                       0x40000003
#define HV_CPUID_ENLIGHTENMENT_INFO             0x40000004
#define HV_CPUID_IMPLEMENTATION_LIMITS          0x40000005

/* Interface signature "Hv#1" in CPUID 0x40000001 EAX */
#define HV_INTERFACE_SIGNATURE_HV1              0x31237648

/*
 * Partition privileges, CPUID 0x40000003 EAX (bits 31:0 of
 * HV_PARTITION_PRIVILEGE_MASK, 4.2.2)
 */
#define HV_ACCESS_PARTITION_REFERENCE_COUNTER   (1UL << 1)
#define HV_ACCESS_INTR_CTRL_REGS                (1UL << 4)
#define HV_ACCESS_HYPERCALL_MSRS                (1UL << 5)
#define HV_ACCESS_VP_INDEX                      (1UL << 6)
#define HV_ACCESS_PARTITION_REFERENCE_TSC       (1UL << 9)
#define HV_ACCESS_FREQUENCY_REGS                (1UL << 11)

/* Features, CPUID 0x40000003 EDX (2.4) */
#define HV_FEATURE_FREQUENCY_REGS               (1UL << 8)

/* Implementation recommendations, CPUID 0x40000004 EAX (2.4) */
#define HV_RECOMMEND_HYPERCALL_REMOTE_FLUSH     (1UL << 2)
#define HV_RECOMMEND_APIC_MSRS                  (1UL << 3)
#define HV_RECOMMEND_CLUSTER_IPI                (1UL << 10)
#define HV_RECOMMEND_EX_PROCESSOR_MASKS         (1UL << 11)

/* Synthetic MSRs (appendix C) */
#define HV_X64_MSR_GUEST_OS_ID                  0x40000000
#define HV_X64_MSR_HYPERCALL                    0x40000001
#define HV_X64_MSR_VP_INDEX                     0x40000002
#define HV_X64_MSR_TIME_REF_COUNT               0x40000020
#define HV_X64_MSR_REFERENCE_TSC                0x40000021
#define HV_X64_MSR_TSC_FREQUENCY                0x40000022
#define HV_X64_MSR_EOI                          0x40000070
#define HV_X64_MSR_ICR                          0x40000071
#define HV_X64_MSR_TPR                          0x40000072
#define HV_X64_MSR_VP_ASSIST_PAGE               0x40000073

/* Hypercall MSR (3.13), reference TSC MSR (12.7), VP assist page MSR (7.8.7.1) */
#define HV_X64_MSR_HYPERCALL_ENABLE             0x1ULL
#define HV_X64_MSR_HYPERCALL_LOCKED             0x2ULL
#define HV_X64_MSR_HYPERCALL_RESERVED           0xFFCULL
#define HV_X64_MSR_REFERENCE_TSC_ENABLE         0x1ULL
#define HV_X64_MSR_VP_ASSIST_PAGE_ENABLE        0x1ULL
#define HV_X64_MSR_PAGE_RESERVED_MASK           0xFFEULL

/*
 * Guest OS identity for an open source OS (2.6): bit 63 set. Bits 62:56
 * hold an OS type allocated by Microsoft; none is allocated to ReactOS, so
 * they stay zero.
 */
#define HV_GUEST_OS_ID_OPEN_SOURCE              (1ULL << 63)

/* Hypercall input value (3.7) */
#define HV_HYPERCALL_FAST                       (1ULL << 16)
#define HV_HYPERCALL_VARIABLE_HEADER_SHIFT      17
#define HV_HYPERCALL_REP_COUNT_SHIFT            32

/* Hypercall result value (3.8) */
#define HV_HYPERCALL_RESULT_MASK                0xFFFFULL

/* Call codes (appendix A) */
#define HvCallFlushVirtualAddressSpace          0x0002
#define HvCallFlushVirtualAddressList           0x0003
#define HvCallSendSyntheticClusterIpi           0x000B
#define HvCallFlushVirtualAddressSpaceEx        0x0013
#define HvCallFlushVirtualAddressListEx         0x0014
#define HvCallSendSyntheticClusterIpiEx         0x0015

/* Status codes (appendix B) */
#define HV_STATUS_SUCCESS                       0x0000

/* Flush flags (9.3, 9.4) */
#define HV_FLUSH_ALL_VIRTUAL_ADDRESS_SPACES     0x00000002ULL
#define HV_FLUSH_NON_GLOBAL_MAPPINGS_ONLY       0x00000004ULL

/* A GVA range element: the low 12 bits count the pages after the first (9.4, HvFlushVirtualAddressList) */
#define HV_GVA_RANGE_MAX_ADDITIONAL_PAGES       0xFFF

/* VP index (7.8) and processor sets (7.8.7.3, 7.8.7.4) */
#define HV_ANY_VP                               0xFFFFFFFFUL
#define HV_GENERIC_SET_SPARSE_4K                0
#define HV_VP_SET_BANK_SIZE                     64

/* Reference TSC page (12.7); a sequence of 0 means the page is not valid */
typedef struct _HV_REFERENCE_TSC_PAGE
{
    volatile ULONG TscSequence;
    ULONG Reserved1;
    volatile ULONG64 TscScale;
    volatile LONG64 TscOffset;
    ULONG64 Reserved2[509];
} HV_REFERENCE_TSC_PAGE, *PHV_REFERENCE_TSC_PAGE;

/* Reference time runs at 10 MHz (100 ns units, 12.4) */
#define HV_REFERENCE_TIME_FREQUENCY             10000000ULL
