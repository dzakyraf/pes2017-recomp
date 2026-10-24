// PES17_DUMP_TEXTURES: every distinct texture the title binds, untiled, as PNM.
// Always built (it costs a null check per draw until the switch is set), because
// the pitch's markings and the like can only be judged by looking at the textures.
// Read with env::, not diag::, so it works in the release build.
#include <ppc_recomp_shared.h>
#include "internal.h"
#include "gpu.h"
#include "texture.h"
#include "../env.h"
#include "../guest.h"
#include "../log.h"
#include "../kernel/kernel.h"

#include <cstdio>
#include <unordered_set>
#include <vector>

namespace
{
    using namespace gpu::detail;

    // PES17_DUMP_TEXTURES=<dir>: the only way to confirm by eye that untiling,
    // endianness, format and addressing are all right at once -- a wrong tiling
    // function still produces a plausible buffer, but not a picture.
    const char* DumpTextureDirectory()
    {
        static const char* dir = env::Text("PES17_DUMP_TEXTURES");
        return dir;
    }

    void DumpTexture(const gpu::TextureFetch& fetch)
    {
        const char* name = gpu::TextureFormatName(fetch.Format());
        if (!name) name = "unknown";
        const uint32_t va = kernel::FromPhysical(fetch.BaseAddress());
        if (!va)
        {
            LOGW("texture: %s %ux%u at physical %08X does not resolve",
                 name, fetch.Width(), fetch.Height(), fetch.BaseAddress());
            return;
        }
        // PES17_DUMP_TEXTURES_RAW=1: the bytes as the guest holds them, before any
        // untiling, so a layout can be tried offline instead of costing a run per
        // guess. Always for a volume.
        static const bool raw = env::Flag("PES17_DUMP_TEXTURES_RAW");
        if (fetch.Dimension() == gpu::TextureDimension::D3 || raw)
        {
            char path[512];
            static uint32_t sequence = 0;
            std::snprintf(path, sizeof path, "%s/raw_%s_%ux%ux%u_at_%08X_%03u.bin",
                          DumpTextureDirectory(), name, fetch.Width(), fetch.Height(),
                          fetch.Depth(), fetch.BaseAddress(), sequence++);
            const gpu::TextureFormatInfo info = gpu::TextureFormatOf(fetch.Format());
            const uint32_t blockWidth = info.blockWidth ? info.blockWidth : 1;
            const uint32_t blockHeight = info.blockHeight ? info.blockHeight : 1;
            const size_t bytes =
                fetch.Dimension() == gpu::TextureDimension::D3
                    ? size_t(fetch.Pitch()) * ((fetch.Height() + 31) & ~31u) * fetch.Depth() * 4
                    : size_t((fetch.Pitch() / blockWidth + 31) & ~31u) *
                          ((((fetch.Height() + blockHeight - 1) / blockHeight) + 31) & ~31u) *
                          (info.bitsPerBlock / 8);
            if (std::FILE* f = std::fopen(path, "wb"))
            {
                std::fwrite(guest::Base() + va, 1, bytes, f);
                std::fclose(f);
                LOGI("texture: wrote %s (%zu bytes, tiled as the guest holds them)", path, bytes);
            }
        }
        const gpu::TextureExtents extents = gpu::ExtentsOf(fetch);
        const uint32_t mipVa = extents.mipAddress ? kernel::FromPhysical(extents.mipAddress) : 0;
        const gpu::TextureData data = gpu::ReadTexture(fetch, guest::Base() + va,
                                                       mipVa ? guest::Base() + mipVa : nullptr);
        if (!data.ok)
        {
            LOGW("texture: %s %ux%u not read: %s", name, fetch.Width(), fetch.Height(), data.error);
            return;
        }
        std::vector<uint8_t> rgba;
        if (!gpu::DecodeToRgba(data, fetch.Format(), rgba))
        {
            LOGI("texture: %s %ux%u read (%zu bytes) but not decodable for display",
                 name, data.width, data.height, data.bytes.size());
            return;
        }
        // Blank in guest memory means the title has not filled it, which no
        // untiling changes -- worth distinguishing from a bad read.
        size_t nonZero = 0;
        for (uint8_t b : data.bytes) nonZero += (b != 0);
        static const char* kDimensions[] = { "1D", "2D", "3D", "cube" };
        LOGI("texture: %s %ux%u %s at physical %08X (%08X), pitch %u, %s, %zu%% of %zu bytes"
             " non-zero, %u layers", name, data.width, data.height,
             kDimensions[uint32_t(fetch.Dimension()) & 3], fetch.BaseAddress(), va,
             fetch.Pitch(), fetch.Tiled() ? "tiled" : "linear",
             data.bytes.empty() ? 0 : nonZero * 100 / data.bytes.size(), data.bytes.size(),
             data.layers);
        // A streamed texture fills its small levels first and says so with a min
        // mip above zero and a separate address for the chain.
        LOGI("texture:   mips %u..%u, chain at %08X%s; its six fetch dwords: %08X %08X %08X"
             " %08X %08X %08X", fetch.MinMipLevel(), fetch.MaxMipLevel(), fetch.MipAddress(),
             fetch.PackedMips() ? ", packed" : "", fetch.d[0], fetch.d[1], fetch.d[2],
             fetch.d[3], fetch.d[4], fetch.d[5]);
        // Every slice, stacked: a volume whose first slice is blank and whose
        // rest is not looks exactly like a volume that read as nothing. Each
        // level of a chain after the first goes in a file of its own.
        for (uint32_t level = 0; level < data.levels.size(); level++)
        {
            if (level && !gpu::DecodeToRgba(data, fetch.Format(), rgba, level)) break;
            const gpu::TextureData::Level& l = data.levels[level];
            char path[512];
            if (level)
                std::snprintf(path, sizeof path, "%s/%s_%ux%u_at_%08X_level%u.pnm",
                              DumpTextureDirectory(), name, data.width, data.height,
                              fetch.BaseAddress(), level);
            else
                std::snprintf(path, sizeof path, "%s/%s_%ux%u_at_%08X.pnm",
                              DumpTextureDirectory(), name, data.width, data.height,
                              fetch.BaseAddress());
            std::FILE* f = std::fopen(path, "wb");
            if (!f) break;
            std::fprintf(f, "P6\n%u %u\n255\n", l.width, l.height * l.layers);
            for (size_t i = 0; i < rgba.size(); i += 4) std::fwrite(rgba.data() + i, 1, 3, f);
            std::fclose(f);
            if (!level) LOGI("texture: wrote %s", path);
        }
    }
}

