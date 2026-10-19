/* Guest gate with native i386 fallback, for the identical-binary ABI test. */
#define SYSWOW64_GUEST 1
#include <syscalls.inc>
.code
SyscallId = HEX(1000)
#define SVC_(name, count) STUB_U name, count
#include <w32ksvc32.h>
END
