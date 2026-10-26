// The renderer: the title's draws and resolves, recorded into Vulkan. This
// file holds its state and brings it up and down; renderer_state.h lists the
// parts.
#include "renderer.h"
#include "../../diagnostics.h"
#include <unistd.h>

#ifndef PES17_HAVE_VULKAN
bool vk::renderer::Initialise() { return false; }
void vk::renderer::Shutdown() {}
bool vk::renderer::Ready() { return false; }
void vk::renderer::SetThermalStatus(int) {}
void vk::renderer::Draw(const gpu::RegisterFile&, const DrawCall&) {}
void vk::renderer::Resolve(const gpu::RegisterFile&) {}
void vk::renderer::Swap(uint32_t) {}
void vk::renderer::BeginOcclusionQuery() {}
bool vk::renderer::EndOcclusionQuery(uint32_t, uint64_t& samples) { samples = 0; return true; }
void vk::renderer::CollectOcclusionQueries(void (*)(uint32_t, uint64_t)) {}
void vk::renderer::Report() {}
void vk::renderer::FlashSeen(const char*) {}
uint64_t vk::renderer::CompletedSubmission() { return 0; }
void vk::renderer::BeforeStreamWrite(uint32_t, uint32_t) {}
void vk::renderer::BeforeCompletion() {}
void vk::renderer::PublishWriteBacks() {}
bool vk::renderer::DumpingDraws() { return false; }
#else

#include "renderer_state.h"
#include "../../frame_rate.h"
#ifdef PES17_ANDROID
#include "../../android/android.h"
#endif
#include "display_table.h"
#include "texture_cache.h"
#include "../memory_watch.h"
#include "../../guest_memory.h"
#include "../../crash.h"
#include "../../log.h"

#include <algorithm>
#include <cmath>

namespace vk::renderer::detail
{
    State g;
}

using namespace vk::renderer::detail;

static void RollbackInitialise()
{
    if (!g.device) return;
    for (uint32_t i = 0; i < kPresentImages; i++)
    {
        if (g.present[i]) vkDestroyImage(g.device, g.present[i], nullptr);
        if (g.presentMemory[i]) vkFreeMemory(g.device, g.presentMemory[i], nullptr);
        g.present[i] = VK_NULL_HANDLE;
        g.presentMemory[i] = VK_NULL_HANDLE;
    }
    if (g.descriptors)
    {
        vkDestroyDescriptorPool(g.device, g.descriptors, nullptr);
        g.descriptors = VK_NULL_HANDLE;
    }
    g.constantSet = VK_NULL_HANDLE;
    g.memorySet = VK_NULL_HANDLE;

    gpu::watch::SetShadow(nullptr, 0);
    g.shadowBase = 0;
    if (g.arenaMapped)
    {
        vkUnmapMemory(g.device, g.arenaMemory);
        g.arenaMapped = nullptr;
    }
    if (g.arena)
    {
        vkDestroyBuffer(g.device, g.arena, nullptr);
        g.arena = VK_NULL_HANDLE;
    }
    if (g.arenaMemory)
    {
        vkFreeMemory(g.device, g.arenaMemory, nullptr);
        g.arenaMemory = VK_NULL_HANDLE;
    }
    g.arenaBytes = 0;

    if (g.setupFence)
    {
        vkDestroyFence(g.device, g.setupFence, nullptr);
        g.setupFence = VK_NULL_HANDLE;
    }
    for (State::Slot& slot : g.slots)
    {
        if (slot.fence)
        {
            vkDestroyFence(g.device, slot.fence, nullptr);
            slot.fence = VK_NULL_HANDLE;
        }
    }
    if (g.guestQueries)
    {
        vkDestroyQueryPool(g.device, g.guestQueries, nullptr);
        g.guestQueries = VK_NULL_HANDLE;
    }
    if (g.setupCommands)
    {
        vkDestroyCommandPool(g.device, g.setupCommands, nullptr);
        g.setupCommands = VK_NULL_HANDLE;
    }
    g.setup = VK_NULL_HANDLE;
    for (State::Slot& slot : g.slots)
    {
        if (slot.commands)
        {
            vkDestroyCommandPool(g.device, slot.commands, nullptr);
            slot.commands = VK_NULL_HANDLE;
        }
        slot = State::Slot{};
    }
    vk::textures::Shutdown();
    g.presentWidth = 1280;
    g.presentHeight = 720;
    g.scale = 1;
    g.depthFormat = VK_FORMAT_UNDEFINED;
    g.command = VK_NULL_HANDLE;
    g.ready = false;
    g.device = VK_NULL_HANDLE;
    g.physical = VK_NULL_HANDLE;
    g.queue = VK_NULL_HANDLE;
}

