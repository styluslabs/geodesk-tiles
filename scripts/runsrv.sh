#!/usr/bin/env bash
# Run a command as service - restarting upon failure (unless failure occurs less than 60 seconds after start)
# since Ctrl+C sends SIGINT to all parents of foreground process as well, it will terminate script as desired

# enable core dumps; to open: `gdb <exe> <core dump>`
#sudo sysctl -w kernel.core_pattern=/tmp/cores/core.%e.%p.%t
ulimit -c unlimited

# print file info for executable (helpful when updating and relaunching)
ls -la "$1"
tstart=$SECONDS
until "$@"; do
  exitcode=$?
  tend=$SECONDS
  if (( tend - tstart < 60 )); then   #|| nfailures > MAX_FAILURES
    echo "$(date): command '$@' failed with exit code $exitcode within 60 seconds ... aborting" >&2
    #mutt -s "FAILURE: $@" server-status@styluslabs.com < /dev/null
    break
  fi
  echo "$(date): command '$@' failed with exit code $exitcode ... restarting in 5 seconds" >&2
  sleep 5
  tstart=$SECONDS
done
