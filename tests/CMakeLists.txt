# The checks `ctest` runs: the durable write, the log's repeat counting and the
# software block-compressed decoder. They are defined in a project of their own
# so they can be configured on a tree that cannot build the runtime -- the root
# CMakeLists refuses to configure without the recompiled guest code, which is
# made from the disc's executables and is in no repository, so a CI runner has
# none of it (.github/workflows/tests.yml). The root includes this same file,
# so a local `ctest` runs exactly what CI runs:
#
#   cmake -S tests -B build-tests -G Ninja
#   ctest --test-dir build-tests --output-on-failure
#
# Nothing here needs a disc, a game file, a GPU or the guest image: each check
# depends on one source file and the headers around it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(pes17checks CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release)
    endif()
    enable_testing()
endif()

set(PES17_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../runtime")

# bc_decoder.cpp includes <vulkan/vulkan.h> for the format constants it answers
# with. It opens no device and calls into no driver, so the headers are all it
# needs; Windows has none to find and fetches them as the root build does. The
# root build has already done this when it includes this file, hence the test
# for the target rather than for the platform alone.
if(NOT TARGET Vulkan::Vulkan)
    if(WIN32)
        include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/vulkan-windows.cmake")
    else()
        find_package(Vulkan)
    endif()
endif()

# ---- durable writes --------------------------------------------------------
# Unlike the tools in ../tools, which are harnesses to be read, this asserts:
# a save survives losing the process part-way, a replacement leaves the previous
# contents whole, a successful write leaves no `.tmp`, one left by a dead run is
# not mistaken for the target, and sizes round-trip across block boundaries.
add_executable(durable-test
    "${CMAKE_CURRENT_SOURCE_DIR}/durable_test.cpp"
    "${PES17_RUNTIME}/kernel/durable.cpp")
target_include_directories(durable-test PRIVATE "${PES17_RUNTIME}")
add_test(NAME durable COMMAND durable-test)

# ---- the log's repeat counting ---------------------------------------------
# The rule that keeps a run readable when it logs one thing a thousand times
# (runtime/log.h): a line is written as it doubles with the count beside it, two
# lines differing only by an address are one kind, and a repetition in progress
# cannot flood.
add_executable(log-test
    "${CMAKE_CURRENT_SOURCE_DIR}/log_repeat_test.cpp")
target_include_directories(log-test PRIVATE "${PES17_RUNTIME}")
add_test(NAME log COMMAND log-test)

# ---- the software block-compressed decoder ---------------------------------
# What a GPU without native BC support samples (bc_decoder.cpp), checked against
# a decoder written from the formats themselves -- so a wrong index bit order,
# an endpoint swap or a row stride fails here rather than looking like a bad
# texture on one phone.
add_executable(bc-test
    "${CMAKE_CURRENT_SOURCE_DIR}/bc_decoder_test.cpp"
    "${PES17_RUNTIME}/gpu/vulkan/bc_decoder.cpp")
target_include_directories(bc-test PRIVATE "${PES17_RUNTIME}")
if(TARGET Vulkan::Vulkan)
    target_link_libraries(bc-test PRIVATE Vulkan::Vulkan)
    target_compile_definitions(bc-test PRIVATE PES17_HAVE_VULKAN=1)
endif()
add_test(NAME bc COMMAND bc-test)

# ---- volume textures -------------------------------------------------------
# Where every level of a 3D texture lives in guest memory (runtime/gpu/texture.cpp),
# checked against Xenia's GetGuestTextureLayout transcribed for the comparison:
# the spans, the level offsets, and that untiling a volume reads each level from
# its own place. The depth term of a volume's level stride is what the code
# called unchecked.
add_executable(volume-test
    "${CMAKE_CURRENT_SOURCE_DIR}/texture_volume_test.cpp"
    "${PES17_RUNTIME}/gpu/texture.cpp")
target_include_directories(volume-test PRIVATE "${PES17_RUNTIME}")
add_test(NAME volume COMMAND volume-test)

# ---- release tag order -----------------------------------------------------
# launcher/version.h decides whether a release is newer than the running build
# (launcher/update.cpp). A tag it cannot read means the launcher never offers
# that release -- which is what happened to this project's own -alpha tags --
# so the order and the refusals are checked here, with no network or launcher.
add_executable(version-test
    "${CMAKE_CURRENT_SOURCE_DIR}/version_test.cpp")
target_include_directories(version-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../launcher")
add_test(NAME version COMMAND version-test)

# ---- system music queries --------------------------------------------------
# XMP replies gate the title's own music. Check the real big-endian packet and
# output handling without the game image, Android or an audio device.
add_executable(xmp-test "${CMAKE_CURRENT_SOURCE_DIR}/xmp_test.cpp")
target_include_directories(xmp-test PRIVATE "${PES17_RUNTIME}")
add_test(NAME xmp COMMAND xmp-test)

# ---- the store of converted textures ---------------------------------------
# What one session converts (BC to ASTC) the next reads back from disk
# (runtime/gpu/vulkan/texture_store.cpp): byte for byte, never for other source
# bytes, a record cut short by a crash dropped, another encoder's file restarted.
# POSIX only, as the store is: Windows samples BC itself and keeps nothing.
if(NOT WIN32)
    add_executable(texture-store-test
        "${CMAKE_CURRENT_SOURCE_DIR}/texture_store_test.cpp"
        "${PES17_RUNTIME}/gpu/vulkan/texture_store.cpp")
    target_include_directories(texture-store-test PRIVATE "${PES17_RUNTIME}")
    find_package(Threads REQUIRED)
    target_link_libraries(texture-store-test PRIVATE Threads::Threads)
    add_test(NAME texture-store COMMAND texture-store-test)
endif()

if(WIN32)
    # The same rule the game is linked with: no C++ runtime beside the
    # executable. Without it a check that pulls in one more libstdc++ symbol
    # than the shell's PATH can supply -- a std::filesystem one, say -- fails at
    # process start (0xc0000139) instead of failing on the code under test, and
    # a check that never starts looks like a check that never ran.
    target_link_options(durable-test PRIVATE -static)
    target_link_options(log-test PRIVATE -static)
    target_link_options(bc-test PRIVATE -static)
    target_link_options(volume-test PRIVATE -static)
    target_link_options(version-test PRIVATE -static)
    target_link_options(xmp-test PRIVATE -static)
endif()

# ---- the dynamic scene scale -------------------------------------------------
# The policy that lowers the match's render scale when its GPU time runs over
# the frame and when the phone reports heat, and raises it again when there is
# room (runtime/gpu/vulkan/dynamic_scale.h). Header-only arithmetic.
add_executable(dynamic-scale-test
    "${CMAKE_CURRENT_SOURCE_DIR}/dynamic_scale_test.cpp")
target_include_directories(dynamic-scale-test PRIVATE "${PES17_RUNTIME}")
add_test(NAME dynamic-scale COMMAND dynamic-scale-test)
