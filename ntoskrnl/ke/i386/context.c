/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/i386/context.c
 * PURPOSE:         Context Switching Related Code
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* FUNCTIONS ******************************************************************/

VOID
NTAPI
KiSwapProcess(IN PKPROCESS NewProcess,
              IN PKPROCESS OldProcess)
{
    PKIPCR Pcr = (PKIPCR)KeGetPcr();
#ifdef CONFIG_SMP
    LONG SetMember = (LONG)Pcr->PrcbData.SetMember;

    /* Join the new process before its address space is loaded, so its flushes reach this processor */
    if (NewProcess != OldProcess)
        InterlockedOr((PLONG)&NewProcess->ActiveProcessors, SetMember);
#endif

    /* Check for new LDT */
    if (NewProcess->LdtDescriptor.LimitLow != OldProcess->LdtDescriptor.LimitLow)
    {
        if (NewProcess->LdtDescriptor.LimitLow)
        {
            KeSetGdtSelector(KGDT_LDT,
                             ((PULONG)&NewProcess->LdtDescriptor)[0],
                             ((PULONG)&NewProcess->LdtDescriptor)[1]);
            KiSetLdt((PKPCR)Pcr, KGDT_LDT);
        }
        else
        {
            KiSetLdt((PKPCR)Pcr, 0);
        }
    }

    /* Update CR3 */
    KiSetCr3((PKPCR)Pcr, NewProcess->DirectoryTableBase[0]);

#ifdef CONFIG_SMP
    /* Leave the old process once its address space is no longer loaded */
    if (NewProcess != OldProcess)
        InterlockedAnd((PLONG)&OldProcess->ActiveProcessors, ~SetMember);
#endif

    /* Clear GS */
    Ke386SetGs(0);

    /* Update IOPM offset */
    Pcr->TSS->IoMapBase = NewProcess->IopmOffset;
}

