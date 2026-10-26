# Host-native handle-table test harness (compiles the REAL ntoskrnl/ex/handle.c).
# Usage: make -C tools/hosttest/handle run [SECS=2]     (host gcc, x86_64 Linux)
ROOT := ../../..
CFLAGS ?= -std=gnu11 -O2 -g -pthread -w -Ishim
SECS ?= 2

handle_hosttest: handle_hosttest.c $(ROOT)/ntoskrnl/ex/handle.c shim/ntoskrnl.h shim/debug.h
	$(CC) $(CFLAGS) -o $@ handle_hosttest.c $(ROOT)/ntoskrnl/ex/handle.c

run: handle_hosttest
	./handle_hosttest $(SECS)

# Self-test of the harness: disable the ABA-diverting free-list push in a COPY
# of handle.c; the stress run must then FAIL (assert or ownership violation).
selftest-mutation:
	sed 's/Free = (HandleTable->HandleTableLock\[LockIndex\].Locked) ?/Free = (0) ?/' $(ROOT)/ntoskrnl/ex/handle.c > mutated_handle.c
	grep -q "Free = (0) ?" mutated_handle.c
	$(CC) $(CFLAGS) -o handle_hosttest_mut handle_hosttest.c mutated_handle.c
	! ./handle_hosttest_mut 2 >/dev/null 2>&1
	@echo "mutation detected (harness has teeth)"
	rm -f mutated_handle.c handle_hosttest_mut

clean:
	rm -f handle_hosttest handle_hosttest_mut mutated_handle.c
