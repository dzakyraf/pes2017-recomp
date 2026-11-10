#pragma once
#include "../guest.h"

namespace kernel::xmp
{
    inline constexpr uint32_t kApp = 0xFA;
    inline constexpr uint32_t kGetPlaybackController = 0x0007001B;
    inline constexpr uint32_t kGetStatus = 0x00070009;

    inline bool Handled(uint32_t message)
    {
        return message == kGetPlaybackController || message == kGetStatus;
    }

    // XMP's synchronous queries return HRESULTs. A positive Win32 error would
    // be mistaken for success by the title's HRESULT-to-Win32 SDK wrapper.
    inline uint32_t Query(uint32_t message, uint32_t packet, uint32_t length)
    {
        constexpr uint32_t invalid = 0x80070057u; // E_INVALIDARG
        if (!Handled(message)) return invalid;
        const uint32_t bytes = message == kGetStatus ? 8 : 12;
        // The SDK passes length 0 for these in-process requests. Explicit
        // lengths still have to cover the packet; guest addresses must not wrap.
        if (!guest::Base() || !packet || (packet & 3) ||
            uint64_t(packet) + bytes > 0x100000000ull || (length && length < bytes))
            return invalid;

        const be32* words = GuestPtr<be32>(packet);
        if (uint32_t(words[0]) != 2) return invalid; // supported SDK client
        const uint32_t first = words[1];
        // GetStatus has only two words: do not read a third word.
        const uint32_t second = message == kGetPlaybackController ? uint32_t(words[2]) : 0;
        if (!first || (first & 3) ||
            (message == kGetPlaybackController && (!second || (second & 3))))
            return invalid;

        // There is no Xbox system/custom-music player overriding the title:
        // controller=0, locked=0, status=idle (0). Validate both pointers before
        // either store so a rejected request cannot partially change its outputs.
        *GuestPtr<be32>(first) = 0;
        if (message == kGetPlaybackController) *GuestPtr<be32>(second) = 0;
        return 0;
    }
}
