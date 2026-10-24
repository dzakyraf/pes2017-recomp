// A draw: the render target and pass it goes into, its constants and
// textures, the fixed-function state, and the command itself.
#include "renderer_state.h"
#include "../../diagnostics.h"
#include "texture_cache.h"
#include "../texture.h"
#include "../../guest.h"
#include "../../kernel/physical.h"
#include "../../log.h"
#include "../../env.h"
#include "../internal.h"
#include "../xe/memexport.h"
#include <atomic>

#include <algorithm>
#include <bit>
#include "../../kernel/kernel.h"
#include "../../guest_memory.h"
#include <cmath>
#include <unistd.h>
#include <sys/stat.h>
#include <string>
#include <cstring>

#ifdef PES17_ANDROID
#include "../../android/android.h"
#endif

#ifdef PES17_HAVE_VULKAN

namespace vk::renderer::detail
{
    // `adb shell touch .../files/dump_frame`: the next frame's draws are written
    // to the log, one line each, with every texture they sample -- what a
    // screenshot cannot say about a screen that looks wrong.
    bool g_dumpDraws = false;
    std::vector<uint64_t> g_skipPixelShaders;

    // The seated crowd (docs/PORTING.md "Crowd, second pass"): the person shader that draws its
    // sprite atlas, the two billboard passes into the G-buffers, and the full-screen composite
    // that lights them. Only the composite's prefix was ever written down, so all four are
    // matched on their high bits.
    static bool g_crowdFlipped = false;
    bool CrowdOff()
    {
        static const bool base = env::Flag("PES17_NO_CROWD");
        return base != g_crowdFlipped;
    }
    void FlipCrowdEvery()
    {
        static const uint64_t every = env::Number("PES17_CROWD_ALTERNATE", 0);
        if (every && g.frames && g.frames % every == 0) g_crowdFlipped = !g_crowdFlipped;
    }

    bool IsCrowdShader(uint64_t hash)
    {
        return (hash >> 40) == 0xB4BBEAull || (hash >> 40) == 0x70F64Aull ||
               (hash >> 40) == 0x4934ECull || (hash >> 32) == 0xAB28D67Aull;
    }
    uint32_t g_drawLo = 0, g_drawHi = 0xFFFFFFFFu, g_frameDraw = 0;
    bool g_noVertexReuse = false;

