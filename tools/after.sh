#!/usr/bin/env bash
# Creates a file after so many seconds: tools/after.sh <seconds> <file>
end=$(( $(date +%s) + $1 ))
while [ "$(date +%s)" -lt "$end" ]; do
  sleep 1
done
: > "$2"
