// XMP's output words decide whether the title may play its own music. A
// successful query must not leave them as whatever was on the guest stack.
// No game image, recompiled code, Android device or audio driver is needed.
#include "kernel/xmp.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace
{
    alignas(4) std::array<uint8_t, 256> g_memory;
    uint8_t* g_base = g_memory.data();
    int g_checks = 0, g_failures = 0;
    constexpr uint32_t kPacket = 16, kFirst = 64, kSecond = 80;

    void Check(bool ok, const char* what)
    {
        g_checks++;
        if (ok) return;
        g_failures++;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }

    void Put(uint32_t address, uint32_t value)
    {
        for (int i = 0; i < 4; i++) g_memory[address + i] = uint8_t(value >> (24 - i * 8));
    }

    uint32_t Get(uint32_t address)
    {
        uint32_t value = 0;
        for (int i = 0; i < 4; i++) value = (value << 8) | g_memory[address + i];
        return value;
    }

    void Reset(uint8_t fill)
    {
        g_memory.fill(fill);
        g_base = g_memory.data();
        Put(kPacket, 2);
        Put(kPacket + 4, kFirst);
        Put(kPacket + 8, kSecond);
    }

    void Rejected(uint32_t message, uint32_t packet, uint32_t length)
    {
        const auto before = g_memory;
        Check(kernel::xmp::Query(message, packet, length) == 0x80070057u,
              "invalid XMP request returns HRESULT E_INVALIDARG, not positive Win32 87");
        Check(before == g_memory, "rejected request writes nothing, including its first output");
    }
}

// The runtime's existing translation helper, pointed at bounded test memory.
uint8_t* guest::Base() { return g_base; }

int main()
{
    using namespace kernel::xmp;
    Check(Handled(kGetPlaybackController) && Handled(kGetStatus), "both music queries are handled");
    Check(!Handled(0x0007002B) && !Handled(0x00070019), "other XMP messages remain unhandled");

    for (uint8_t fill : { uint8_t(0), uint8_t(1), uint8_t(0xA5), uint8_t(0xFF) })
    {
        for (uint32_t length : { 0u, 12u })
        {
            Reset(fill);
            const auto before = g_memory;
            Check(Query(kGetPlaybackController, kPacket, length) == 0, "playback-control query succeeds");
            Check(Get(kFirst) == 0 && Get(kSecond) == 0, "both ownership/locked outputs initialized");
            auto expected = before;
            std::memset(expected.data() + kFirst, 0, 4);
            std::memset(expected.data() + kSecond, 0, 4);
            Check(expected == g_memory, "only the two four-byte outputs change");
        }
        Reset(fill);
        Put(kPacket + 8, 0);  // GetStatus has no second pointer.
        Check(Query(kGetStatus, kPacket, 8) == 0, "status needs only its eight-byte packet");
        Check(Get(kFirst) == 0, "no system music player means idle status");
        Check(Get(kSecond) == uint32_t(fill) * 0x01010101u, "status leaves other words untouched");
    }

    // The eight-byte status packet may end exactly at the end of guest memory.
    Reset(0xA5);
    Put(248, 2);
    Put(252, kFirst);
    Check(Query(kGetStatus, 248, 0) == 0 && Get(kFirst) == 0,
          "status never reads the nonexistent third packet word");

    Reset(0xA5); Rejected(kGetPlaybackController, 0, 0);
    Reset(0xA5); Rejected(kGetPlaybackController, kPacket + 1, 0);
    Reset(0xA5); Rejected(kGetPlaybackController, 0xFFFFFFFCu, 0);
    Reset(0xA5); Rejected(kGetStatus, 0xFFFFFFFCu, 0);
    Reset(0xA5); Rejected(kGetPlaybackController, kPacket, 8);
    Reset(0xA5); Rejected(kGetStatus, kPacket, 4);
    Reset(0xA5); Put(kPacket, 1); Rejected(kGetStatus, kPacket, 0);
    Reset(0xA5); Put(kPacket + 4, 0); Rejected(kGetStatus, kPacket, 0);
    Reset(0xA5); Put(kPacket + 4, kFirst + 1); Rejected(kGetStatus, kPacket, 0);
    Reset(0xA5); Put(kPacket + 8, 0); Rejected(kGetPlaybackController, kPacket, 0);
    Reset(0xA5); Put(kPacket + 8, kSecond + 2); Rejected(kGetPlaybackController, kPacket, 0);
    Reset(0xA5); g_base = nullptr; Rejected(kGetStatus, kPacket, 0);
    Reset(0xA5); Rejected(0x0007002B, kPacket, 0);

    if (g_failures)
    {
        std::fprintf(stderr, "xmp: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("xmp: %d checks passed\n", g_checks);
    return 0;
}