    uint64_t SampleGuestMemory(uint32_t address, uint32_t bytes)
    {
        const uint32_t va = kernel::FromPhysical(address);
        if (!va || bytes < 1024) return 0;
        const uint8_t* at = guest::Base() + va;
        constexpr uint32_t kWords = 256;
        const uint64_t step = std::max<uint64_t>(4, (uint64_t(bytes) / kWords) & ~3ull);
        uint64_t sum = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < kWords && uint64_t(i) * step + 4 <= bytes; i++)
        {
            uint32_t word;
            std::memcpy(&word, at + uint64_t(i) * step, 4);
            sum = (sum ^ word) * 0x100000001B3ull;
        }
        return sum | 1;
    }
    bool g_noWriteBackCap = false;
    bool g_dumpHeaders = false;
    uint32_t g_dumpIndex = 0;

    bool DumpRequested()
    {
#ifdef PES17_ANDROID
        const std::string trigger = android::GetPaths().external + "/dump_frame";
        if (access(trigger.c_str(), F_OK) != 0) return false;
        unlink(trigger.c_str());
        return true;
#else
        return false;
#endif
    }

    // How far a clearing rectangle really reaches, in the surface's pixels: its own
    // corners, run through its vertex shader on the CPU as the depth-only clear does.
    // Its scissor is left at the reset value (8192 square) and says nothing; the
    // pass that clear opens would otherwise cover the whole 2048-row surface.
    bool ClearRectangleReach(const gpu::RegisterFile& r, const vk::renderer::DrawCall& call, uint32_t& reachX, uint32_t& reachY)
    {
        if (!call.vertexCode || call.indexed) return false;
        xe_memexport::Job job{};
        job.registers = r.values; job.vertexCode = call.vertexCode; job.vertexWords = call.vertexWords;
        job.vertexCount = 1;
        job.firstVertex = r[0x2102] & 0xFFFFFFu;
        job.physicalBase = GuestPtr<uint8_t>(kernel::FromPhysical(0)); job.physicalSize = 0x20000000;
        float corner[4]{}, maxX = 0, maxY = 0;
        bool have = false;
        for (uint32_t v = 0; v < 3 && v < call.indexCount; v++)
            if (xe_memexport::Position(job, v, corner))
            {
                maxX = std::max(maxX, corner[0] + 0.5f);
                maxY = std::max(maxY, corner[1] + 0.5f);
                have = true;
            }
        if (!have || !(maxX > 0) || !(maxY > 0) || maxX > 8192.0f || maxY > 8192.0f) return false;
        reachX = uint32_t(std::ceil(maxX));
        reachY = uint32_t(std::ceil(maxY));
        return true;
    }

    void ScissorOf(const gpu::RegisterFile& r, VkRect2D& scissor)
    {
        const gpu::ScissorCorner topLeft{ r[gpu::PA_SC_WINDOW_SCISSOR_TL] };
        const gpu::ScissorCorner bottomRight{ r[gpu::PA_SC_WINDOW_SCISSOR_BR] };
        scissor.offset = { int32_t(topLeft.X()), int32_t(topLeft.Y()) };
        scissor.extent = { bottomRight.X() > topLeft.X() ? bottomRight.X() - topLeft.X() : 0,
                           bottomRight.Y() > topLeft.Y() ? bottomRight.Y() - topLeft.Y() : 0 };
    }

    // The viewport is a scale and an offset rather than a rectangle, and
    // PA_CL_VTE_CNTL says which of the six terms apply at all.
    void ViewportOf(const gpu::RegisterFile& r, const VkRect2D& scissor, VkViewport& viewport)
    {
        auto asFloat = [&](uint32_t index) { return std::bit_cast<float>(r[index]); };
        const gpu::ViewportControl control{ r[gpu::PA_CL_VTE_CNTL] };

        // The hardware's transform is `screen = offset + ndc * scale`; Vulkan's is
        // `screen = y + (ndc + 1)/2 * height`, so height = 2*scale and y = offset -
        // scale.
        //
        // Keeping the sign is the whole point. Xenos puts +Y up in clip space and
        // expresses that as a negative Y scale; Vulkan puts +Y down. Taking the
        // magnitude gives a viewport of the right size that draws upside down -- and
        // reverses every triangle's winding with it, so culling discards the frame.
        //
        // A term that is switched off is not absent, it is one: the hardware
        // computes `window = ndc * 1 + 0` and the shader is expected to have
        // emitted window coordinates itself. That is what a pre-transformed
        // vertex is -- D3D9's XYZRHW -- and MW2 clears EDRAM with one. Filling
        // the target's size in instead maps [-1,1] across the whole surface, so
        // a vertex at pixel 1024 lands at NDC 1024 and the primitive is clipped
        // away entirely. Follows Xenia's GetViewportInfo (BSD-3).
        const float xScale = control.XScale() ? asFloat(gpu::PA_CL_VPORT_XSCALE) : 1.0f;
        const float yScale = control.YScale() ? asFloat(gpu::PA_CL_VPORT_XSCALE + 2) : 1.0f;
        const float xOffset = control.XOffset() ? asFloat(gpu::PA_CL_VPORT_XSCALE + 1) : 0.0f;
        const float yOffset = control.YOffset() ? asFloat(gpu::PA_CL_VPORT_XSCALE + 3) : 0.0f;
        float x = xOffset - xScale, y = yOffset - yScale;
        float w = 2.0f * xScale, h = 2.0f * yScale;
        // A viewport of no width is not a transform, it is an unset register.
        if (w == 0.0f || h == 0.0f) { x = float(scissor.offset.x); y = float(scissor.offset.y);
                                      w = float(scissor.extent.width); h = float(scissor.extent.height); }

        // D3D9 puts a pixel's centre on the integer coordinate, Vulkan half a
        // pixel further on (PA_SU_VTX_CNTL.pix_center: 0 is D3D's). Shifting the
        // viewport by half a pixel lines the two up, as Xenia does. Without it an
        // edge through a pixel's D3D centre misses the Vulkan one: PES 2017 writes
        // its exposure into a 1x1 target with a triangle whose edge runs through
        // that pixel's Vulkan centre, so the value was never written, the scene
        // kept a stale exposure of 0.06 and the match drew a twentieth as bright.
        if (!(r[gpu::PA_SU_VTX_CNTL] & 1))
        {
            x += 0.5f;
            y += 0.5f;
        }
        viewport.x = x;
        viewport.y = y;
        viewport.width = w;
        viewport.height = h;
        const float zScale = control.ZScale() ? asFloat(gpu::PA_CL_VPORT_XSCALE + 4) : 1.0f;
        const float zOffset = control.ZOffset() ? asFloat(gpu::PA_CL_VPORT_XSCALE + 5) : 0.0f;
        viewport.minDepth = std::clamp(std::min(zOffset, zOffset + zScale), 0.0f, 1.0f);
        viewport.maxDepth = std::clamp(std::max(zOffset, zOffset + zScale), 0.0f, 1.0f);
    }

    // Slots neither shader names are left empty: the layout declares all
    // thirty-two, but a shader cannot read what it did not declare.
    //
    // `scaled` gets a bit for each slot bound to a resolve's copy, whose image
    // is State::scale times the size the title gave it.

    // Frame dump: a kit-sized texture's average colour, read from guest memory
    // (the bytes the title made, whatever the GPU side holds). tools/kits.sh
    // groups the players by it.
    void LogKitTexture(uint32_t slot, const gpu::TextureFetch& fetch, const char* source)
    {
        if (fetch.Format() != 6 || fetch.Width() != 256 || fetch.Height() != 256) return;
        const uint32_t va = kernel::FromPhysical(fetch.BaseAddress());
        if (!va) return;
        const uint8_t* bytes = guest::Base() + va;
        uint64_t sum[4] = {};
        uint32_t n = 0;
        for (uint32_t y = 0; y < 256; y += 8)
            for (uint32_t x = 0; x < 256; x += 8)
            {
                const uint8_t* p = bytes + (size_t(y) * 256 + x) * 4;   // linear; tiled is close enough for an average
                for (int c = 0; c < 4; c++) sum[c] += p[c];
                n++;
            }
        LOGW("DUMP   kit slot %u: %08X %s %s avg bytes %02X %02X %02X %02X", slot, fetch.BaseAddress(),
             fetch.Tiled() ? "tiled" : "linear", source, uint32_t(sum[0] / n), uint32_t(sum[1] / n),
             uint32_t(sum[2] / n), uint32_t(sum[3] / n));
    }

    VkDescriptorSet TextureSetFor(const gpu::RegisterFile& r, uint32_t mask, const uint8_t* kinds,
                                  uint32_t& scaled)
    {
        scaled = 0;
        Stopwatch watch(g.textureNanoseconds);
        uint64_t ids[vk::textures::kSlots]{};
        for (uint32_t slot = 0; slot < vk::textures::kSlots; slot++)
        {
            if (!(mask & (1u << slot))) continue;
            gpu::TextureFetch fetch;
            for (uint32_t i = 0; i < 6; i++)
                fetch.d[i] = r[gpu::kConstantBaseFetch + slot * 6 + i];
            if (!fetch.IsTexture() || (!fetch.BaseAddress() && !fetch.MipAddress())) continue;
            auto found = g.resolvedTo.find(fetch.BaseAddress());
            const Resolved* newest =
                found == g.resolvedTo.end() ? nullptr
                                            : &found->second.copies[found->second.newest];
            // Memory a resolve once wrote is not a resolve for ever. PES 2017's
            // pitch -- a 2048x2048 DXT1 the title loads -- sits where an earlier
            // screen resolved a 512x128 colour image, and was drawn from that
            // copy: the grass came out grey. A colour copy is only what a fetch
            // reads when the fetch asks for the format the resolve wrote and
            // fits inside it.
            // And a resolve written back to guest memory is read from memory:
            // the title builds each player's kit there and then reuses the
            // address for the next player's composition, so the GPU copy can be
            // a different player's kit -- defenders in the keeper's, opponents
            // in ours.
            if (newest && newest->writtenBack) newest = nullptr;
            // And memory the title has written since the resolve is not the resolve either --
            // for a fetch that is a part of the copy. One that is exactly the copy's size is
            // the title reading what it resolved (a kit composition: 256x256 resolved and
            // fetched at 256x256, with the memory rewritten around it); applying the check
            // there turned every kit but the first team's black.
            if (newest && newest->memorySignature &&
                (fetch.Width() < newest->width || fetch.Height() < newest->height) &&
                SampleGuestMemory(fetch.BaseAddress(), newest->memoryBytes) != newest->memorySignature)
            {
                if (g_dumpDraws)
                    LOGW("DUMP   tex slot %u: not the resolve at %08X (the title wrote the memory since)",
                         slot, fetch.BaseAddress());
                static std::atomic<uint32_t> told{ 0 };
                if (told++ < 40)
                    LOGI("renderer: %08X was resolved to, then written by the title: read from memory (fetch %ux%u fmt %u)",
                         fetch.BaseAddress(), fetch.Width(), fetch.Height(), fetch.Format());
                newest = nullptr;
            }
            if (newest && !newest->depth &&
                ((newest->guestFormat && fetch.Format() != newest->guestFormat) ||
                 fetch.Width() > newest->width || fetch.Height() > newest->height))
            {
                if (g_dumpDraws)
                    LOGW("DUMP   tex slot %u: not the resolve at %08X (resolve fmt %u %ux%u, fetch fmt %u %ux%u)",
                         slot, fetch.BaseAddress(), newest->guestFormat, newest->width, newest->height,
                         fetch.Format(), fetch.Width(), fetch.Height());
                newest = nullptr;
            }
            // A fetch of a corner of the surface samples the corner's own image
            // (ResolveRing::exact) once a resolve has made one.
            bool corner = false;
            if (newest && newest->view && !newest->depth &&
                (fetch.Width() < newest->width || fetch.Height() < newest->height))
            {
                ResolveRing& ring = found->second;
                ring.fetchWidth = fetch.Width();
                ring.fetchHeight = fetch.Height();
                const Resolved& exact = ring.exact[ring.newest];
                if (exact.view && exact.width == fetch.Width() && exact.height == fetch.Height() &&
                    exact.writtenFrame == newest->writtenFrame)
                {
                    newest = &exact;
                    corner = true;
                }
            }
            if (newest && newest->view)
            {
                // The key names the copy, not just the guest address: the texture
                // cache keys descriptor sets on ids and drops them all when the
                // view behind an id changes, and with a copy per frame slot that
                // would be every frame. The fetch says whether the sampler
                // linearises, and the two views of one copy are different
                // resources to the set cache, so that is part of the key too.
                const bool linearise = !newest->depth && fetch.Linearises() && newest->gammaView;
                const uint64_t key = fetch.BaseAddress() |
                                     (uint64_t(found->second.newest + 1) << 32) |
                                     (uint64_t(linearise ? 1 : 0) << 34) |
                                     (uint64_t(corner ? 1 : 0) << 35);
                ids[slot] = vk::textures::Adopt(
                    key, linearise ? newest->gammaView : newest->view, fetch);
                if (g_dumpDraws) LogKitTexture(slot, fetch, "resolve-copy");
                if (g_dumpDraws)
                    LOGW("DUMP   tex slot %u: words %08X %08X %08X %08X clamp %u/%u", slot, fetch.d[0], fetch.d[1], fetch.d[2],
                         fetch.d[3], uint32_t(fetch.ClampX()), uint32_t(fetch.ClampY()));
                if (g_dumpDraws)
                    LOGW("DUMP   tex slot %u: resolve copy of %08X (copy %ux%u vk fmt %d%s) fetch %ux%u fmt %u%s -> %s",
                         slot, fetch.BaseAddress(), newest->width, newest->height,
                         int(newest->format), newest->depth ? " depth" : "", fetch.Width(), fetch.Height(), fetch.Format(),
                         linearise ? " linearised" : "", ids[slot] ? (corner ? "bound (corner)" : "bound") : "NOT BOUND");
                if (ids[slot])
                {
                    // Only a copy whose image is not the title's size.
                    if (g.scale > 1 || newest->scale != 1.0f) scaled |= 1u << slot;
                    if (g.drawRecord) NoteDrawTexture(slot, fetch, ids[slot], true);
                    continue;
                }
            }
            ids[slot] = vk::textures::Upload(fetch, nullptr);
            if (g_dumpDraws) LogKitTexture(slot, fetch, "guest");
#ifdef PES17_ANDROID
            // The crowd composite's colour sources, as the GPU has them (files/frame_images/tex_<addr>_<w>x<h>.pnm).
            if (g_dumpDraws && ids[slot] &&
                ((fetch.Width() == 256 && fetch.Height() == 2048) || (fetch.Width() == 512 && fetch.Height() == 512) ||
                 (fetch.Width() == 256 && fetch.Height() == 256) || (fetch.Width() == 32 && fetch.Height() == 32)))
            {
                if (fetch.Width() == 256 && fetch.Height() == 2048)
                {
                    // How much of the crowd's colour atlas holds any colour in guest memory (DXT1: two 565 words a block).
                    const uint32_t va = kernel::FromPhysical(fetch.BaseAddress());
                    const uint8_t* mem = va ? guest::Base() + va : nullptr;
                    uint32_t blocks = 0, lit = 0;
                    for (uint32_t b = 0; mem && b < 256u * 2048u / 16u; b++)
                    {
                        blocks++;
                        if (mem[b * 8] | mem[b * 8 + 1] | mem[b * 8 + 2] | mem[b * 8 + 3]) lit++;
                    }
                    LOGW("DUMP   crowd colour atlas %08X: %u of %u blocks have a colour in guest memory", fetch.BaseAddress(), lit, blocks);
                }
                std::vector<uint8_t> bytes;
                const char* error = nullptr;
                if (vk::textures::Readback(ids[slot] & 0xFFFFFFFFull, bytes, &error) &&
                    bytes.size() >= size_t(fetch.Width()) * fetch.Height() * 4)
                {
                    {
                        // The texture's first block as the title left it, next to what the GPU copy holds
                        // at the same corner: a decode that disagrees with its source shows here.
                        const uint32_t va = kernel::FromPhysical(fetch.BaseAddress());
                        const uint8_t* mem = va ? guest::Base() + va : nullptr;
                        uint32_t row2 = fetch.Pitch() ? fetch.Pitch() : fetch.Width();
                        LOGW("DUMP   tex %08X %ux%u fmt %u %s words %08X %08X %08X %08X %08X %08X: memory[0..15] %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X; gpu pixel(0,0) %02X%02X%02X, pixel(%u,%u) %02X%02X%02X; bytes %zu",
                             fetch.BaseAddress(), fetch.Width(), fetch.Height(), fetch.Format(), fetch.Tiled() ? "tiled" : "linear",
                             fetch.d[0], fetch.d[1], fetch.d[2], fetch.d[3], fetch.d[4], fetch.d[5],
                             mem ? mem[0] : 0, mem ? mem[1] : 0, mem ? mem[2] : 0, mem ? mem[3] : 0, mem ? mem[4] : 0, mem ? mem[5] : 0, mem ? mem[6] : 0, mem ? mem[7] : 0,
                             mem ? mem[8] : 0, mem ? mem[9] : 0, mem ? mem[10] : 0, mem ? mem[11] : 0, mem ? mem[12] : 0, mem ? mem[13] : 0, mem ? mem[14] : 0, mem ? mem[15] : 0,
                             bytes[0], bytes[1], bytes[2], fetch.Width() - 4, 0u, bytes[(fetch.Width() - 4) * 4], bytes[(fetch.Width() - 4) * 4 + 1],
                             bytes[(fetch.Width() - 4) * 4 + 2], bytes.size());
                        (void)row2;
                    }
                    const std::string dir = android::GetPaths().external + "/frame_images";
                    mkdir(dir.c_str(), 0775);
                    char name[96];
                    std::snprintf(name, sizeof name, "/tex_%08X_%ux%u.pnm", fetch.BaseAddress(), fetch.Width(), fetch.Height());
                    if (std::FILE* out = std::fopen((dir + name).c_str(), "wb"))
                    {
                        std::fprintf(out, "P6\n%u %u\n255\n", fetch.Width(), fetch.Height());
                        for (size_t i = 0; i < size_t(fetch.Width()) * fetch.Height(); i++) std::fwrite(&bytes[i * 4], 1, 3, out);
                        std::fclose(out);
                    }
                }
                else
                    LOGW("DUMP   tex slot %u: readback of %ux%u failed (%s, %zu bytes)", slot, fetch.Width(), fetch.Height(),
                         error ? error : "?", bytes.size());
            }
#endif
            if (g_dumpDraws)
            {
                // The first kilobyte as the title left it: all zero means the CPU
                // has not produced this texture (or the GPU cannot see what it did).
                const uint32_t va = kernel::FromPhysical(fetch.BaseAddress());
                const uint8_t* bytes = va ? guest::Base() + va : nullptr;
                uint32_t nonzero = 0, first[8]{};
                for (uint32_t i = 0; bytes && i < 1024; i++) nonzero += bytes[i] != 0;
                for (uint32_t i = 0; bytes && i < 8; i++) first[i] = bytes[i];
                LOGW("DUMP   tex slot %u words %08X %08X %08X %08X %08X %08X", slot, fetch.d[0], fetch.d[1], fetch.d[2], fetch.d[3], fetch.d[4], fetch.d[5]);
                LOGW("DUMP   tex slot %u: addr %08X (guest %08X) %ux%u fmt %u pitch %u %s -> %s; first KB %u/1024 non-zero, starts %02X%02X%02X%02X %02X%02X%02X%02X",
                     slot, fetch.BaseAddress(), va, fetch.Width(), fetch.Height(), fetch.Format(), fetch.Pitch(),
                     fetch.Tiled() ? "tiled" : "linear", ids[slot] ? "bound" : "NOT BOUND", nonzero,
                     first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7]);
            }
            // PES17_MARK_EMPTY: the world pass is the multisampled one; the
            // interface and the post chain are not. A texture read from an empty
            // block matters when a draw into the world binds it.
            if constexpr (diag::kOn)
                if (ids[slot] && g.currentSamples > 1)
                    ids[slot] = vk::textures::NoteDrawnInWorld(ids[slot]);
            if (g.drawRecord) NoteDrawTexture(slot, fetch, ids[slot], false);
        }
        return static_cast<VkDescriptorSet>(vk::textures::DescriptorSet(ids, kinds));
    }

    // A draw as the recorder thread makes it: the state that changed since the
    // last draw in the command buffer, and the draw. Which state changed is
    // decided here, on the consumer, against State::Bound.
    struct DrawOp
    {
        enum : uint32_t
        {
            kNdc = 1, kPipeline = 2, kViewport = 4, kScissor = 8, kDepthBias = 16,
            kStencil = 32, kConstants = 64, kTextures = 128, kAlphaTest = 256,
            kIndexBuffer = 512, kIndexed = 1024, kFaces = 2048, kDepthTests = 4096,
            kScaled = 8192,
        };
        uint32_t set = 0;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkViewport viewport{};
        VkRect2D scissor{};
        float ndc[4]{};
        float depthBias[2]{};
        uint32_t stencil[2]{};
        uint32_t push[4]{};
        uint32_t scaled = 0;
        float scaledFactors[2]{};
        VkCullModeFlags cullMode = 0;
        VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        DepthTests depthTests{};
        VkDescriptorSet constantSet = VK_NULL_HANDLE, textureSet = VK_NULL_HANDLE;
        uint32_t dynamic[bindings::kConstantBlocks]{};
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkIndexType indexType = VK_INDEX_TYPE_UINT16;
        uint32_t count = 0, firstIndex = 0;
        int32_t vertexOffset = 0;      // VGT_INDX_OFFSET: added to every vertex index

        void operator()(VkCommandBuffer command) const
        {
            // The pixel stage's range runs over these bytes on its way to the
            // scaled slots, so a write of them names both stages.
            constexpr VkShaderStageFlags kBoth = VK_SHADER_STAGE_VERTEX_BIT |
                                                 VK_SHADER_STAGE_FRAGMENT_BIT;
            if (set & kNdc)
                vkCmdPushConstants(command, layout, kBoth,
                                   bindings::kVertexPushOffset, sizeof ndc, ndc);
            if (set & kScaled)
            {
                uint32_t words[3] = { scaled, 0, 0 };
                std::memcpy(&words[1], scaledFactors, sizeof scaledFactors);
                vkCmdPushConstants(command, layout, kBoth, bindings::kScaledPushOffset, sizeof words, words);
            }
            if (set & kPipeline)
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            if (set & kViewport) vkCmdSetViewport(command, 0, 1, &viewport);
            if (set & kScissor) vkCmdSetScissor(command, 0, 1, &scissor);
            if (set & kDepthBias) vkCmdSetDepthBias(command, depthBias[0], 0.0f, depthBias[1]);
            if (set & kFaces)
            {
                if (dispatch.setCullMode) dispatch.setCullMode(command, cullMode);
                if (dispatch.setFrontFace) dispatch.setFrontFace(command, frontFace);
            }
            if (set & kDepthTests)
            {
                const DepthTests& d = depthTests;
                if (dispatch.setDepthTestEnable)
                {
                    dispatch.setDepthTestEnable(command, d.depthTest);
                    dispatch.setDepthWriteEnable(command, d.depthWrite);
                    dispatch.setDepthCompareOp(command, d.depthCompare);
                    dispatch.setStencilTestEnable(command, d.stencilTest);
                    dispatch.setStencilOp(command, VK_STENCIL_FACE_FRONT_BIT, d.front.failOp,
                                          d.front.passOp, d.front.depthFailOp, d.front.compareOp);
                    dispatch.setStencilOp(command, VK_STENCIL_FACE_BACK_BIT, d.back.failOp,
                                          d.back.passOp, d.back.depthFailOp, d.back.compareOp);
                }
            }
            if (set & kStencil)
            {
                const VkStencilFaceFlags faces[2] = { VK_STENCIL_FACE_FRONT_BIT,
                                                      VK_STENCIL_FACE_BACK_BIT };
                for (uint32_t i = 0; i < 2; i++)
                {
                    const gpu::StencilRefMask state{ stencil[i] };
                    vkCmdSetStencilCompareMask(command, faces[i], state.Mask());
                    vkCmdSetStencilWriteMask(command, faces[i], state.WriteMask());
                    vkCmdSetStencilReference(command, faces[i], state.Reference());
                }
            }
            if (set & kConstants)
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                        bindings::kConstantSet, 1, &constantSet,
                                        bindings::kConstantBlocks, dynamic);
            if (set & kTextures)
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                        bindings::kTextureSet, 1, &textureSet, 0, nullptr);
            if (set & kAlphaTest)
                vkCmdPushConstants(command, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   bindings::kPixelPushOffset, sizeof push, push);
            if (set & kIndexBuffer) vkCmdBindIndexBuffer(command, indexBuffer, 0, indexType);
            if (set & kIndexed) vkCmdDrawIndexed(command, count, 1, firstIndex, vertexOffset, 0);
            else vkCmdDraw(command, count, 1, uint32_t(vertexOffset), 0);
        }
    };
}

