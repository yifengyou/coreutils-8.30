#!/bin/sh
# Ensure "df --direct" works as documented

# Copyright (C) 2010 Free Software Foundation, Inc.

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

. "${srcdir=.}/init.sh"; path_prepend_ ../src
print_ver_ df

df || skip_ "df fails"

DIR=`pwd` || framework_failure
FILE="$DIR/file"
touch "$FILE" || framework_failure
echo "$FILE" > file_exp || framework_failure
echo "Mounted on" > header_mounted_exp || framework_failure
echo "File" > header_file_exp || framework_failure

fail=0

df --portability "$FILE" > df_out || fail=1
df --portability --direct "$FILE" > df_direct_out || fail=1
df --portability --direct --local "$FILE" > /dev/null 2>&1 && fail=1

# check df header
$AWK '{ if (NR==1) print $6 " " $7; }' df_out > header_mounted_out \
  || framework_failure
$AWK '{ if (NR==1) print $6; }' df_direct_out > header_file_out \
  || framework_failure
compare header_mounted_out header_mounted_exp || fail=1
compare header_file_out header_file_exp || fail=1

# check df output (without --direct)
$AWK '{ if (NR==2) print $6; }' df_out > file_out \
  || framework_failure
compare file_out file_exp && fail=1

# check df output (with --direct)
$AWK '{ if (NR==2) print $6; }' df_direct_out > file_out \
  || framework_failure
compare file_out file_exp || fail=1

Exit $fail
