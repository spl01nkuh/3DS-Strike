#!/usr/bin/env bash
# Build the 3DS port and summarize errors.
cd /c/dev/sf3/3s-3ds || exit 1

if [ ! -d build ]; then
    cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" || exit 1
fi

cmake --build build -- -k 0 > build1.log 2>&1
status=$?

echo "=== build exit: $status ==="
echo "=== error count ==="
grep -c 'error:' build1.log
echo "=== top error categories ==="
grep -oE 'error: .*' build1.log | cut -c1-110 | sort | uniq -c | sort -rn | head -30
echo "=== first errors ==="
grep -nE 'error: ' build1.log | head -10