using namespace vk::renderer::detail;

bool vk::renderer::DumpingDraws() { return g_dumpDraws; }

void vk::renderer::Draw(const gpu::RegisterFile& r, const DrawCall& call)
{
    struct StageScope
    {
        StageScope() { gpu::detail::SetCurrentDrawStage("Draw_Start"); }
        ~StageScope() { gpu::detail::SetCurrentDrawStage("Draw_Idle"); }
    } stageScope;

    Stopwatch watch(g.drawNanoseconds);
    if (!g.device) return;
    // A flash hunt's record of this draw, open until it returns however it does.
    if constexpr (diag::kOn)
        g.drawRecord = BeginDrawRecord(r, call.vertexHash, call.pixelHash, call.primitive,
                                       call.indexCount, call.indexed ? call.indexAddress : 0);
    struct CloseRecord {
        ~CloseRecord() {
            g.drawRecord = nullptr;
            gpu::detail::SetCurrentDrawStage("idle");
        }
    } closeRecord;
    // and the pass and framebuffer that go with it.
    const auto setupFrom = Timing() ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    g.draws++;
    if (g_dumpHeaders)
        LOGW("DUMP draw %u: prim %u idx %u%s vs %016llX ps %016llX", g_dumpIndex++, call.primitive, call.indexCount,
             call.indexed ? "" : " (auto)", (unsigned long long)call.vertexHash, (unsigned long long)call.pixelHash);
    if (g_dumpDraws)
    {
        const gpu::ColorInfo c{ r[gpu::RB_COLOR_INFO] };
        const gpu::SurfaceInfo sf{ r[gpu::RB_SURFACE_INFO] };
        LOGW("DUMP draw %u: prim %u idx %u%s vs %016llX ps %016llX -> tile %u pitch %u fmt %u x%u blend %08X mask %X depth %08X",
             g_dumpIndex++, call.primitive, call.indexCount, call.indexed ? "" : " (auto)",
             (unsigned long long)call.vertexHash, (unsigned long long)call.pixelHash, c.BaseTile(), sf.Pitch(),
             c.Format(), sf.MsaaSamples(), r[gpu::RB_BLENDCONTROL0], r[gpu::RB_COLOR_MASK], r[gpu::RB_DEPTHCONTROL]);
        {
            static std::unordered_set<uint64_t> shown;
            auto show = [&](const char* kind, uint64_t hash, const uint32_t* code, size_t words) {
                if (!code || !shown.insert(hash).second) return;
                std::string text; char w[12];
                for (size_t i = 0; i < words && i < 240; i++) { std::snprintf(w, sizeof w, "%08X ", code[i]); text += w; }
                LOGW("DUMP ucode %s %016llX (%zu dwords): %s", kind, (unsigned long long)hash, words, text.c_str());
            };
            show("vs", call.vertexHash, call.vertexCode, call.vertexWords);
            show("ps", call.pixelHash, call.pixelCode, call.pixelWords);
        }
        // What the shader is fed: each vertex fetch constant's buffer, its first
        // dwords as floats, and the viewport and scissor it lands in.
        for (uint32_t slot = 0; slot < 96; slot++)
        {
            const uint32_t w0 = r[gpu::kConstantBaseFetch + slot * 2], w1 = r[gpu::kConstantBaseFetch + slot * 2 + 1];
            if ((w0 & 3) != 3 || !((w1 >> 2) & 0xFFFFFF)) continue;
            const uint32_t va = kernel::FromPhysical(w0 & ~3u);
            if (!va) continue;
            const be32* words = reinterpret_cast<const be32*>(guest::Base() + va);
            float f[12];
            for (int i = 0; i < 12; i++) { const uint32_t v = uint32_t(words[i]); std::memcpy(&f[i], &v, 4); }
            {
                std::string hex;
                char t[12];
                for (int i = 0; i < 15; i++) { std::snprintf(t, sizeof t, "%08X ", uint32_t(words[i])); hex += t; }
                LOGW("DUMP   vfetch %u raw: %s", slot, hex.c_str());
            }
            LOGW("DUMP   vfetch %u: addr %08X size %u dwords endian %u: %g %g %g %g | %g %g %g %g | %g %g %g %g",
                 slot, w0 & ~3u, (w1 >> 2) & 0xFFFFFF, w1 & 3, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
        }
        {
            const uint32_t base = r[0x2307] & 0x1FF;
            static std::vector<uint64_t> shownVs;
            static uint64_t shownVsFrame = UINT64_MAX;
            if (shownVsFrame != g.frames) { shownVs.clear(); shownVsFrame = g.frames; }
            const bool firstVs = std::find(shownVs.begin(), shownVs.end(), call.vertexHash) == shownVs.end();
            if (firstVs) shownVs.push_back(call.vertexHash);
            for (uint32_t c = 0; c < (firstVs ? 256u : 8u); c++)
            {
                float v[4];
                for (int k = 0; k < 4; k++) v[k] = std::bit_cast<float>(r[gpu::kConstantBaseAlu + 4 * (base + c) + k]);
                if (c < 8 || v[0] != 0 || v[1] != 0 || v[2] != 0 || v[3] != 0)
                    LOGW("DUMP   vs c%u (base %u) = %g %g %g %g", c, base, v[0], v[1], v[2], v[3]);
            }
            // Pixel shader constants: SQ_PS_CONST names the window (256 for D3D).
            const uint32_t psBase = r[0x2308] & 0x1FF;
            // Every non-zero one the first time a pixel shader shows up in this
            // dump, the first sixteen after that.
            static std::vector<uint64_t> shown;
            static uint64_t shownFrame = UINT64_MAX;
            if (shownFrame != g.frames) { shown.clear(); shownFrame = g.frames; }
            const bool first = std::find(shown.begin(), shown.end(), call.pixelHash) == shown.end();
            if (first) shown.push_back(call.pixelHash);
            for (uint32_t c = 0; c < (first ? 256u : 16u); c++)
            {
                float v[4];
                for (int k = 0; k < 4; k++) v[k] = std::bit_cast<float>(r[gpu::kConstantBaseAlu + 4 * (std::min(psBase, 256u) + c) + k]);
                if (v[0] != 0 || v[1] != 0 || v[2] != 0 || v[3] != 0)
                    LOGW("DUMP   ps c%u (base %u) = %g %g %g %g", c, psBase, v[0], v[1], v[2], v[3]);
            }
            // The last pixel constant is where D3D9 titles keep their literals (the crowd's alpha
            // threshold is in it), so it is shown for every draw, not only the first of a shader.
            if (!first)
            {
                float v[4];
                for (int k = 0; k < 4; k++) v[k] = std::bit_cast<float>(r[gpu::kConstantBaseAlu + 4 * (std::min(psBase, 256u) + 255) + k]);
                LOGW("DUMP   ps c255 (base %u) = %g %g %g %g", psBase, v[0], v[1], v[2], v[3]);
            }
        }
        {
            // Registers (outside the constant windows) that changed since the last draw.
            static std::vector<uint32_t> previous;
            if (previous.size() != gpu::kRegisterCount) previous.assign(gpu::kRegisterCount, 0);
            std::string diff; char t[40]; int shown = 0;
            for (uint32_t i = 0; i < 0x4000 && i < gpu::kRegisterCount; i++)
                if (r.values[i] != previous[i] && shown++ < 40)
                { std::snprintf(t, sizeof t, " %04X:%08X>%08X", i, previous[i], r.values[i]); diff += t; }
            LOGW("DUMP   regdiff%s", diff.c_str());
            std::memcpy(previous.data(), r.values, gpu::kRegisterCount * 4);
        }
        LOGW("DUMP   modeCntl %08X colorcontrol %08X alpharef %g rb_modecontrol %08X color_info %08X depth_info %08X",
             r[gpu::PA_SU_SC_MODE_CNTL], r[0x2202], std::bit_cast<float>(r[gpu::RB_ALPHA_REF]),
             r[gpu::RB_MODECONTROL], r[gpu::RB_COLOR_INFO], r[gpu::RB_DEPTH_INFO]);
        LOGW("DUMP   viewport: control %08X scale %g,%g offset %g,%g; window scissor %08X..%08X",
             r[gpu::PA_CL_VTE_CNTL], std::bit_cast<float>(r[gpu::PA_CL_VPORT_XSCALE]), std::bit_cast<float>(r[gpu::PA_CL_VPORT_XSCALE + 2]),
             std::bit_cast<float>(r[gpu::PA_CL_VPORT_XSCALE + 1]), std::bit_cast<float>(r[gpu::PA_CL_VPORT_XSCALE + 3]),
             r[gpu::PA_SC_WINDOW_SCISSOR_TL], r[gpu::PA_SC_WINDOW_SCISSOR_BR]);
    }

    {
        const uint32_t index = g_frameDraw++;
        if (index < g_drawLo || index > g_drawHi) return;
    }
    if (g_dumpDraws && call.vertexCode && (!call.indexed || call.primitive == 13))
    {
        xe_memexport::Job job{};
        if (call.indexed) { job.indices = GuestPtr<uint8_t>(call.indexAddress); job.index32 = call.index32; }
        job.registers = r.values; job.vertexCode = call.vertexCode; job.vertexWords = call.vertexWords;
        job.vertexCount = call.indexCount;
        job.firstVertex = r[0x2102] & 0xFFFFFFu;
        job.physicalBase = GuestPtr<uint8_t>(kernel::FromPhysical(0)); job.physicalSize = 0x20000000;
        xe_memexport::TraceVertices(job, [](const char* text) { LOGW("DUMP   cpu-vs %s", text); });
    }
    // No crowd (PES17_NO_CROWD, Settings > Performance): its draws are dropped before they cost
    // the recorder or the GPU anything. The title still animates it.
    if (CrowdOff() && IsCrowdShader(call.pixelHash)) { Skip("crowd off"); return; }
    // Bisecting a frame (files/skip_ps: pixel shader hashes, one a line).
    for (uint64_t hash : g_skipPixelShaders)
        if (hash == call.pixelHash) { Skip("skip_ps"); return; }
    const VkPrimitiveTopology topology = TopologyFor(call.primitive);
    if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) { Skip("primitive type"); return; }

    gpu::detail::SetCurrentDrawStage("Draw_ShaderFor");
    const Shader* vertex = ShaderFor(shader::Type::Vertex, call.vertexHash,
                                     call.vertexCode, call.vertexWords);
    const Shader* pixel = ShaderFor(shader::Type::Pixel, call.pixelHash,
                                    call.pixelCode, call.pixelWords);
    if (!vertex || !pixel) { Skip("shader not translated"); return; }

    // A vertex shader that exports to memory is a compute step: run it on the
    // CPU, where the guest's memory is, before anything reads what it writes.
    // A draw with no colour to write and no depth is nothing else.
    if (vertex->translation.writesMemory && call.vertexCode)
    {
        xe_memexport::Job job{};
        job.registers = r.values;
        job.vertexCode = call.vertexCode;
        job.vertexWords = call.vertexWords;
        job.vertexCount = call.indexCount;
        if (call.indexed)
        {
            job.indices = GuestPtr<uint8_t>(call.indexAddress);
            job.index32 = call.index32;
        }
        job.physicalBase = GuestPtr<uint8_t>(kernel::FromPhysical(0));
        job.physicalSize = 0x20000000;
        static std::atomic<uint32_t> runs{ 0 }, wrote{ 0 };
        const uint32_t stored = xe_memexport::Execute(job);
        wrote += stored;
        if (runs++ < 4 || runs % 20000 == 0)
            LOGI("renderer: memexport %016llX: %u vertices, %u exported (%u/%u so far)",
                 (unsigned long long)call.vertexHash, job.vertexCount, stored, wrote.load(), runs.load());
        if (!(r[gpu::RB_COLOR_MASK] & 0xFFFF) && !gpu::DepthControl{ r[gpu::RB_DEPTHCONTROL] }.ZWrite())
            return;
    }

    VkRect2D scissor{};
    ScissorOf(r, scissor);
    if (!scissor.extent.width || !scissor.extent.height) { Skip("empty scissor"); return; }

    const gpu::SurfaceInfo surface{ r[gpu::RB_SURFACE_INFO] };
    const gpu::ColorInfo colour{ r[gpu::RB_COLOR_INFO] };
    const gpu::DepthInfo depthInfo{ r[gpu::RB_DEPTH_INFO] };
    const gpu::DepthControl depthState{ r[gpu::RB_DEPTHCONTROL] };
    const gpu::ViewportControl viewportControl{ r[gpu::PA_CL_VTE_CNTL] };
    const gpu::EdramMode edramMode = gpu::EdramMode(r[gpu::RB_MODECONTROL] & 7);
    if (!surface.Pitch()) { Skip("no surface pitch"); return; }

    // MW2 clears EDRAM by drawing rectangles with the depth test ALWAYS, depth
    // writes on and the viewport transform off (see ClearAliasedDepth).
    const bool clearRectangles = call.primitive == 8 && depthState.ZWrite() &&
                                 depthState.ZFunc() == 7 &&
                                 !viewportControl.XScale() && !viewportControl.YScale();

    // The depth-only form. Answered before the targets are sized, because the
    // rectangle's scissor is left at its reset value -- 8192 square -- and
    // letting that decide how tall a surface is makes every depth buffer in the
    // frame the largest one this runtime allows.
    if (clearRectangles && edramMode == gpu::EdramMode::DepthOnly)
    {
        if (!BeginFrame()) { Skip("no command buffer"); return; }
        // The rectangle's own z is the clear value (its vertex shader, run on
        // the CPU), through the viewport's z terms if on; its corners say how
        // many EDRAM tiles it covers (80x16 samples a tile; 2x MSAA doubles the
        // rows, 4x the columns as well).
        float depth = 0.0f;
        uint32_t endTile = 2048;
        if (call.vertexCode && !call.indexed)
        {
            xe_memexport::Job job{};
            job.registers = r.values; job.vertexCode = call.vertexCode; job.vertexWords = call.vertexWords;
            job.vertexCount = 1;
            job.firstVertex = r[0x2102] & 0xFFFFFFu;
            job.physicalBase = GuestPtr<uint8_t>(kernel::FromPhysical(0)); job.physicalSize = 0x20000000;
            float corner[4]{}, maxX = 0, maxY = 0;
            bool have = false;
            for (uint32_t v = 0; v < 3 && v < call.indexCount; v++)
                if (xe_memexport::Position(job, v, corner))
                {
                    if (v == 0) depth = corner[3] != 0.0f ? corner[2] / corner[3] : corner[2];
                    maxX = std::max(maxX, corner[0] + 0.5f);
                    maxY = std::max(maxY, corner[1] + 0.5f);
                    have = true;
                }
            if (have)
            {
                if (viewportControl.ZScale()) depth *= std::bit_cast<float>(r[gpu::PA_CL_VPORT_XSCALE + 4]);
                if (viewportControl.ZOffset()) depth += std::bit_cast<float>(r[gpu::PA_CL_VPORT_XSCALE + 5]);
                if (!viewportControl.XScale() && !viewportControl.YScale() && maxX > 0 && maxY > 0)
                {
                    const uint32_t samples = surface.MsaaSamples();
                    const uint32_t columns = uint32_t(std::ceil(std::max(maxX, float(surface.Pitch())))) * (samples >= 2 ? 2u : 1u);
                    const uint32_t rows = uint32_t(std::ceil(maxY)) * (samples >= 1 ? 2u : 1u);
                    const uint32_t tiles = ((columns + 79u) / 80u) * ((rows + 15u) / 16u);
                    endTile = std::min(2048u, depthInfo.BaseTile() + tiles);
                }
            }
            static std::atomic<int> told{ 0 };
            if (told++ < 6) LOGI("renderer: EDRAM depth clear to %g, tiles %u..%u", depth, depthInfo.BaseTile(), endTile);
        }
        {
            // The stencil is replaced only by a clear that has the stencil test on with a REPLACE op
            // (the reference is then what lands there); with it off the stencil bits are untouched.
            const gpu::StencilRefMask stencilRef{ r[gpu::RB_STENCILREFMASK] };
            const bool writesStencil = depthState.StencilEnable() && depthState.StencilZPass() == 2 &&
                                       (stencilRef.WriteMask() & 0xFF) == 0xFF;
            ClearAliasedDepth(depthInfo.BaseTile(), endTile, depth, writesStencil, stencilRef.Reference());
        }
        if (std::abs(depth) < 1e-6f && (r[gpu::RB_STENCILREFMASK] & 0xFF) == 0)
            ClearAliasedColour(depthInfo.BaseTile());
        Skip("EDRAM depth clear");
        return;
    }

    // The colour-and-depth form comes in pairs that describe one row of EDRAM
    // two ways: the bulk of it as a surface half as wide with twice the sample
    // count, the columns left over at the surface's own count, because a tile
    // is eighty samples wide and a surface is not a whole number of tiles. On
    // the console the pair covers the row exactly once.
    //
    // A target here is an image per sample count, so the wide half would land
    // in an image nothing else draws into, and the world surface would be
    // cleared over the sixty-four columns the other half names and nowhere
    // else. Both halves span the same row of EDRAM, and the rectangle carries
    // clip coordinates its own shader normalised by its own pitch, so drawing
    // it into the surface the world uses, with that surface's viewport, puts it
    // over exactly the columns it names on the hardware. That surface is the
    // one with the *lowest* sample count: the idiom doubles the count to halve
    // the pitch.
    uint32_t usePitch = surface.Pitch();
    uint32_t useSamples = surface.MsaaSamples();
    // A row's width in samples: 2x doubles it (MW2's pairs: half the pitch at
    // twice the count), and 4x -- two by two -- doubles it too, its other factor
    // going into the height (see the y term of the window transform below).
    // PES 2017 clears its player-shadow atlas with a 4x half and a 1x remainder;
    // matching 4x by `pitch << 2` never found the 1x surface, so the 4x half
    // cleared an image nothing reads and the atlas kept the menu's pixels, which
    // the shadow test took for occluders: the whole pitch in shadow.
    if (clearRectangles)
    {
        const auto rowOf = [](uint32_t pitch, uint32_t samples) { return pitch << std::min(samples, 1u); };
        const uint32_t row = rowOf(surface.Pitch(), surface.MsaaSamples());
        const uint32_t format = uint32_t(ColourFormatFor(colour.Format()));
        for (const auto& [key, target] : g.targets)
            if (!key.depth && key.baseTile == colour.BaseTile() && key.format == format &&
                key.samples < useSamples && rowOf(key.pitch, key.samples) == row)
            {
                usePitch = key.pitch;
                useSamples = key.samples;
            }
    }

    // A surface has a pitch but no height: how tall it is only shows in the region
    // the draws cover. Clamped, because a scissor left at its reset value asks for
    // a target taller than any display.
    const uint32_t maxTiles = (2048u > colour.BaseTile()) ? (2048u - colour.BaseTile()) : 0u;
    const uint32_t tilesPerRow = (usePitch + 79u) / 80u;
    const uint32_t maxEdramLines = tilesPerRow ? ((maxTiles / tilesPerRow) * 16u) : 2048u;
    // 4096, not 2048: PES 2017 draws its crowd's sprites into a 128x3328 atlas,
    // and every sprite below row 2048 was dropped as outside the target.
    const uint32_t maxAllowedHeight = std::min(4096u, std::max(32u, maxEdramLines));
    const uint32_t width = std::min(usePitch, 2048u);
    const uint32_t height = std::clamp(uint32_t(scissor.offset.y) + scissor.extent.height,
                                       32u, maxAllowedHeight);

    gpu::detail::SetCurrentDrawStage("Draw_EnsureTarget");
    Target* colourTarget = EnsureTarget({ colour.BaseTile(), usePitch,
                                          uint32_t(ColourFormatFor(colour.Format())),
                                          useSamples, false },
                                        width, height, colour.Format());
    Target* depthTarget = EnsureTarget(DepthKey(depthInfo.BaseTile(), uint32_t(g.depthFormat),
                                                useSamples),
                                        width, height, depthInfo.Format());
    if (!colourTarget || !depthTarget) return;

    // In a frame dump, what the stencil buffer holds when a draw that tests it for EQUAL begins: the
    // crowd's composite is one, and it draws only where the billboard passes marked the stencil.
    static int uiStencilDumps = 0;
    static uint64_t uiStencilFrame = UINT64_MAX;
    if (uiStencilFrame != g.frames) { uiStencilFrame = g.frames; uiStencilDumps = 0; }
    // The first few draws of the interface's sprite shader as well: it tests stencil for EQUAL 1.
    const bool uiSprite = call.pixelHash == 0xB401ACD7DE3D19F7ull && uiStencilDumps < 6 && uiStencilDumps++ >= 0;
    const bool crowdShader = uiSprite || call.pixelHash == 0xB4BBEAC4E75DDB31ull || call.pixelHash == 0x70F64A42C2C649AAull ||
                             call.pixelHash == 0xD0C7E9595FC160B9ull;
    if (g_dumpDraws && ((depthState.StencilEnable() && depthState.StencilFunc() == 2) || crowdShader) && depthTarget->image)
    {
        if (g.recording) Submit(true);
        const uint32_t w = HostOf(depthTarget->width, depthTarget->scale);
        const uint32_t h = HostOf(depthTarget->height, depthTarget->scale);
        const std::vector<uint8_t> stencil = ReadColourImage(depthTarget->image,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, w, h, 1, VK_IMAGE_ASPECT_STENCIL_BIT);
        if (!stencil.empty())
        {
            uint64_t counts[256]{};
            for (uint8_t v : stencil) counts[v]++;
            std::string line;
            for (int v = 0; v < 256; v++)
                if (counts[v]) { char t[40]; std::snprintf(t, sizeof t, " %02X:%llu", v, (unsigned long long)counts[v]); line += t; }
            const gpu::StencilRefMask ref{ r[gpu::RB_STENCILREFMASK] };
            LOGW("DUMP   stencil before draw %u (%ux%u, tile %u samples %u; stencil %s func %u ref %02X mask %02X wmask %02X, ops fail %u zpass %u zfail %u):%s",
                 g_dumpIndex - 1, w, h, depthInfo.BaseTile(), unsigned(depthTarget->samples), depthState.StencilEnable() ? "on" : "off",
                 depthState.StencilFunc(), ref.Reference(), ref.Mask(), ref.WriteMask(), depthState.StencilFail(), depthState.StencilZPass(),
                 depthState.StencilZFail(), line.c_str());
#ifdef PES17_ANDROID
            const std::string dir = android::GetPaths().external + "/frame_images";
            mkdir(dir.c_str(), 0775);
            char name[96]; std::snprintf(name, sizeof name, "/stencil_before_%u.pgm", g_dumpIndex - 1);
            if (std::FILE* out = std::fopen((dir + name).c_str(), "wb"))
            {
                std::fprintf(out, "P5\n%u %u\n255\n", w, h);
                std::fwrite(stencil.data(), 1, stencil.size(), out);
                std::fclose(out);
            }
#endif
        }
    }

    if (!BeginFrame()) { Skip("no command buffer"); return; }

    // A colour-and-depth clearing rectangle clears the stencil of the depth image it is drawn into, and
    // -- as the console's one EDRAM buffer would -- of the same tile's images at other sample counts.
    // The interface's own clear (the match intro's team board, the half-time stats) is a stencil-only
    // rectangle, in depth-only mode: depth test and depth write off, stencil ALWAYS with REPLACE -- not a
    // clearRectangles draw, which needs the depth write -- so the crowd's marks in the 1x image outlived
    // it, and the board's sprites, which test the stencil for EQUAL 1, failed over every crowd pixel
    // behind them.
    const bool stencilClearRectangle = call.primitive == 8 && !depthState.ZWrite() &&
                                       depthState.StencilEnable() && depthState.StencilFunc() == 7 &&
                                       !viewportControl.XScale() && !viewportControl.YScale();
    if (((clearRectangles && edramMode != gpu::EdramMode::DepthOnly) || stencilClearRectangle) &&
        depthState.StencilEnable() && depthState.StencilZPass() == 2)
    {
        const gpu::StencilRefMask clearRef{ r[gpu::RB_STENCILREFMASK] };
        if (g_dumpDraws)
        {
            int others = 0;
            for (const auto& [k, t] : g.targets) if (t.depth && k.baseTile == depthInfo.BaseTile() && t.image != depthTarget->image) others++;
            LOGW("DUMP   clearing rectangle with stencil replace: tile %u, wmask %02X, %d other depth image(s) at the tile", depthInfo.BaseTile(), clearRef.WriteMask(), others);
        }
        if ((clearRef.WriteMask() & 0xFF) == 0xFF)
            ClearAliasedStencil(*depthTarget, depthInfo.BaseTile(), clearRef.Reference());
    }

    // Both keys carry the surface's sample count, so the two agree unless a
    // depth image made under another count is reused; a framebuffer cannot mix
    // them.
    if (colourTarget->samples != depthTarget->samples) { Skip("sample counts differ"); return; }
    const uint32_t passWidth = std::min(colourTarget->width, depthTarget->width);
    const uint32_t passHeight = std::min(colourTarget->height, depthTarget->height);
    // What this draw touches of the pass, in the title's pixels and rounded up to
    // 64: the render area it asks for (see State::passAreaWidth).
    uint32_t reachX = std::max(0, scissor.offset.x) + scissor.extent.width;
    uint32_t reachY = std::max(0, scissor.offset.y) + scissor.extent.height;
    if (g.smallPasses && clearRectangles)
    {
        // Only the rectangle's own extent counts: see ClearRectangleReach.
        uint32_t rectX = 0, rectY = 0;
        if (ClearRectangleReach(r, call, rectX, rectY)) { reachX = std::min(reachX, rectX); reachY = std::min(reachY, rectY); }
    }
    const uint32_t touchedWidth = std::min(passWidth, (reachX + 63u) & ~63u);
    const uint32_t touchedHeight = std::min(passHeight, (reachY + 63u) & ~63u);
    const bool sameViews = colourTarget->view == g.currentColour && depthTarget->view == g.currentDepth;
    const bool outgrown = sameViews && g.smallPasses &&
        (touchedWidth > g.passAreaWidth || touchedHeight > g.passAreaHeight);
    if (!sameViews || outgrown)
    {
        gpu::detail::SetCurrentDrawStage("Draw_BeginRendering");
        if (diag::kOn && TracePasses() && DiagnoseHere() && g.passFrames <= TracePasses())
            LOGK("pass: %ux%u colour at tile %u, %ux drawn %ux, mask %08X, depth %08X, mode %u,"
                 " scissor %d,%d %ux%u, after %llu draws; depth tile %u %ux, %llu draws,"
                 " cleared %s",
                 colourTarget->width, colourTarget->height, colour.BaseTile(),
                 1u << surface.MsaaSamples(), unsigned(colourTarget->samples), r[gpu::RB_COLOR_MASK],
                 r[gpu::RB_DEPTHCONTROL], unsigned(edramMode),
                 scissor.offset.x, scissor.offset.y, scissor.extent.width,
                 scissor.extent.height, (unsigned long long)g.drawsRecorded,
                 depthInfo.BaseTile(), unsigned(depthTarget->samples),
                 (unsigned long long)depthTarget->draws,
                 depthTarget->depthClearedIn == g.frames ? "this frame" : "not this frame");
        // A pass that is outgrown is reopened over the whole surface, so one pass breaks
        // at most once for it.
        BeginRendering(*colourTarget, *depthTarget, passWidth, passHeight,
                       outgrown ? passWidth : touchedWidth, outgrown ? passHeight : touchedHeight);
    }

    if (Timing())
        g.setupNanoseconds += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - setupFrom).count());

    // Before the constants, because the highest index in the run is what says how
    // much of the vertex buffer the fetch constants have to bring across.
    uint32_t indexOffset = 0;
    uint32_t lowestVertex = 0;
    uint32_t highestVertex = call.indexCount ? call.indexCount - 1 : 0;
    if (call.indexed)
    {
        if (!call.indexAddress || !call.indexCount) { Skip("indexed draw with no indices"); return; }
        Stopwatch indices(g.indexNanoseconds);
        indexOffset = WriteIndices(call.indexAddress, call.indexCount, call.index32,
                                   lowestVertex, highestVertex);
        if (indexOffset == UINT32_MAX) { Skip("frame arena full"); return; }
    }

    // Three vertices a rectangle, and the fourth corner is written into the arena
    // beside them: without it a clear covers the triangle it names and the rest of
    // the surface keeps what earlier frames left there.
    const uint32_t rectangles =
        (call.primitive == 8 && !call.indexed) ? call.indexCount / 3 : 0;
    // VGT_INDX_OFFSET is added to every vertex index. A batch of sprites in one
    // buffer is drawn with it: one draw a quad, the offset stepping by four, so the
    // draws that name the same fetch constant still read different vertices.
    const uint32_t vertexBias = rectangles ? 0 : (r[0x2102] & 0xFFFFFFu);
    lowestVertex += vertexBias;
    highestVertex += vertexBias;
    if (call.primitive == 8 && !rectangles) { Skip("rectangle list this pass cannot expand"); return; }

    // The match ball: 4020 indices, in the colour pass (vs 5867DE0B52B6EFCE) and in
    // the depth/shadow passes (vs EA544757ADDA275A); found with files/dump_frame.
    // Drawn larger so it reads on a phone's screen.
    const bool isBall = g.ballScale != 1.0f && call.indexCount == 4020 &&
                        (call.vertexHash == 0x5867DE0B52B6EFCEull || call.vertexHash == 0xEA544757ADDA275Aull);
    const ConstantOffsets constants =
        [&] { Stopwatch watch(g.constantsNanoseconds);
              return WriteConstants(r, vertex->translation, pixel->translation,
                                    lowestVertex, highestVertex, rectangles,
                                    isBall ? g.ballScale : 1.0f); }();
    if (!constants.ok) { Skip("frame arena full"); return; }
    // From here to the first command recorded: the pipeline key, the texture
    // descriptor set and the viewport.
    const auto stateFrom = Timing() ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};

    if (rectangles)
    {
        indexOffset = Allocate(rectangles * 6 * 2, kArenaIndices);
        if (indexOffset == UINT32_MAX) { Skip("frame arena full"); return; }
        uint16_t* list = reinterpret_cast<uint16_t*>(g.arenaMapped + indexOffset);
        for (uint32_t rect = 0; rect < rectangles; rect++)
        {
            const uint16_t a = uint16_t(rect * 3), b = uint16_t(rect * 3 + 1);
            const uint16_t c = uint16_t(rect * 3 + 2);
            const uint16_t d = uint16_t(rectangles * 3 + rect);   // the corner made here
            const uint16_t six[6] = { a, b, c, a, c, d };
            std::memcpy(list + rect * 6, six, sizeof six);
        }
    }

    // A quad list (primitive 13) has no Vulkan topology: each four vertices
    // become two triangles, a b c and a c d, through an index list written here.
    // PES 2017 draws its pitch so -- two passes of 13-17 thousand indices before
    // the scene -- and without them the grass was the scene's grey.
    const uint32_t quads = call.primitive == 13 ? call.indexCount / 4 : 0;
    const bool quadsWide = quads && (call.indexed ? call.index32 : call.indexCount > 0xFFFF);
    if (call.primitive == 13)
    {
        if (!quads) { Skip("quad list of fewer than four vertices"); return; }
        const uint32_t size = quadsWide ? 4 : 2;
        const uint32_t expanded = Allocate(quads * 6 * size, kArenaIndices);
        if (expanded == UINT32_MAX) { Skip("frame arena full"); return; }
        // The indices come from guest memory (big-endian, as WriteIndices reads them), not back
        // out of the arena, and the list is built here and copied in once: the arena is
        // write-combined, uncached memory on Mali, where reading it an index at a time and
        // storing two bytes at a time made the crowd's billboards (quad lists of thousands)
        // cost the POCO X7 Pro about 12 ms of CPU a frame, 45 fps against 53 without them.
        const uint8_t* const guestIndices = guest::Base() + call.indexAddress;
        const auto index = [&](uint32_t i) -> uint32_t {
            if (!call.indexed) return i;
            if (call.index32)
            {
                uint32_t value;
                std::memcpy(&value, guestIndices + size_t(i) * 4, 4);
                return __builtin_bswap32(value);
            }
            uint16_t value;
            std::memcpy(&value, guestIndices + size_t(i) * 2, 2);
            return __builtin_bswap16(value);
        };
        static thread_local std::vector<uint32_t> wide;
        static thread_local std::vector<uint16_t> narrow;
        if (quadsWide) wide.resize(size_t(quads) * 6);
        else narrow.resize(size_t(quads) * 6);
        for (uint32_t q = 0; q < quads; q++)
        {
            const uint32_t a = index(q * 4), b = index(q * 4 + 1), c = index(q * 4 + 2), d = index(q * 4 + 3);
            const uint32_t six[6] = { a, b, c, a, c, d };
            for (uint32_t k = 0; k < 6; k++)
            {
                if (quadsWide) wide[size_t(q) * 6 + k] = six[k];
                else narrow[size_t(q) * 6 + k] = uint16_t(six[k]);
            }
        }
        if (quadsWide) std::memcpy(g.arenaMapped + expanded, wide.data(), wide.size() * 4);
        else std::memcpy(g.arenaMapped + expanded, narrow.data(), narrow.size() * 2);
        indexOffset = expanded;
    }

    // PES17_TRACE_SHADER_VERTS=<hex> dumps what a draw actually hands the GPU: the
    // fetch constants after rewriting, the vertices they name, and the first
    // float constants. A sprite the title projects on the CPU carries its clip
    // positions in the vertex data itself, so "why is this quad a fifth of the
    // screen" is answered here, without a capture.
    const bool named = diag::kOn && TracedShaders().Names(call.vertexHash, call.pixelHash);
    if (named) g.vertsMatched++;
    if (named && DiagnoseHere() && g.vertsTraced < 24)
    {
        g.vertsTraced++;
        const auto viewportTerm = [&](uint32_t i) {
            return std::bit_cast<float>(r[gpu::PA_CL_VPORT_XSCALE + i]);
        };
        LOGK("verts: colour tile %u %ux %ux%u, depth tile %u, mask %08X, mode %u,"
             " depthcontrol %08X, %u rectangle(s), viewport scale/offset"
             " x %.1f/%.1f y %.1f/%.1f z %.4f/%.4f, scissor %d,%d %ux%u",
             colour.BaseTile(), 1u << surface.MsaaSamples(),
             colourTarget->width, colourTarget->height, depthInfo.BaseTile(),
             r[gpu::RB_COLOR_MASK], unsigned(edramMode), r[gpu::RB_DEPTHCONTROL],
             rectangles, viewportTerm(0), viewportTerm(1), viewportTerm(2), viewportTerm(3),
             viewportTerm(4), viewportTerm(5),
             scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height);
        if (g.vertsTraced == 1)
            LOGK("verts: the shader itself: %s",
                 shader::Describe(vertex->translation).c_str());
        LOGK("verts: %016llX/%016llX primitive %u, %u %sindices, vertices %u..%u",
             (unsigned long long)call.vertexHash, (unsigned long long)call.pixelHash,
             call.primitive, call.indexCount, call.indexed ? "" : "auto ",
             lowestVertex, highestVertex);
        const float* c = reinterpret_cast<const float*>(g.arenaMapped + constants.vertexFloats);
        for (uint32_t row = 0; row < 8; row++)
            LOGK("verts:   c%-2u %9.4f %9.4f %9.4f %9.4f", row,
                 c[row * 4], c[row * 4 + 1], c[row * 4 + 2], c[row * 4 + 3]);
        const uint32_t* fetch = reinterpret_cast<const uint32_t*>(g.arenaMapped + constants.fetch);
        for (uint32_t slot = 0; slot < 96; slot++)
        {
            const uint32_t* entry = fetch + slot * 4;
            if ((entry[0] & 3) != 3 || !entry[1]) continue;
            const uint32_t stride = vertex->translation.vertexFetchStride[slot];
            LOGK("verts:   fetch slot %u: arena %08X, %u dwords declared, stride %u",
                 slot, entry[0] & ~3u, (entry[1] >> 2) & 0xFFFFFF, stride);
            if (!stride || stride > 32) continue;
            // Past highestVertex by the rectangle count: the fourth corner of
            // each rectangle is written after the vertices the title gave.
            const uint32_t last = highestVertex + rectangles;
            for (uint32_t v = lowestVertex; v <= last && v < lowestVertex + 8; v++)
            {
                const uint32_t at = (entry[0] & ~3u) + v * stride * 4;
                if (at + stride * 4 > g.arenaBytes) break;
                // Hex as well as float: several of a vertex's dwords are packed
                // attributes, which read as floats are nonsense. The copy holds
                // the guest's big-endian bytes.
                const uint32_t* u = reinterpret_cast<const uint32_t*>(g.arenaMapped + at);
                char line[512];
                int n = std::snprintf(line, sizeof line, "verts:     v%-3u", v);
                for (uint32_t w = 0; w < stride && w < 8; w++)
                    n += std::snprintf(line + n, sizeof line - n, " %08X", u[w]);
                n += std::snprintf(line + n, sizeof line - n, "  |");
                for (uint32_t w = 0; w < stride && w < 4; w++)
                    n += std::snprintf(line + n, sizeof line - n, " %11.4f",
                                       std::bit_cast<float>(__builtin_bswap32(u[w])));
                LOGK("%s", line);
            }
        }
    }

    if constexpr (diag::kOn)
    {
        if (!OnlyShaders().hashes.empty() && !OnlyShaders().Names(call.vertexHash, call.pixelHash))
        { Skip("not the shader asked for"); return; }
        if (SkipShaders().Names(call.vertexHash, call.pixelHash))
        { Skip("the shader asked to be skipped"); return; }
    }

    PipelineKey key;
    key.vertexShader = call.vertexHash;
    key.pixelShader = call.pixelHash;
    key.colourFormat = uint32_t(colourTarget->format);
    key.depthFormat = uint32_t(depthTarget->format);
    key.topology = uint32_t(topology);
    // A shader writing nothing and a depth test rejecting everything leave the
    // same surface behind; PES17_NO_DEPTH_TEST tells them apart in one run.
    static const bool noDepth = diag::Flag("PES17_NO_DEPTH_TEST");
    // The depth and stencil tests and the culling are set per draw (below),
    // not built into the pipeline.
    uint32_t depthControl = noDepth ? 0 : r[gpu::RB_DEPTHCONTROL];
    uint32_t modeCntl = r[gpu::PA_SU_SC_MODE_CNTL];
    key.blendControl = r[gpu::RB_BLENDCONTROL0];
    key.colourMask = r[gpu::RB_COLOR_MASK];
    key.samples = colourTarget->samples;
    if (vk::pipeline::StaticStates())
    {
        key.depthControl = depthControl;
        key.modeCntl = modeCntl;
    }
    gpu::detail::SetCurrentDrawStage("Draw_EnsurePipeline");
    bool newPipeline = false;
