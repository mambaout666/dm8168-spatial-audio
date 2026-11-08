#!/bin/sh
# Read-only inventory. Run in the SDK VM; set SDK_ROOT to the actual location.
set -u
printf '=== System ===\n'
uname -a
printf '\n=== Cross compiler ===\n'
if command -v arm-none-linux-gnueabi-gcc >/dev/null 2>&1; then
    arm-none-linux-gnueabi-gcc --version | head -n 1
else
    printf 'arm-none-linux-gnueabi-gcc is not on PATH\n'
fi
printf '\n=== Qt ===\n'
if command -v qmake >/dev/null 2>&1; then qmake -query QT_VERSION; else printf 'qmake is not on PATH\n'; fi
if [ -z "${SDK_ROOT:-}" ]; then
    printf '\nSet SDK_ROOT to the existing ti-ezsdk_dm816x-evm directory and rerun.\n'
    exit 1
fi
if [ ! -d "$SDK_ROOT" ]; then printf 'SDK_ROOT is not a directory\n' >&2; exit 1; fi
printf '\n=== Relevant SDK files (up to 80 matches) ===\n'
find "$SDK_ROOT" -type f \( -name 'Rpe.h' -o -name '*rpe*.h' -o -name 'test1.c' \
    -o -name 'sphdec1_copy.c' -o -name '*045a*' -o -name '*161a*' \
    -o -name 'running*' -o -name 'libRPE*' \) -print | head -n 80
printf '\nNo drivers, firmware, boot arguments or files have been changed.\n'
