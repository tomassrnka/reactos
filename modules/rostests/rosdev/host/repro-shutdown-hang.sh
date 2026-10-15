#!/bin/bash
# repro-shutdown-hang.sh VM ROSDEV_HUNGWIN.EXE [hung|churn] [forced|plain] [SECONDS]
#
# Reproducers for a reboot of a test VM that never ends or takes too long. A misbehaving GUI program (the
# rosdev_hungwin target of this directory's parent) runs when the reboot is requested:
#   hung     a window whose thread never answers messages
#   churn    a window of a process whose threads keep ending and starting
# and the reboot is requested either as `rosdev reboot` (forced: EWX_REBOOT | EWX_FORCE) or as
# `shutdown -r -t 0` in the guest (plain: not forced).
#   - forced + hung:  the force flag has to end the program at once. If winlogon drops EWX_FORCE from the
#     logoff that starts every shutdown, the session manager asks the program first and waits out its
#     timeouts (about a minute with the default settings).
#   - forced or plain + churn: if the thread list walk of the session manager is not safe against threads that
#     end meanwhile, csrss gets an access violation in CsrRemoveThread, keeps the process lock, and the
#     guest stays up for good (winlogon waits for csrss for ever).
# Prints RESTARTED or HUNG; the exit status is 0 only for RESTARTED. Needs a VM that `rosdev up` made.
set -u
vm=$1
exe=$2
kind=${3:-churn}
how=${4:-forced}
limit=${5:-150}
R=${ROSDEV:-rosdev}

count() { $R run "$vm" 'type C:\rosdev\rosdev.log | find /c "started"' --timeout 20 2>/dev/null | tail -n 1 | tr -d '\r '; }

$R push "$vm" "$exe" 'C:\rosdev\hungwin\' > /dev/null || exit 2
before=$(count)
[ -n "$before" ] || { echo "the guest does not answer"; exit 2; }
if [ "$kind" = churn ]; then
    $R run "$vm" 'C:\rosdev\hungwin\rosdev_hungwin.exe churn' --detach > /dev/null || exit 2
else
    $R run "$vm" 'C:\rosdev\hungwin\rosdev_hungwin.exe' --detach > /dev/null || exit 2
fi
sleep 5
start=$(date +%s)
if [ "$how" = plain ]; then
    $R run "$vm" 'shutdown -r -t 0' --detach > /dev/null || exit 2
else
    $R reboot "$vm" > /dev/null || exit 2
fi
while [ $(( $(date +%s) - start )) -lt "$limit" ]; do
    sleep 4
    now=$(count)
    if [ -n "$now" ] && [ "$now" != "$before" ]; then
        echo "RESTARTED after $(( $(date +%s) - start )) s"
        exit 0
    fi
done
echo "HUNG: the guest did not restart within $limit s"
exit 1