bool vk::renderer::Initialise()
{
    if (g.ready) return true;
    if (!vk::pipeline::Ready() && !vk::pipeline::Initialise()) return false;

    g.device = static_cast<VkDevice>(vk::pipeline::Device());
    g.physical = static_cast<VkPhysicalDevice>(vk::pipeline::PhysicalDevice());
    g.queue = static_cast<VkQueue>(vk::pipeline::Queue());
    if (!g.device || !g.queue) { RollbackInitialise(); return false; }
    if (!vk::textures::Initialise()) { RollbackInitialise(); return false; }

    vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(g.physical, &properties);
    g.uniformAlignment = uint32_t(std::max<VkDeviceSize>(
        std::max(properties.limits.minUniformBufferOffsetAlignment,
                 properties.limits.minStorageBufferOffsetAlignment), 256));

    // AMD has no D24_UNORM_S8_UINT, so the depth format is whichever 24-bit-ish
    // combined format the device actually has.
    for (VkFormat candidate : { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT })
    {
        VkFormatProperties format{};
        vkGetPhysicalDeviceFormatProperties(g.physical, candidate, &format);
        if (format.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            g.depthFormat = candidate;
            break;
        }
    }
    if (g.depthFormat == VK_FORMAT_UNDEFINED) { LOGW("renderer: no depth format"); RollbackInitialise(); return false; }

    // The console's 2x and 4x surfaces are drawn with that many samples unless
    // PES17_NO_MSAA=1. A count is usable when colour, depth and stencil can all
    // be drawn at it; and a multisampled depth target can only be read back
    // through a render pass resolve, which Vulkan 1.2 has core (or
    // VK_KHR_depth_stencil_resolve before that) and has to take the first
    // sample -- the only mode every such device is required to offer.
    {
#ifdef PES17_ANDROID
        // files/no_msaa: the same, for measuring without changing the app's setting.
        const bool offByFile = access((android::GetPaths().external + "/no_msaa").c_str(), F_OK) == 0;
#else
        const bool offByFile = false;
#endif
        const bool off = env::Flag("PES17_NO_MSAA") || offByFile;
        g.sampleCounts = properties.limits.framebufferColorSampleCounts &
                         properties.limits.framebufferDepthSampleCounts &
                         properties.limits.framebufferStencilSampleCounts;
        VkPhysicalDeviceDepthStencilResolveProperties resolve{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES };
        VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties2.pNext = &resolve;
        const bool modern = properties.apiVersion >= VK_API_VERSION_1_2;
        if (modern || vk::pipeline::DeviceHasExtension(g.physical, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME))
            vkGetPhysicalDeviceProperties2(g.physical, &properties2);
        const bool depthResolve =
            (modern || vk::pipeline::DeviceHasExtension(g.physical, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME)) &&
            (resolve.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
        g.msaa = !off && depthResolve && (g.sampleCounts & ~VkSampleCountFlags(VK_SAMPLE_COUNT_1_BIT));
        if (g.msaa)
            LOGI("renderer: multisampling on: the device draws at%s%s%s%s, the title's 2x and 4x"
                 " surfaces are drawn at %ux and %ux%s",
                 (g.sampleCounts & VK_SAMPLE_COUNT_2_BIT) ? " 2x" : "",
                 (g.sampleCounts & VK_SAMPLE_COUNT_4_BIT) ? " 4x" : "",
                 (g.sampleCounts & VK_SAMPLE_COUNT_8_BIT) ? " 8x" : "",
                 (g.sampleCounts & VK_SAMPLE_COUNT_16_BIT) ? " 16x" : "",
                 unsigned(HostSamples(1)), unsigned(HostSamples(2)),
                 env::Text("PES17_MSAA") ? " (PES17_MSAA)" : "");
        else
            LOGI("renderer: multisampling off (%s): the title's 2x and 4x surfaces are drawn at 1x",
                 off ? "PES17_NO_MSAA" : !modern && !vk::pipeline::DeviceHasExtension(g.physical, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) ? "the device is Vulkan 1.1 without the depth resolve extension"
                     : !depthResolve ? "no first-sample depth resolve"
                     : "the device draws at 1x only");
    }

    // PES17_SCALE=2 or 3 draws everything that many times wider and taller. A
    // surface is at most 4096 of the title's pixels either way, and the scale
    // is lowered until that fits an image of the device's.
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(g.physical, &properties);
        const uint32_t asked = uint32_t(std::clamp<uint64_t>(env::Number("PES17_SCALE", 1), 1, 3));
        g.scale = asked;
        while (g.scale > 1 && 4096 * g.scale > properties.limits.maxImageDimension2D) g.scale--;
        g.presentWidth *= g.scale;
        g.presentHeight *= g.scale;
        if (g.scale != asked)
            LOGW("renderer: PES17_SCALE=%u is more than the device's %u-pixel images hold",
                 asked, properties.limits.maxImageDimension2D);
        if (g.scale > 1)
            LOGI("renderer: drawing at %ux the title's sizes, a %ux%u frame", g.scale,
                 g.presentWidth, g.presentHeight);
        // PES17_BALL_SCALE: the ball model drawn this many times larger (1 = off).
        double ballAsked = env::Real("PES17_BALL_SCALE", 1.0);
#ifdef PES17_ANDROID
        // files/ball_scale overrides it, for debugging (a huge ball shows whether the draw is the right one).
        if (FILE* f = std::fopen((android::GetPaths().external + "/ball_scale").c_str(), "r"))
        {
            double value = 0;
            if (std::fscanf(f, "%lf", &value) == 1 && value > 0) ballAsked = value;
            std::fclose(f);
        }
#endif
        g.ballScale = float(std::clamp(ballAsked, 1.0, 10.0));
        if (g.ballScale != 1.0f) LOGI("renderer: the ball is drawn %.2gx larger", g.ballScale);
        // PES17_SCENE_SCALE: the full-frame surfaces drawn smaller (renderer_state.h
        // SceneScaleFor). Rounded to a sixteenth, so 1280 lands on whole pixels.
        g.sceneScale = 1.0f;
        if (g.scale == 1)
        {
            double asked = env::Real("PES17_SCENE_SCALE", 1.0);
#ifdef PES17_ANDROID
            // files/scene_scale overrides it, for measuring.
            if (FILE* f = std::fopen((android::GetPaths().external + "/scene_scale").c_str(), "r"))
            {
                double value = 0;
                if (std::fscanf(f, "%lf", &value) == 1 && value > 0) asked = value;
                std::fclose(f);
            }
#endif
            asked = std::clamp(asked, 0.5, 1.0);
            g.sceneScale = float(std::round(asked * 16.0) / 16.0);
            if (g.sceneScale < 1.0f)
                LOGI("renderer: the scene is drawn at %.4g of 1280x720 (%ux%u)", g.sceneScale,
                     HostOf(1280, g.sceneScale), HostOf(720, g.sceneScale));

            // PES17_HEAT_SCALE and PES17_LOAD_SCALE (PES17_DYNAMIC_SCALE is both): the scale the
            // player chose is the most it will be, and half of 1280x720 the least. The GPU time the
            // load rule keeps the passes under is a share of one frame at the rate the title is
            // held to.
            const bool both = env::Flag("PES17_DYNAMIC_SCALE");
            g.heatScale = both || env::Flag("PES17_HEAT_SCALE");
            g.loadScale = both || env::Flag("PES17_LOAD_SCALE");
            g.dynamicScale = g.heatScale || g.loadScale;
            if (g.dynamicScale)
            {
                const uint32_t limit = frame_rate::Limit();
                const double period = 1000.0 / double(limit ? std::min(limit, 240u) : 60u);
                g.dynScale.Configure(int(std::lround(g.sceneScale * 16.0f)), 8, period * 0.78);
                g.dynScale.SetRules(g.heatScale, g.loadScale);
                g.dynScale.SetThermal(g.thermalStatus.load());
                LOGI("renderer: the scene scale moves with %s%s%s: %u/16 to %u/16, passes under %.1f ms",
                     g.heatScale ? "the phone's heat" : "", g.heatScale && g.loadScale ? " and " : "",
                     g.loadScale ? "the GPU time" : "", unsigned(g.dynScale.Floor()),
                     unsigned(g.dynScale.Ceiling()), g.dynScale.Budget());
            }
        }
    }

    VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.queueFamilyIndex = vk::pipeline::QueueFamily();
    VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    // A pool each, because a slot is reset on its own while the others are busy.
    for (State::Slot& slot : g.slots)
    {
        if (vkCreateCommandPool(g.device, &pool, nullptr, &slot.commands) != VK_SUCCESS)
        {
            RollbackInitialise();
            return false;
        }
        allocate.commandPool = slot.commands;
        allocate.commandBufferCount = kSegmentBuffers;
        slot.buffers.resize(kSegmentBuffers);
        if (vkAllocateCommandBuffers(g.device, &allocate, slot.buffers.data()) != VK_SUCCESS)
        {
            RollbackInitialise();
            return false;
        }
        allocate.commandBufferCount = 1;
    }

    VkCommandPoolCreateInfo setupPool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    setupPool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    setupPool.queueFamilyIndex = vk::pipeline::QueueFamily();
    if (vkCreateCommandPool(g.device, &setupPool, nullptr, &g.setupCommands) != VK_SUCCESS)
    {
        RollbackInitialise();
        return false;
    }
    allocate.commandPool = g.setupCommands;
    if (vkAllocateCommandBuffers(g.device, &allocate, &g.setup) != VK_SUCCESS)
    {
        RollbackInitialise();
        return false;
    }

    VkQueryPoolCreateInfo queries{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    queries.queryType = VK_QUERY_TYPE_OCCLUSION;
    queries.queryCount = kGuestQueryRing;
    if (vkCreateQueryPool(g.device, &queries, nullptr, &g.guestQueries) != VK_SUCCESS)
        g.guestQueries = VK_NULL_HANDLE;

    // Render passes opened over what their draws touch, not the whole 2048-row surface (a
    // clearing rectangle is sized from its own corners): files/small_passes or
    // PES17_SMALL_PASSES=1. Off by default. It saved about 20% of pass GPU time but not a measurable
    // frame rate, and it clipped the crowd's data passes to a rectangle (docs/performance.md).
    g.smallPasses = env::Flag("PES17_SMALL_PASSES");
#ifdef PES17_ANDROID
    if (access((android::GetPaths().external + "/small_passes").c_str(), F_OK) == 0) g.smallPasses = true;
#endif
    LOGI("renderer: render passes are opened over %s", g.smallPasses ? "the area their draws touch" : "the whole surface");

    // GPU timing of the render passes (files/gpu_timing).
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(g.physical, &props);
        g.timestampPeriod = props.limits.timestampPeriod;
        bool wanted = env::Flag("PES17_GPU_TIMING");
#ifdef PES17_ANDROID
        if (!wanted) wanted = access((android::GetPaths().external + "/gpu_timing").c_str(), F_OK) == 0;
#endif
        g.timingReport = wanted;
        // The load rule reads the passes' times, so it needs them taken.
        if (g.loadScale) wanted = true;
        if (wanted && props.limits.timestampComputeAndGraphics)
        {
            VkQueryPoolCreateInfo timestamps{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            timestamps.queryType = VK_QUERY_TYPE_TIMESTAMP;
            timestamps.queryCount = 2 * State::kTimedPasses;
            g.timingOn = true;
            for (auto& timing : g.passTiming)
                if (vkCreateQueryPool(g.device, &timestamps, nullptr, &timing.pool) != VK_SUCCESS)
                    { timing.pool = VK_NULL_HANDLE; g.timingOn = false; }
            if (g.timingReport) LOGW("renderer: GPU timing of the render passes is on (%.2f ns a tick)", g.timestampPeriod);
        }
        if (g.loadScale && !g.timingOn)
        {
            LOGW("renderer: the device cannot timestamp render passes, so the scene scale cannot follow the GPU time");
            g.loadScale = false;
            g.dynScale.SetRules(g.heatScale, false);
            g.dynamicScale = g.heatScale;
        }
    }

    VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (State::Slot& slot : g.slots)
        if (vkCreateFence(g.device, &fence, nullptr, &slot.fence) != VK_SUCCESS)
        {
            RollbackInitialise();
            return false;
        }
    if (vkCreateFence(g.device, &fence, nullptr, &g.setupFence) != VK_SUCCESS)
    {
        RollbackInitialise();
        return false;
    }

    // Constants, vertex data and indices all live in the arena, which is why
    // it carries every usage at once.
    // Ask for the whole thing and settle for what the device will give: on a
    // shared-memory part this is system RAM, and refusing to start because the
    // largest size did not fit would be the wrong answer.
    //
    // Behind the arena, when there is room, the shadow of guest physical
    // memory: vertex data a draw reads from pages the title has not written
    // since they were copied is read there instead of being copied again (see
    // gpu/memory_watch.h). One buffer, so set 2 reaches both and a fetch
    // constant says which by its address alone.
    // Read through env::, not diag::, because the shadow is 512 MB of resident
    // memory and a release build cannot see a diagnostics switch (diag::Flag is
    // a constant false there): on the device whose memory is shortest the
    // switch would have done nothing at all.
    static const bool noShadow = env::Flag("PES17_NO_SHADOW");
    const uint32_t shadowBytes = guest::kPhysicalEnd - guest::kPhysicalBase;
    uint32_t wanted = uint32_t(env::Number("PES17_ARENA_MB", kArenaWanted >> 20)) << 20;
    bool withShadow = !noShadow;
    // The arena (and the shadow behind it) is one storage buffer bound whole, and a shader reads nothing
    // past maxStorageBufferRange of it: what lies beyond reads as zero. A frame slot there would be a frame
    // of zeros, so the arena itself is never larger than the device's range (the shadow behind it may run
    // past: the S20 FE's range is 512 MB against 256 + 512, and it draws correctly). PES17_IGNORE_STORAGE_RANGE=1
    // skips the check.
    const VkDeviceSize storageRange = env::Flag("PES17_IGNORE_STORAGE_RANGE") ? ~VkDeviceSize(0) : properties.limits.maxStorageBufferRange;
    for (; wanted >= kArenaLeast; withShadow ? (void)(withShadow = false) : (void)(wanted /= 2))
    {
        VkBufferCreateInfo buffer{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer.size = VkDeviceSize(wanted) + (withShadow ? shadowBytes : 0);
        if (VkDeviceSize(wanted) > storageRange) continue;
        buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (vkCreateBuffer(g.device, &buffer, nullptr, &g.arena) != VK_SUCCESS) continue;
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(g.device, g.arena, &needs);
        const uint32_t type = FindMemory(needs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) { LOGW("renderer: no host-visible memory"); RollbackInitialise(); return false; }
        VkMemoryAllocateInfo allocateArena{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateArena.allocationSize = needs.size;
        allocateArena.memoryTypeIndex = type;
        if (vkAllocateMemory(g.device, &allocateArena, nullptr, &g.arenaMemory) == VK_SUCCESS &&
            vkBindBufferMemory(g.device, g.arena, g.arenaMemory, 0) == VK_SUCCESS &&
            vkMapMemory(g.device, g.arenaMemory, 0, buffer.size, 0,
                        reinterpret_cast<void**>(&g.arenaMapped)) == VK_SUCCESS)
        {
            g.arenaBytes = wanted;
            if (withShadow)
            {
                g.shadowBase = wanted;
                gpu::watch::SetShadow(g.arenaMapped + wanted, shadowBytes);
            }
            break;
        }
        if (g.arenaMemory) vkFreeMemory(g.device, g.arenaMemory, nullptr);
        vkDestroyBuffer(g.device, g.arena, nullptr);
        g.arenaMemory = VK_NULL_HANDLE;
        g.arena = VK_NULL_HANDLE;
    }
    if (!g.arenaBytes) { LOGW("renderer: no frame arena"); RollbackInitialise(); return false; }

    // A slot's share has to hold everything one submission copies -- a frame's
    // is about 50 MB here against the 512 MB asked for, so three shares fit with
    // room to spare. An overflow is reported rather than silently wrapping.
    // A constant window is copied at the length its shader reads, but its
    // descriptor range is the full 256 vec4 either way, and Vulkan requires the
    // offset plus that range to stay inside the buffer. So every slot stops
    // 4 KB short of where its share ends.
    constexpr uint32_t kWindowHeadroom = bindings::kBlockBytes[bindings::kVertexFloats];
    const uint32_t share = (g.arenaBytes / kFrameSlots) & ~(g.uniformAlignment - 1);
    g.arenaSlotBytes = share - kWindowHeadroom;
    for (uint32_t i = 0; i < kFrameSlots; i++) g.slots[i].arenaBase = i * share;
    g.command = g.slots[g.slot].buffers[0];
    g.arenaBase = g.slots[g.slot].arenaBase;
    LOGI("renderer: %u frame slots of %u KB of arena each", kFrameSlots, g.arenaSlotBytes / 1024);
    LOGI("renderer: arena %u MB, shadow %s, buffer %llu MB against maxStorageBufferRange %llu MB",
         g.arenaBytes >> 20, g.shadowBase ? "on" : "off",
         (unsigned long long)((VkDeviceSize(g.arenaBytes) + (g.shadowBase ? shadowBytes : 0)) >> 20),
         (unsigned long long)(properties.limits.maxStorageBufferRange >> 20));

    // Before any command is recorded: the recorder thread only reads these.
    // On Vulkan 1.3 core devices (Mali, Tensor, etc.), the commands may be exported
    // without the KHR / EXT suffix. Check both.
    const auto command = [](const char* name, const char* fallback = nullptr) {
        PFN_vkVoidFunction ptr = vkGetDeviceProcAddr(g.device, name);
        if (!ptr && fallback)
            ptr = vkGetDeviceProcAddr(g.device, fallback);
        return ptr;
    };
    dispatch.beginRendering = PFN_vkCmdBeginRenderingKHR(command("vkCmdBeginRenderingKHR", "vkCmdBeginRendering"));
    dispatch.endRendering = PFN_vkCmdEndRenderingKHR(command("vkCmdEndRenderingKHR", "vkCmdEndRendering"));
    dispatch.setCullMode = PFN_vkCmdSetCullModeEXT(command("vkCmdSetCullModeEXT", "vkCmdSetCullMode"));
    dispatch.setFrontFace = PFN_vkCmdSetFrontFaceEXT(command("vkCmdSetFrontFaceEXT", "vkCmdSetFrontFace"));
    dispatch.setDepthTestEnable = PFN_vkCmdSetDepthTestEnableEXT(command("vkCmdSetDepthTestEnableEXT", "vkCmdSetDepthTestEnable"));
    dispatch.setDepthWriteEnable = PFN_vkCmdSetDepthWriteEnableEXT(command("vkCmdSetDepthWriteEnableEXT", "vkCmdSetDepthWriteEnable"));
    dispatch.setDepthCompareOp = PFN_vkCmdSetDepthCompareOpEXT(command("vkCmdSetDepthCompareOpEXT", "vkCmdSetDepthCompareOp"));
    dispatch.setStencilTestEnable = PFN_vkCmdSetStencilTestEnableEXT(command("vkCmdSetStencilTestEnableEXT", "vkCmdSetStencilTestEnable"));
    dispatch.setStencilOp = PFN_vkCmdSetStencilOpEXT(command("vkCmdSetStencilOpEXT", "vkCmdSetStencilOp"));
    if (!dispatch.beginRendering || !dispatch.endRendering || !dispatch.setCullMode ||
        !dispatch.setStencilOp)
    {
        if (!vk::pipeline::LegacyMode())
        {
            LOGW("renderer: the device did not give dynamic rendering or dynamic state commands");
            RollbackInitialise();
            return false;
        }
        LOGI("renderer: running in legacy mode without dynamic rendering / dynamic state commands");
    }

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sizes[0].descriptorCount = bindings::kConstantBlocks;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo descriptors{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    descriptors.maxSets = 2;
    descriptors.poolSizeCount = 2;
    descriptors.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(g.device, &descriptors, nullptr, &g.descriptors) != VK_SUCCESS)
    {
        RollbackInitialise();
        return false;
    }

    VkDescriptorSetLayout constantLayout =
        static_cast<VkDescriptorSetLayout>(vk::pipeline::SetLayout(bindings::kConstantSet));
    VkDescriptorSetLayout memoryLayout =
        static_cast<VkDescriptorSetLayout>(vk::pipeline::SetLayout(bindings::kArenaSet));
    VkDescriptorSetAllocateInfo allocateSet{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocateSet.descriptorPool = g.descriptors;
    allocateSet.descriptorSetCount = 1;
    allocateSet.pSetLayouts = &constantLayout;
    if (vkAllocateDescriptorSets(g.device, &allocateSet, &g.constantSet) != VK_SUCCESS)
    {
        RollbackInitialise();
        return false;
    }
    allocateSet.pSetLayouts = &memoryLayout;
    if (vkAllocateDescriptorSets(g.device, &allocateSet, &g.memorySet) != VK_SUCCESS)
    {
        RollbackInitialise();
        return false;
    }

    // Dynamic views into the arena; their ranges are the block sizes, and
    // every draw supplies its own offsets.
    constexpr uint32_t kBlocks = bindings::kConstantBlocks;
    VkDescriptorBufferInfo buffers[kBlocks + 1]{};
    VkWriteDescriptorSet writes[kBlocks + 1]{};
    for (uint32_t i = 0; i < kBlocks; i++)
    {
        buffers[i] = { g.arena, 0, bindings::kBlockBytes[i] };
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g.constantSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].pBufferInfo = &buffers[i];
    }
    buffers[kBlocks] = { g.arena, 0, VK_WHOLE_SIZE };
    writes[kBlocks].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[kBlocks].dstSet = g.memorySet;
    writes[kBlocks].dstBinding = bindings::kArenaBinding;
    writes[kBlocks].descriptorCount = 1;
    writes[kBlocks].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[kBlocks].pBufferInfo = &buffers[kBlocks];
    vkUpdateDescriptorSets(g.device, kBlocks + 1, writes, 0, nullptr);

    for (uint32_t i = 0; i < kPresentImages; i++)
    {
        Target image;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.width = g.presentWidth / g.scale;     // MakeImage scales
        image.height = g.presentHeight / g.scale;
        // STORAGE so the display's colour table can be applied to it in place; SAMPLED so the
        // window's upscaler (upscaler.cpp) can read it.
        if (!MakeImage(image, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0))
        {
            RollbackInitialise();
            return false;
        }
        g.present[i] = image.image;
        g.presentMemory[i] = image.memory;
    }

    LOGI("renderer: ready on %s, depth %s, %u MB frame arena%s", vk::pipeline::DeviceName(),
         g.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", g.arenaBytes >> 20,
         g.shadowBase ? " and a shadow of guest memory behind it" : ", no shadow of guest memory");
    StartPreparing();
    PrewarmFromCache();
    // A lost device ends the run, the way closing the window does.
    vk::pipeline::OnDeviceLost(crash::RequestExit);
    vk::record::Start();
    g.ready = true;
    return true;
}

void vk::renderer::Shutdown()
{
    if (!g.device) return;
    g.ready = false;
    // Before anything is destroyed, the display table's pipeline included: the
    // last frames are still on the GPU, and they use it -- and the last
    // commands may still be with the recorder thread.
    vk::record::Stop();
    StopWarmupScheduler();
    vkDeviceWaitIdle(g.device);
    StopPreparing();
    vk::display::Shutdown();
    WriteShaderCache(true);
    vk::pipeline::SaveCache();
    SummariseTargets();
    ClearRenderPassCache();
    g.pipelineCount = g.pipelines.size();
    g.shaderCount = g.shaders.size();
    for (auto& [key, pipeline] : g.pipelines) if (pipeline) vkDestroyPipeline(g.device, pipeline, nullptr);
    for (VkPipeline pipeline : g.replacedPipelines) vkDestroyPipeline(g.device, pipeline, nullptr);
    for (auto& [key, library] : g.inputLibraries) if (library) vkDestroyPipeline(g.device, library, nullptr);
    for (auto& [key, library] : g.outputLibraries) if (library) vkDestroyPipeline(g.device, library, nullptr);
    g.replacedPipelines.clear();
    g.inputLibraries.clear();
    g.outputLibraries.clear();
    for (auto& [hash, shader] : g.shaders)
    {
        if (shader.library) vkDestroyPipeline(g.device, shader.library, nullptr);
        if (shader.module) vkDestroyShaderModule(g.device, shader.module, nullptr);
    }
    for (auto& [key, target] : g.targets) DestroyTarget(target);
    g.pipelines.clear();
    g.shaders.clear();
    for (auto& last : g.lastShader) last = {};
    g.targets.clear();
    for (uint32_t i = 0; i < kPresentImages; i++)
    {
        if (g.present[i]) vkDestroyImage(g.device, g.present[i], nullptr);
        if (g.presentMemory[i]) vkFreeMemory(g.device, g.presentMemory[i], nullptr);
        g.present[i] = VK_NULL_HANDLE;
        g.presentMemory[i] = VK_NULL_HANDLE;
    }
    for (auto& [address, ring] : g.resolvedTo)
        for (Resolved* list : { ring.copies, ring.exact })
        for (uint32_t i = 0; i < kFrameSlots; i++)
        {
            Resolved& resolved = list[i];
            if (resolved.view) vkDestroyImageView(g.device, resolved.view, nullptr);
            if (resolved.gammaView) vkDestroyImageView(g.device, resolved.gammaView, nullptr);
            if (resolved.image) vkDestroyImage(g.device, resolved.image, nullptr);
            if (resolved.memory) vkFreeMemory(g.device, resolved.memory, nullptr);
        }
    g.resolvedTo.clear();
    if (g.readBack) vkDestroyBuffer(g.device, g.readBack, nullptr);
    if (g.readBackMemory) vkFreeMemory(g.device, g.readBackMemory, nullptr);
    g.readBack = VK_NULL_HANDLE;
    g.readBackMemory = VK_NULL_HANDLE;
    g.readBackMapped = nullptr;
    if (g.descriptors) vkDestroyDescriptorPool(g.device, g.descriptors, nullptr);
    gpu::watch::SetShadow(nullptr, 0);
    g.shadowBase = 0;
    if (g.arenaMapped) vkUnmapMemory(g.device, g.arenaMemory);
    if (g.arena) vkDestroyBuffer(g.device, g.arena, nullptr);
    if (g.arenaMemory) vkFreeMemory(g.device, g.arenaMemory, nullptr);
    if (g.guestQueries) vkDestroyQueryPool(g.device, g.guestQueries, nullptr);
    for (auto& timing : g.passTiming)
        if (timing.pool) { vkDestroyQueryPool(g.device, timing.pool, nullptr); timing.pool = VK_NULL_HANDLE; }
    g.timingOn = false;
    for (State::Slot& slot : g.slots)
    {
        if (slot.fence) vkDestroyFence(g.device, slot.fence, nullptr);
        if (slot.commands) vkDestroyCommandPool(g.device, slot.commands, nullptr);
    }
    if (g.setupFence) vkDestroyFence(g.device, g.setupFence, nullptr);

    if (g.setupCommands) vkDestroyCommandPool(g.device, g.setupCommands, nullptr);

    // The handles are gone; the counters stay, because the report is written
    // after the shutdown.
    g.arena = VK_NULL_HANDLE;
    g.arenaMemory = VK_NULL_HANDLE;
    g.arenaMapped = nullptr;
    g.guestQueries = VK_NULL_HANDLE;
    g.descriptors = VK_NULL_HANDLE;
    g.setupFence = VK_NULL_HANDLE;
    g.setupCommands = VK_NULL_HANDLE;
    for (State::Slot& slot : g.slots) slot = State::Slot{};
    g.recording = false;
    g.device = VK_NULL_HANDLE;
}

bool vk::renderer::Ready() { return g.ready; }

// The phone's thermal status (PowerManager THERMAL_STATUS_*), from the Java side. Read by the
// dynamic scene scale at the next frame.
void vk::renderer::SetThermalStatus(int status) { g.thermalStatus.store(status, std::memory_order_relaxed); }

#endif  // PES17_HAVE_VULKAN