#ifdef PES17_ANDROID
    VkPipeline built = FindPipeline(key);
    // A draw into a small off-screen surface -- the kits PES 2017 composes once
    // per player during the load, their numbers and names, the crowd's atlas --
    // is not drawn again next frame: skipped while its pipeline compiles, it is
    // lost for good (one player, always the same, played in black). Those are
    // compiled now, waiting for it; full-screen passes still skip a frame
    // rather than stall the game.
    if (!built && colourTarget->width < 640)
    {
        Stopwatch watch(g.pipelineNanoseconds);
        built = EnsurePipeline(key, *vertex, *pixel, &newPipeline);
        if (!built) { Skip("pipeline rejected"); return; }
    }
    if (!built)
    {
        Recorded rec{ key.vertexShader, key.pixelShader,
                      uint32_t(colourTarget->format), uint32_t(depthTarget->format),
                      key.topology, depthControl, key.blendControl,
                      key.colourMask, modeCntl, key.samples };
        EnqueuePipelineWarmup(rec, /*highPriority=*/true);
        Skip("pipeline compiling asynchronously in background");
        return;
    }
#else
    VkPipeline built = [&] { Stopwatch watch(g.pipelineNanoseconds);
                             return EnsurePipeline(key, *vertex, *pixel, &newPipeline); }();
    if (!built) { Skip("pipeline rejected"); return; }
