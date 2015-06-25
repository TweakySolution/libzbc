#!/bin/bash

. ../zbc_test_common_functions.sh

if [ $# -ne 2 -a $# -ne 3 ]; then
  echo "[usage] $0 <target_device> <test_bin_path> [test_log_path]"
  echo "    target_device          : device file. e.g. /dev/sg3"
  echo "    test_bin_path          : binary directory"
  echo "    test_log_path          : [option] output log directory."
  echo "                                      If this option isn't specified, use current directory."
  exit 1
fi

# Store argument
device=${1}
bin_path=${2}

if [ $# -eq 3 ]; then
    log_path=${3}
else
    log_path=`pwd`
fi

# Extract testname
testbase=${0##*/}
testname=${testbase%.*}

# Set file names
log_file="${log_path}/${testname}.log"
zone_info_file="/tmp/{testname}_zone_info.log"

# Delete old log file
rm -f ${log_file}
rm -f ${zone_info_file}

# Set expected error code
expected_sk="Illegal-request"
expected_asc="Logical-block-address-out-of-range"

# Test print
echo -n "    ${testname}: READ attempt to read invalid data test (reading over the last LBA)... "

# Get drive information
zbc_test_get_drive_info

# Start testing
sudo ${bin_path}/zbc_test_read_zone -v ${device} ${max_lba} 2 >> ${log_file} 2>&1

# Check result
zbc_test_get_sk_ascq
zbc_test_check_sk_ascq

# Post process
rm -f ${zone_info_file}
