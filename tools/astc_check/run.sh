#!/bin/bash
# Checks the BC -> ASTC 4x4 transcoder (runtime/gpu/vulkan/bc_decoder.cpp) against a real ASTC
# decoder: Apple's Metal on a Mac (Apple Silicon samples ASTC natively). It makes 65536 BC1 blocks
# and 65536 BC2/BC3 blocks, transcodes them, has Metal decode the ASTC, and compares with the BC
# reference decode (colour error in levels, alpha error, alpha mismatches).
#   usage: tools/astc_check/run.sh [work-dir]      (default /tmp/astc_check)
# Needs clang++, swiftc and the Android NDK (only for its vulkan.h; the NDK's include directory
# itself must NOT be an -isystem path: it shadows libc++'s headers, hence the symlink dir).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/tools/astc_check
W=${1:-/tmp/astc_check}; mkdir -p "$W/inc" "$W/bc23"
NDK=${ANDROID_NDK_ROOT:-$(ls -d "${ANDROID_HOME:-$HOME/Library/Android/sdk}"/ndk/28.* | tail -1)}
INC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/include
ln -sfn "$INC/vulkan" "$W/inc/vulkan"; ln -sfn "$INC/vk_video" "$W/inc/vk_video"
CXX="clang++ -std=c++20 -O1 -I$ROOT/runtime -isystem $W/inc -DPES17_HAVE_VULKAN=1"
$CXX "$HERE/gen.cpp"   "$ROOT/runtime/gpu/vulkan/bc_decoder.cpp" -o "$W/gen"
$CXX "$HERE/gen23.cpp" "$ROOT/runtime/gpu/vulkan/bc_decoder.cpp" -o "$W/gen23"
swiftc -O "$HERE/dec.swift" -o "$W/dec" 2>&1 | grep -E 'error' || true
cd "$W"
echo "== BC1"; ./gen && ./dec && python3 "$HERE/cmp.py"
echo "== BC2 / BC3"; ./gen23 && (cd bc23 && ../dec) && python3 "$HERE/cmp23.py"