bool gpu::detail::DumpingTextures() { return DumpTextureDirectory() != nullptr; }

// Each distinct texture shape the title binds, once. A texture the title
// streams in is empty at its first bind and full a second later, which dumping
// on first sight cannot tell from one that never arrives, so
// PES17_DUMP_TEXTURES_AFTER=<seconds> waits.
void gpu::detail::DumpBoundTextures(const RegisterFile& r)
{
    static const double after = env::Real("PES17_DUMP_TEXTURES_AFTER");
    static std::unordered_set<uint64_t> dumped;
    if (Seconds() < after) return;
    for (uint32_t slot = 0; slot < 32; slot++)
    {
        TextureFetch fetch;
        for (uint32_t i = 0; i < 6; i++) fetch.d[i] = r[kConstantBaseFetch + slot * 6 + i];
        if (!fetch.IsTexture() || !fetch.BaseAddress()) continue;
        const uint64_t shape = (uint64_t(uint32_t(fetch.Sign(0))) << 46)
                             | (uint64_t(fetch.Format()) << 40)
                             | (uint64_t(uint32_t(fetch.Dimension())) << 38)
                             | (uint64_t(fetch.Tiled() ? 1 : 0) << 37)
                             | (uint64_t(uint32_t(fetch.Endian())) << 35)
                             | (uint64_t(fetch.Width() & 0x1FFF) << 22)
                             | (uint64_t(fetch.Height() & 0x1FFF) << 9)
                             | uint64_t(fetch.MaxMipLevel() & 0xF);
        if (dumped.insert(shape).second) DumpTexture(fetch);
    }
}

