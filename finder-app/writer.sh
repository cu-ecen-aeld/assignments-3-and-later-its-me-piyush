#!/bin/sh
# writer.sh
# Usage: ./writer.sh <writefile> <writestr>
# @credits: ChatGPT 

writefile="$1"
writestr="$2"

# Check arguments
if [ -z "$writefile" ] || [ -z "$writestr" ]; then
  echo "ERROR: Missing required arguments."
  echo "Usage: $0 <writefile> <writestr>"
  exit 1
fi

# Create directory path if it does not exist
dirpath=$(dirname "$writefile")
mkdir -p "$dirpath" 2>/dev/null

# Write string to file (overwrite if exists)
echo "$writestr" > "$writefile" 2>/dev/null # Used ChatGPT to help with this line

# Check if file creation/write succeeded
if [ $? -ne 0 ]; then
  echo "ERROR: Could not create or write to file '$writefile'"
  exit 1
fi

exit 0
