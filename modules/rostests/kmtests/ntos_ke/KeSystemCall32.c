/*
 * PROJECT:     ReactOS kernel tests
 * LICENSE:     MIT
 * PURPOSE:     Compatibility-mode syscall setup
 */

#include <kmt_test.h>

START_TEST(KeSystemCall32)
{
#ifdef _M_AMD64
    ULONG64 Star = __readmsr(0xc0000081);
    ULONG64 Lstar = __readmsr(0xc0000082);
    ULONG64 Cstar = __readmsr(0xc0000083);
    ULONG64 Mask = __readmsr(0xc0000084);

    ok(Cstar != 0 && Cstar != Lstar, "Invalid compatibility entry: %I64x\n", Cstar);
    ok_eq_hex((Star >> 48) & 0xffff, 0x23);
    ok_eq_hex((Star >> 32) & 0xffff, 0x10);
    ok((Mask & 0x4700) == 0x4700, "SYSCALL must clear TF, IF, DF and NT: %I64x\n", Mask);
#else
    skip(FALSE, "CSTAR requires amd64\n");
#endif
}
