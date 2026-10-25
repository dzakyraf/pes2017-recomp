#!/bin/sh
# Builds tools/framegen_check.cpp against the frame generator and runs it (see the .cpp for what it
# prints). Needs a desktop Vulkan loader and driver (Homebrew vulkan-loader + molten-vk on a Mac).
set -e
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/framegen_check"
prefix="$(brew --prefix 2>/dev/null || echo /usr/local)"
c++ -std=c++20 -O1 -DPES17_HAVE_VULKAN -include vulkan/vulkan.h -I"$prefix/include" -o "$out" \
    "$here/tools/framegen_check.cpp" "$here/runtime/gpu/vulkan/framegen.cpp" -L"$prefix/lib" -lvulkan
exec "$out" "$@"