#endif
    // What the background worker built for draws since the last look goes into
    // the shader cache too, so the next run prewarms it.
    {
        static uint32_t tick = 0;
        if ((++tick & 255) == 0 && ShaderCachePath())
        {
            std::vector<Recorded> built = TakeBuiltForDraws();
            if (!built.empty())
            {
                g.recorded.insert(g.recorded.end(), built.begin(), built.end());
                WriteShaderCache();
            }
        }
    }
    if (newPipeline && ShaderCachePath())
    {
        g.recorded.push_back({ key.vertexShader, key.pixelShader,
                               uint32_t(colourTarget->format), uint32_t(depthTarget->format),
                               key.topology, depthControl, key.blendControl,
                               key.colourMask, modeCntl, key.samples });
        static uint32_t s_lastSaved = 0;
        if (g.recorded.size() - s_lastSaved >= 5)
        {
            s_lastSaved = uint32_t(g.recorded.size());
            WriteShaderCache();
        }
    }

    // What each slot is declared as, so a texture of another kind is not bound
    // there. The two stages share the set; where both sample a slot the pixel
    // shader's reading wins, and the title has not been seen to disagree.
    uint8_t kinds[vk::textures::kSlots];
    for (uint32_t s = 0; s < vk::textures::kSlots; s++)
        kinds[s] = (pixel->translation.textureMask & (1u << s))
                       ? pixel->translation.textureKinds[s]
                       : vertex->translation.textureKinds[s];
    uint32_t scaledSlots = 0;
    gpu::detail::SetCurrentDrawStage("Draw_TextureSetFor");
    VkDescriptorSet textureSet = TextureSetFor(
        r, vertex->translation.textureMask | pixel->translation.textureMask, kinds, scaledSlots);
    if (!textureSet) { Skip("no texture descriptor set"); return; }

    VkViewport viewport{};
    ViewportOf(r, scissor, viewport);
    if (g_dumpDraws)
        LOGW("DUMP   scissor %d,%d %ux%u viewport %g,%g %gx%g depth %g..%g",
             scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height,
             viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth);

    if (scissor.offset.x < 0)
    {
        if (int32_t(scissor.extent.width) + scissor.offset.x <= 0) { Skip("scissor off-screen"); return; }
        scissor.extent.width = uint32_t(int32_t(scissor.extent.width) + scissor.offset.x);
        scissor.offset.x = 0;
    }
    if (scissor.offset.y < 0)
    {
        if (int32_t(scissor.extent.height) + scissor.offset.y <= 0) { Skip("scissor off-screen"); return; }
        scissor.extent.height = uint32_t(int32_t(scissor.extent.height) + scissor.offset.y);
        scissor.offset.y = 0;
    }
    // A pass opened over part of the surface draws within it: whatever reaches past was
    // made a bigger pass above (outgrown), except a clearing rectangle's reset scissor.
    const uint32_t clipWidth = g.smallPasses && g.passAreaWidth ? std::min(g.currentWidth, g.passAreaWidth) : g.currentWidth;
    const uint32_t clipHeight = g.smallPasses && g.passAreaHeight ? std::min(g.currentHeight, g.passAreaHeight) : g.currentHeight;
    if (uint32_t(scissor.offset.x) >= clipWidth || uint32_t(scissor.offset.y) >= clipHeight)
    { Skip("scissor outside target"); return; }

    scissor.extent.width = std::min(scissor.extent.width,
                                    clipWidth - uint32_t(scissor.offset.x));
    scissor.extent.height = std::min(scissor.extent.height,
                                     clipHeight - uint32_t(scissor.offset.y));
    if (!scissor.extent.width || !scissor.extent.height) { Skip("empty scissor"); return; }

    State::Bound& bound = g.bound;
    DrawOp op;
    op.layout = static_cast<VkPipelineLayout>(vk::pipeline::Layout());

    // Window coordinates to clip space, which the translated vertex epilogue
    // applies from these four floats. The identity for every ordinary draw.
    //
    // When PA_CL_VTE_CNTL switches the X and Y scale terms off, the program has
    // emitted window coordinates itself and the hardware writes them through
    // unchanged. Vulkan clips against w first, so a vertex at window x = 960 with
    // w = 1 would be thrown out and the primitive with it -- which is how MW2's
    // EDRAM clears came to clear nothing.
    //
    // x is normalised by the pitch the *title* named rather than by the surface
    // this draw may have been redirected to, so the wide half of a clear pair --
    // half the pitch at twice the sample count -- still lands over the EDRAM it
    // names. y and the viewport share the target's height, which makes y an
    // identity.
    {
        float ndc[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
        if (!viewportControl.XScale() && !viewportControl.YScale() &&
            g.currentWidth && g.currentHeight)
        {
            ndc[0] = 2.0f / float(surface.Pitch());
            // A 4x rectangle drawn into the 1x surface: its rows are sample pairs.
            const float rows = (surface.MsaaSamples() == 2 && useSamples < 2) ? 2.0f : 1.0f;
            ndc[1] = 2.0f * rows / float(g.currentHeight);
            ndc[2] = -1.0f;
            ndc[3] = -1.0f;
            // D3D9 puts a pixel's centre on the integer coordinate (see ViewportOf), and a window
            // coordinate here is a pixel's own: the crowd atlas is cleared with a rectangle from
            // 79.5 to 127.5, which should include the pixel centred on 127 but ended exactly on
            // the centre of column 127 as Vulkan has it (127.5, an edge a pixel is not covered
            // past), so the atlas's last column and row stayed transparent black. The billboards
            // sampled it at their quads' edges and drew a black line there: the bars in the stands.
            if (!(r[gpu::PA_SU_VTX_CNTL] & 1))
            {
                ndc[2] += 0.5f * ndc[0];
                ndc[3] += 0.5f * ndc[1];
            }
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = float(g.currentWidth);
            viewport.height = float(g.currentHeight);
        }
        if (!bound.ndcValid || std::memcmp(bound.ndc, ndc, sizeof ndc))
        {
            op.set |= DrawOp::kNdc;
            std::memcpy(op.ndc, ndc, sizeof ndc);
            std::memcpy(bound.ndc, ndc, sizeof ndc);
            bound.ndcValid = true;
        }
    }

    if (Timing())
        g.stateNanoseconds += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - stateFrom).count());

    Stopwatch record(g.recordNanoseconds);
    if (bound.pipeline != built)
    {
        op.set |= DrawOp::kPipeline;
        op.pipeline = built;
        bound.pipeline = built;
    }

    // The title's pixels up to here, the images' from here.
    const float hostScale = float(g.scale) * g.currentScale;
    viewport.x *= hostScale;
    viewport.y *= hostScale;
    viewport.width *= hostScale;
    viewport.height *= hostScale;
    scissor = { Scaled(scissor.offset, g.currentScale), Scaled(scissor.extent, g.currentScale) };
    if (g.currentWidth && g.currentHeight)
    {
        scissor.offset.x = std::max(0, scissor.offset.x);
        scissor.offset.y = std::max(0, scissor.offset.y);
        const int32_t maxX = int32_t(HostOf(clipWidth, g.currentScale));
        const int32_t maxY = int32_t(HostOf(clipHeight, g.currentScale));
        if (scissor.offset.x >= maxX || scissor.offset.y >= maxY)
        {
            scissor.offset = { 0, 0 };
            scissor.extent = { 0, 0 };
        }
        else
        {
            scissor.extent.width = std::min(scissor.extent.width, uint32_t(maxX - scissor.offset.x));
            scissor.extent.height = std::min(scissor.extent.height, uint32_t(maxY - scissor.offset.y));
        }
    }

    // Which slots read a resolve's copy drawn at another size than the title
    // gave it, by how much, and how much smaller this pass is drawn: texture
    // sizes and the pixel position the shader sees are the title's.
    {
        const float factors[2] = { 1.0f / (float(g.scale) * (g.sceneScale)), 1.0f / hostScale };
        if (!bound.scaledValid || bound.scaled != scaledSlots ||
            std::memcmp(bound.scaledFactors, factors, sizeof factors))
        {
            op.set |= DrawOp::kScaled;
            op.scaled = scaledSlots;
            std::memcpy(op.scaledFactors, factors, sizeof factors);
            bound.scaled = scaledSlots;
            std::memcpy(bound.scaledFactors, factors, sizeof factors);
            bound.scaledValid = true;
        }
    }

    if (!bound.viewportValid || std::memcmp(&bound.viewport, &viewport, sizeof viewport))
    {
        op.set |= DrawOp::kViewport;
        op.viewport = viewport;
        bound.viewport = viewport;
        bound.viewportValid = true;
    }

    if (!bound.scissorValid || std::memcmp(&bound.scissor, &scissor, sizeof scissor))
    {
        op.set |= DrawOp::kScissor;
        op.scissor = scissor;
        bound.scissor = scissor;
        bound.scissorValid = true;
    }

    // The guest's polygon offset is Direct3D 9's: an absolute depth offset.
    // Vulkan's constant factor counts steps of the smallest resolvable depth
    // difference instead, so the offset is converted as Xenia does it -- for a
    // float24 guest, in float24 steps (2^21 in the [0.5, 1) range) of 8
    // float32 steps each -- and the slope from 1/16 pixel units to pixels.
    {
        const gpu::ModeCntl mode{ modeCntl };
        // Triangles, fans, strips, quads and polygons; not points, lines or
        // rectangles.
        const bool polygonal = call.primitive < 16 && ((0xE0F0u >> call.primitive) & 1);
        float scale = 0.0f, offset = 0.0f;
        auto asFloat = [&](uint32_t reg) { return std::bit_cast<float>(r[reg]); };
        if (polygonal)
        {
            // Front first: it is the face that is drawn, bar shadow volumes.
            if (mode.PolyOffsetFront() && !mode.CullFront())
            {
                scale = asFloat(gpu::PA_SU_POLY_OFFSET_FRONT_SCALE);
                offset = asFloat(gpu::PA_SU_POLY_OFFSET_FRONT_OFFSET);
                if (scale)
                {
                    // To the nearest order of magnitude, as Xenia does.
                    const double order = std::pow(10.0, std::floor(std::log10(std::fabs(scale))));
                    scale = float(std::round(scale / order) * order);
                }
            }
            if (mode.PolyOffsetBack() && !mode.CullBack() && !scale && !offset)
            {
                scale = asFloat(gpu::PA_SU_POLY_OFFSET_BACK_SCALE);
                offset = asFloat(gpu::PA_SU_POLY_OFFSET_BACK_OFFSET);
            }
        }
        else if (mode.PolyOffsetPara())
        {
            scale = asFloat(gpu::PA_SU_POLY_OFFSET_FRONT_SCALE);
            offset = asFloat(gpu::PA_SU_POLY_OFFSET_FRONT_OFFSET);
        }
        const float bias[2] = {
            offset * (depthInfo.Format() ? float(1u << 24) : float((1u << 24) - 1)),
            // A slope is depth per pixel, and the images' pixels are smaller
            // than the title's.
            scale * (1.0f / 16.0f) * float(g.scale) * g.currentScale,
        };
        if (!bound.depthBiasValid || std::memcmp(bound.depthBias, bias, sizeof bias))
        {
            op.set |= DrawOp::kDepthBias;
            std::memcpy(op.depthBias, bias, sizeof bias);
            std::memcpy(bound.depthBias, bias, sizeof bias);
            bound.depthBiasValid = true;
        }
    }

    // Culling and winding, and the depth and stencil tests: dynamic state, so
    // one pipeline serves every combination the title draws a pair of shaders
    // with, and a shader compiled on its own fits them all.
    if (!vk::pipeline::StaticStates() && (!bound.facesValid || bound.modeCntl != modeCntl))
    {
        op.set |= DrawOp::kFaces;
        FacesFor(modeCntl, op.cullMode, op.frontFace);
        bound.modeCntl = modeCntl;
        bound.facesValid = true;
    }
    if (!vk::pipeline::StaticStates() && (!bound.depthTestsValid || bound.depthControl != depthControl))
    {
        op.set |= DrawOp::kDepthTests;
        op.depthTests = DepthTestsFor(depthControl);
        bound.depthControl = depthControl;
        bound.depthTestsValid = true;
    }

    // The stencil reference and masks. Every pipeline declares them dynamic, so
    // they are set whether this draw tests stencil or not.
    {
        const uint32_t front = r[gpu::RB_STENCILREFMASK];
        const uint32_t stencil[2] = { front, depthState.BackfaceEnable()
                                                 ? r[gpu::RB_STENCILREFMASK_BF] : front };
        if (!bound.stencilValid || std::memcmp(bound.stencil, stencil, sizeof stencil))
        {
            op.set |= DrawOp::kStencil;
            std::memcpy(op.stencil, stencil, sizeof stencil);
            std::memcpy(bound.stencil, stencil, sizeof stencil);
            bound.stencilValid = true;
        }
    }

    // In binding order, which is the order the dynamic offsets are read in.
    const uint32_t dynamic[bindings::kConstantBlocks] = {
        constants.vertexFloats, constants.fetch, constants.booleans, constants.loops,
        constants.pixelFloats };
    if (!bound.dynamicValid || std::memcmp(bound.dynamic, dynamic, sizeof dynamic))
    {
        op.set |= DrawOp::kConstants;
        op.constantSet = g.constantSet;
        std::memcpy(op.dynamic, dynamic, sizeof dynamic);
        std::memcpy(bound.dynamic, dynamic, sizeof dynamic);
        bound.dynamicValid = true;
    }

    if (bound.textureSet != textureSet)
    {
        op.set |= DrawOp::kTextures;
        op.textureSet = textureSet;
        bound.textureSet = textureSet;
    }
    // Set 2 is bound once in BeginFrame; it is the same for every draw.

    // The alpha test. Vulkan has no fixed-function form, so the translated pixel
    // shader does the comparison and these are what it compares with; `always`
    // when the title has the test off, which the shader's mask makes free.
    //
    // Also `always` in depth-only mode, where the console does not run the pixel
    // shader at all (Xenia's reading too), so nothing it computes can discard.
    // The sun shadow casters depend on it: their technique has no pixel shader,
    // for which D3D9 loads nothing, and the draws run under whatever shader the
    // command stream loaded last.
    {
        const bool pixelShaderRuns = edramMode != gpu::EdramMode::DepthOnly;
        const gpu::ColorControl control{ r[gpu::RB_COLORCONTROL] };
        // Two to the power of the target's exponent bias, built by moving the
        // exponent of 1.0f rather than by calling powf on the draw path.
        const int32_t bias = std::clamp(colour.ExpBias(), -31, 31);
        const float scale = std::bit_cast<float>(uint32_t(0x3F800000) + (bias << 23));
        const bool gammaTarget = colour.Format() == 1;
        struct { uint32_t function; float reference; float scale; uint32_t gamma; } test{
            (control.AlphaTest() && pixelShaderRuns) ? control.AlphaFunc() : 7u,
            std::bit_cast<float>(r[gpu::RB_ALPHA_REF]),
            scale,
            // Bit 0: the target is 8_8_8_8_GAMMA. Bits 8-15: the param gen
            // register plus one (SQ_PROGRAM_CNTL bit 18, SQ_CONTEXT_MISC 8-15).
            (gammaTarget ? 1u : 0u) |
                (((r[gpu::SQ_PROGRAM_CNTL] >> 18) & 1)
                     ? ((((r[gpu::SQ_PROGRAM_CNTL + 1] >> 8) & 0xFF) + 1) & 0xFF) << 8 : 0u) };
        static_assert(sizeof test == sizeof g.bound.push);
        if (!bound.pushValid || std::memcmp(bound.push, &test, sizeof test))
        {
            op.set |= DrawOp::kAlphaTest;
            std::memcpy(op.push, &test, sizeof test);
            std::memcpy(bound.push, &test, sizeof test);
            bound.pushValid = true;
        }
    }

    if (call.indexed || rectangles || quads)
    {
        // The whole arena is bound once per index size and each draw names
        // where its indices start: a new binding every draw made the driver
        // emit it again every draw. The arena allocator's alignment keeps
        // every offset a whole number of indices.
        const bool wide = quads ? quadsWide : call.indexed && call.index32;
        const int indexType = wide ? int(VK_INDEX_TYPE_UINT32) : int(VK_INDEX_TYPE_UINT16);
        if (bound.indexBuffer != g.arena || bound.indexOffset != 0 ||
            bound.indexType != indexType)
        {
            op.set |= DrawOp::kIndexBuffer;
            op.indexBuffer = g.arena;
            op.indexType = VkIndexType(indexType);
            bound.indexBuffer = g.arena;
            bound.indexOffset = 0;
            bound.indexType = indexType;
        }
        op.set |= DrawOp::kIndexed;
        op.count = rectangles ? rectangles * 6 : quads ? quads * 6 : call.indexCount;
        op.firstIndex = indexOffset / (wide ? 4 : 2);
    }
    else op.count = call.indexCount;
    op.vertexOffset = int32_t(vertexBias);
    gpu::detail::SetCurrentDrawStage("Draw_Record");
    Record(op);
    colourTarget->draws++;
    // The depth target too, so a dump can tell a depth buffer some pass is
    // testing against from one no pass has ever bound.
    depthTarget->draws++;
    if (edramMode == gpu::EdramMode::ColorDepth && key.colourMask &&
        !colourTarget->colourDrawsThisFrame++)
        colourTarget->framesWithColour++;
    g.drawsRecorded++;
    g.segmentDraws = true;
    gpu::detail::SetCurrentDrawStage("idle");
}

#endif  // PES17_HAVE_VULKAN
