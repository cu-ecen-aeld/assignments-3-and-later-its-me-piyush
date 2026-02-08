#!/bin/sh
# Tester script for assignment 1 and assignment 2
# Author: Siddhant Jajoo

# Finder-test script for Buildroot/QEMU image (Assignment 4)
# Requirements:
# - runnable from any directory (no ./ relative calls)
# - assumes configs at /etc/finder-app/conf
# - assumes executables/scripts are in PATH
# - writes finder output to /tmp/assignment4-result.txt

set -e
set -u

CONF_DIR="/etc/finder-app/conf"

NUMFILES=10
WRITESTR="AELD_IS_FUN"
BASE_WRITEDIR="/tmp/aeld-data"

# Read username from config location required by assignment
username="$(cat "${CONF_DIR}/assignment.txt")"

# Optional args:
#   0 args: defaults
#   1 arg : NUMFILES
#   2 args: NUMFILES WRITESTR
#   3 args: NUMFILES WRITESTR SUBDIRNAME (creates /tmp/aeld-data/SUBDIRNAME)
if [ $# -ge 1 ]; then
  NUMFILES="$1"
fi
if [ $# -ge 2 ]; then
  WRITESTR="$2"
fi

WRITEDIR="${BASE_WRITEDIR}"
if [ $# -ge 3 ]; then
  WRITEDIR="${BASE_WRITEDIR}/$3"
fi

MATCHSTR="The number of files are ${NUMFILES} and the number of matching lines are ${NUMFILES}"

echo "Writing ${NUMFILES} files containing string ${WRITESTR} to ${WRITEDIR}"

# Clean and recreate WRITEDIR
rm -rf "${BASE_WRITEDIR}"
mkdir -p "${WRITEDIR}"

# Use writer from PATH (cross-compiled binary in /usr/bin)
i=1
while [ "$i" -le "$NUMFILES" ]; do
  writer "${WRITEDIR}/${username}${i}.txt" "${WRITESTR}"
  i=$((i + 1))
done

# Run finder.sh from PATH and capture output
OUTPUTSTRING="$(finder.sh "${WRITEDIR}" "${WRITESTR}")"

# Write output to required file
echo "${OUTPUTSTRING}" > /tmp/assignment4-result.txt

# Cleanup test data directory (leave result file)
rm -rf "${BASE_WRITEDIR}"

# Validate output
echo "${OUTPUTSTRING}" | grep -F "${MATCHSTR}" >/dev/null 2>&1
echo "success"
exit 0
