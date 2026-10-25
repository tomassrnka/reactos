/*
 * PROJECT:         ReactOS
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            include/psdk/ntverp.h
 * PURPOSE:         Master Version File.
 *                  This file should be modified only by the official builder
 *                  to update VERSION, VER_PRODUCTVERSION, VER_PRODUCTVERSION_
 *                  STR and VER_PRODUCTBETA_STR values.
 *                  The VER_PRODUCTBUILD lines must contain the product
 *                  comments and end with the build#<CR><LF>.
 *                  The VER_PRODUCTBETA_STR lines must contain the product
 *                  comments and end with "somestring"<CR><LF>.
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

//
// VERBUMP-01 single switch: system-reported NT version tier.
//   0   = stock ReactOS (NT 5.2.3790, Server 2003 SP2)  [baseline]
//   62  = Windows 8     (NT 6.2.9200)   -- clr.dll EEStartup floor (T118/T132)
//   100 = Windows 10 22H2 (NT 10.0.19045.5247) -- modern-app identity
// NOTE: boot/bootdata/*.inf (registry CurrentVersion block, CSDVersion) and
// sdk/include/reactos/version.rc (DLL FileVersion) are text, not C -- they are
// edited in this branch to MATCH VERBUMP_TIER and must be re-synced if the tier
// is changed. See VERBUMP-01-kickoff-findings.md.
//
#ifndef VERBUMP_TIER
#define VERBUMP_TIER 100
#endif

#if VERBUMP_TIER == 100
#define VER_PRODUCTBUILD                    19045
#define VER_PRODUCTBUILD_QFE                5247
#define VER_PRODUCTMAJORVERSION             10
#define VER_PRODUCTMINORVERSION             0
#define VER_PRODUCTVERSION_W                (0x0A00)
#define VER_PRODUCTVERSION_DW               (0x0A000000 | VER_PRODUCTBUILD)
#elif VERBUMP_TIER == 62
#define VER_PRODUCTBUILD                    9200
#define VER_PRODUCTBUILD_QFE                0
#define VER_PRODUCTMAJORVERSION             6
#define VER_PRODUCTMINORVERSION             2
#define VER_PRODUCTVERSION_W                (0x0602)
#define VER_PRODUCTVERSION_DW               (0x06020000 | VER_PRODUCTBUILD)
#else
#define VER_PRODUCTBUILD                    3790
#define VER_PRODUCTBUILD_QFE                3959
#define VER_PRODUCTMAJORVERSION             5
#define VER_PRODUCTMINORVERSION             2
#define VER_PRODUCTVERSION_W                (0x0502)
#define VER_PRODUCTVERSION_DW               (0x05020000 | VER_PRODUCTBUILD)
#endif

//
// Not a beta
//
#define VER_PRODUCTBETA_STR                 ""

//
// ANSI String Generating Macros
//
#define VER_PRODUCTVERSION_MAJORMINOR2(x,y) \
    #x "." #y
#define VER_PRODUCTVERSION_MAJORMINOR1(x,y) \
    VER_PRODUCTVERSION_MAJORMINOR2(x, y)
#define VER_PRODUCTVERSION_STRING           \
    VER_PRODUCTVERSION_MAJORMINOR1(VER_PRODUCTMAJORVERSION, VER_PRODUCTMINORVERSION)

//
// Unicode String Generating Macros
//
#define LVER_PRODUCTVERSION_MAJORMINOR2(x,y)\
    L#x L"." L#y
#define LVER_PRODUCTVERSION_MAJORMINOR1(x,y)\
    LVER_PRODUCTVERSION_MAJORMINOR2(x, y)
#define LVER_PRODUCTVERSION_STRING          \
    LVER_PRODUCTVERSION_MAJORMINOR1(VER_PRODUCTMAJORVERSION, VER_PRODUCTMINORVERSION)

//
// Full Product Version
//
#define VER_PRODUCTVERSION                  \
    VER_PRODUCTMAJORVERSION,VER_PRODUCTMINORVERSION,VER_PRODUCTBUILD,VER_PRODUCTBUILD_QFE

//
// Padding for ANSI Version String
//
#if     (VER_PRODUCTBUILD < 10)
#define VER_BPAD "000"
#elif   (VER_PRODUCTBUILD < 100)
#define VER_BPAD "00"
#elif   (VER_PRODUCTBUILD < 1000)
#define VER_BPAD "0"
#else
#define VER_BPAD
#endif

//
// Padding for Unicode Version String
//
#if     (VER_PRODUCTBUILD < 10)
#define LVER_BPAD L"000"
#elif   (VER_PRODUCTBUILD < 100)
#define LVER_BPAD L"00"
#elif   (VER_PRODUCTBUILD < 1000)
#define LVER_BPAD L"0"
#else
#define LVER_BPAD
#endif

//
// ANSI Product Version String
//
#define VER_PRODUCTVERSION_STR2(x,y)        \
    VER_PRODUCTVERSION_STRING "." VER_BPAD #x "." #y
#define VER_PRODUCTVERSION_STR1(x,y)        \
    VER_PRODUCTVERSION_STR2(x, y)
#define VER_PRODUCTVERSION_STR              \
    VER_PRODUCTVERSION_STR1(VER_PRODUCTBUILD, VER_PRODUCTBUILD_QFE)

//
// Unicode Product Version String
//
#define LVER_PRODUCTVERSION_STR2(x,y)       \
    LVER_PRODUCTVERSION_STRING L"." LVER_BPAD L#x L"." L#y
#define LVER_PRODUCTVERSION_STR1(x,y)       \
    LVER_PRODUCTVERSION_STR2(x, y)
#define LVER_PRODUCTVERSION_STR             \
    LVER_PRODUCTVERSION_STR1(VER_PRODUCTBUILD, VER_PRODUCTBUILD_QFE)

//
// Debug Flag
//
#if DBG
#define VER_DEBUG                           VS_FF_DEBUG
#else
#define VER_DEBUG                           0
#endif

//
// Beta Flag
//
#if BETA
#define VER_PRERELEASE                      VS_FF_PRERELEASE
#else
#define VER_PRERELEASE                      0
#endif

//
// Internal Flag
//
#if OFFICIAL_BUILD
#define VER_PRIVATE                         0
#else
#define VER_PRIVATE                         VS_FF_PRIVATEBUILD
#endif

//
// Other Flags
//
#define VER_FILEFLAGSMASK                   VS_FFI_FILEFLAGSMASK
#define VER_FILEOS                          VOS_NT_WINDOWS32
#define VER_FILEFLAGS                       (VER_PRERELEASE | \
                                             VER_DEBUG | \
                                             VER_PRIVATE)

//
// Company and Trademarks
//
#define VER_COMPANYNAME_STR                 \
    "ReactOS Project"
#define VER_PRODUCTNAME_STR                 \
    "ReactOS Operating System"
#define VER_LEGALTRADEMARKS_STR             \
    "ReactOS is a registered trademark of ReactOS Deutschland e.V."
