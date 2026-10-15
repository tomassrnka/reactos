/* Native service numbers always follow the actual amd64 table. */
#include <syscalls.inc>
.code
SyscallId = HEX(1000)
#define SVC_(name, count) STUB_U name, count
#include <w32ksvc64.h>
END
