#include "framegen.h"

#ifndef PES17_HAVE_VULKAN
bool vk::framegen::Wanted(uint32_t, double) { return false; }
bool vk::framegen::WantsBall() { return false; }
void vk::framegen::NoteBall(uint64_t, bool, float, float, float) {}
vk::framegen::Result vk::framegen::Record(VkDevice, VkPhysicalDevice, VkCommandBuffer, VkImage, uint32_t, uint32_t,
                                          int64_t, double, uint64_t) { return {}; }
void vk::framegen::Reset() {}
uint64_t vk::framegen::MadeFrames() { return 0; }
VkImage vk::framegen::DebugField(uint32_t&, uint32_t&, uint32_t) { return VK_NULL_HANDLE; }
void vk::framegen::Destroy(VkDevice) {}
#else

#include <algorithm>
#include <cmath>
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <unistd.h>
#include "upscaler.h"
#include "util.h"
#include "../../env.h"
#include "../../log.h"
#ifdef PES17_ANDROID
#include "../../android/android.h"
#endif

namespace
{
    namespace shaders
    {
#include "framegen/shaders.h"
    }

    constexpr VkFormat kColour = VK_FORMAT_R8G8B8A8_UNORM;   // the renderer's present images
    constexpr VkFormat kField = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr uint32_t kLumaDivisor = 4;                       // each luma level is a quarter of the one above
    constexpr uint32_t kBlock = 4;                             // a block is 4x4 pixels of its level
    // A level's pixels each way round no motion. The coarse window reaches 128 of the frame's pixels;
    // the fine one only has to catch what the coarse level could not see (a player against the pan).
    constexpr float kCoarseRange = 8.0f, kFineRange = 2.0f;
    constexpr float kSmoothCost = 0.02f;

    enum Pass { kDownsample, kSearch, kSmooth, kWarp, kPasses };
    constexpr uint32_t kPushBytes = 80;

    // The ball, as the renderer saw it in each recent frame (NoteBall), by serial.
    struct Ball { uint64_t serial = 0; bool seen = false; float x = 0, y = 0, radius = 0; };
    constexpr uint32_t kBalls = 8;
    // A ball that moved further than this between two real frames went somewhere else (a cut).
    constexpr float kBallReach = 400.0f;
    // Real frames (about 3 s at 30) without the ball before the picture counts as a menu, not a match.
    constexpr uint32_t kMenuFrames = 90;

    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;     // as of the last command recorded
    };

    // One level of the luma pyramid: the two frames' luma (by parity) and the motion found on it.
    struct Level
    {
        uint32_t width = 0, height = 0, gridWidth = 0, gridHeight = 0;
        Image luma[2], raw, field;
        VkDescriptorSet downsampleSet[2]{}, searchSet[2]{}, smoothSet = VK_NULL_HANDLE;
    };

    struct State
    {
        bool tried = false, ready = false;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkShaderModule modules[kPasses]{};
        VkPipeline pipelines[kPasses]{};

        // Made for one frame size. history[parity] is the newest real frame, history[!parity] the one
        // before; the luma follows the same parity. levels[0] is a quarter of the frame each way,
        // levels[1] a sixteenth.
        uint32_t width = 0, height = 0;
        Image history[2], made;
        Level levels[2];
        VkDescriptorSet warpSet[2]{};
        uint32_t parity = 0;

        bool havePrevious = false;     // history[parity] holds a frame to pair the next one with
        bool fieldValid = false;       // the field holds last pair's motion, a predictor for the next
        int64_t previousAtNs = 0;
        std::atomic<uint64_t> made_{ 0 };
        std::atomic<bool> reset{ false };

        std::mutex ballLock;
        Ball balls[kBalls];
        Ball previousBall;             // the ball of history[parity] once the next frame comes
        uint32_t framesWithoutBall = 0; // real frames since the renderer last saw the ball
    } g;

    Ball BallOf(uint64_t serial)
    {
        std::lock_guard lock(g.ballLock);
        for (const Ball& ball : g.balls)
            if (ball.serial == serial && serial) return ball;
        return {};
    }

    bool Check(VkResult result, const char* what)
    {
        if (result == VK_SUCCESS) return true;
        LOGW("framegen: %s failed (%d); frames are shown as drawn", what, int(result));
        return false;
    }

    // Everything before, in this queue, against everything after: the passes are few and small,
    // and one queue carries the renderer's work and the window's, so this is all the ordering needed.
    void Transition(VkCommandBuffer commands, Image& image, VkImageLayout to)
    {
        vk::util::Barrier(commands, image.image, VK_IMAGE_ASPECT_COLOR_BIT, image.layout, to,
                          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        image.layout = to;
    }

    void Discard(VkCommandBuffer commands, Image& image, VkImageLayout to)
    {
        image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        Transition(commands, image, to);
    }

    void ComputeToCompute(VkCommandBuffer commands)
    {
        VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    bool Setup(VkDevice device)
    {
        g.tried = true;
        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 0.0f;
        if (!Check(vkCreateSampler(device, &sampler, nullptr, &g.sampler), "the sampler")) return false;

        VkDescriptorSetLayoutBinding bindings[5]{};
        for (uint32_t i = 0; i < 4; i++)
            bindings[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g.sampler };
        bindings[4] = { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo setLayout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setLayout.bindingCount = 5;
        setLayout.pBindings = bindings;
        if (!Check(vkCreateDescriptorSetLayout(device, &setLayout, nullptr, &g.setLayout), "the descriptor set layout"))
            return false;

        constexpr uint32_t kSets = 2 * 5 + 2;     // per level: downsample x2, search x2, smooth; warp x2
        const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 4 },
                                                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets } };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets = kSets;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        if (!Check(vkCreateDescriptorPool(device, &pool, nullptr, &g.pool), "the descriptor pool")) return false;
        VkDescriptorSetLayout layouts[kSets];
        for (auto& l : layouts) l = g.setLayout;
        VkDescriptorSet sets[kSets];
        VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocate.descriptorPool = g.pool;
        allocate.descriptorSetCount = kSets;
        allocate.pSetLayouts = layouts;
        if (!Check(vkAllocateDescriptorSets(device, &allocate, sets), "the descriptor sets")) return false;
        uint32_t next = 0;
        for (Level& level : g.levels)
        {
            level.downsampleSet[0] = sets[next++]; level.downsampleSet[1] = sets[next++];
            level.searchSet[0] = sets[next++]; level.searchSet[1] = sets[next++];
            level.smoothSet = sets[next++];
        }
        g.warpSet[0] = sets[next++]; g.warpSet[1] = sets[next++];

        const VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushBytes };
        VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &g.setLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &range;
        if (!Check(vkCreatePipelineLayout(device, &layout, nullptr, &g.layout), "the pipeline layout")) return false;

        const struct { const uint32_t* code; size_t bytes; } code[kPasses] = {
            { shaders::framegen_downsample, sizeof(shaders::framegen_downsample) },
            { shaders::framegen_search, sizeof(shaders::framegen_search) },
            { shaders::framegen_smooth, sizeof(shaders::framegen_smooth) },
            { shaders::framegen_warp, sizeof(shaders::framegen_warp) },
        };
        for (uint32_t i = 0; i < kPasses; i++)
        {
            VkShaderModuleCreateInfo module{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            module.codeSize = code[i].bytes;
            module.pCode = code[i].code;
            if (!Check(vkCreateShaderModule(device, &module, nullptr, &g.modules[i]), "a shader module")) return false;
            VkComputePipelineCreateInfo pipeline{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pipeline.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                               VK_SHADER_STAGE_COMPUTE_BIT, g.modules[i], "main", nullptr };
            pipeline.layout = g.layout;
            if (!Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &g.pipelines[i]),
                       "a pipeline"))
                return false;
        }
        LOGI("framegen: on -- a made frame between each two the title draws");
        return true;
    }

    void DestroyImage(VkDevice device, Image& image)
    {
        if (image.view) vkDestroyImageView(device, image.view, nullptr);
        if (image.image) vkDestroyImage(device, image.image, nullptr);
        if (image.memory) vkFreeMemory(device, image.memory, nullptr);
        image = {};
    }

    void DestroyImages(VkDevice device)
    {
        for (Image* image : { &g.history[0], &g.history[1], &g.made }) DestroyImage(device, *image);
        for (Level& level : g.levels)
            for (Image* image : { &level.luma[0], &level.luma[1], &level.raw, &level.field }) DestroyImage(device, *image);
        g.width = g.height = 0;
        g.havePrevious = g.fieldValid = false;
    }

    bool MakeImage(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, Image& out, VkFormat format,
                   uint32_t width, uint32_t height, VkImageUsageFlags usage)
    {
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(vkCreateImage(device, &info, nullptr, &out.image), "an image")) return false;
        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(device, out.image, &needs);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = vk::util::FindMemory(memory, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX ||
            !Check(vkAllocateMemory(device, &allocate, nullptr, &out.memory), "image memory") ||
            !Check(vkBindImageMemory(device, out.image, out.memory, 0), "image memory"))
            return false;
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = out.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return Check(vkCreateImageView(device, &view, nullptr, &out.view), "an image view");
    }

    // The set's sampled images at bindings 0.., its output at 4, all in GENERAL layout.
    void Write(VkDevice device, VkDescriptorSet set, std::initializer_list<const Image*> sampled, const Image& output)
    {
        VkDescriptorImageInfo infos[5]{};
        VkWriteDescriptorSet writes[5]{};
        uint32_t n = 0;
        for (const Image* image : sampled)
        {
            infos[n] = { VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL };
            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, n, 0, 1,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &infos[n], nullptr, nullptr };
            n++;
        }
        infos[n] = { VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL };
        writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 4, 0, 1,
                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[n], nullptr, nullptr };
        vkUpdateDescriptorSets(device, n + 1, writes, 0, nullptr);
    }

    // The images for frames of this size, made again when it changes (the device is let go idle
    // first; this happens at a change of resolution, not per frame).
    bool Resize(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height)
    {
        if (g.width == width && g.height == height) return true;
        vkDeviceWaitIdle(device);
        // The upscaler keeps a view of every image it was handed, by handle; ours are about to go.
        vk::upscale::Destroy(device);
        DestroyImages(device);

        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        // SAMPLED on the colour images also lets the window's upscaler read them.
        const VkImageUsageFlags colour = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        for (uint32_t i = 0; i < 2; i++)
            if (!MakeImage(device, memory, g.history[i], kColour, width, height, colour | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
                return false;
        if (!MakeImage(device, memory, g.made, kColour, width, height, colour | VK_IMAGE_USAGE_STORAGE_BIT)) return false;

        uint32_t above = width, aboveHeight = height;
        for (Level& level : g.levels)
        {
            level.width = (above + kLumaDivisor - 1) / kLumaDivisor;
            level.height = (aboveHeight + kLumaDivisor - 1) / kLumaDivisor;
            level.gridWidth = (level.width + kBlock - 1) / kBlock;
            level.gridHeight = (level.height + kBlock - 1) / kBlock;
            above = level.width; aboveHeight = level.height;
            for (uint32_t i = 0; i < 2; i++)
                if (!MakeImage(device, memory, level.luma[i], kColour, level.width, level.height,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT))
                    return false;
            const VkImageUsageFlags field = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!MakeImage(device, memory, level.raw, kField, level.gridWidth, level.gridHeight, field) ||
                !MakeImage(device, memory, level.field, kField, level.gridWidth, level.gridHeight, field))
                return false;
        }

        Level& fine = g.levels[0];
        Level& coarse = g.levels[1];
        for (uint32_t p = 0; p < 2; p++)
        {
            Write(device, fine.downsampleSet[p], { &g.history[p] }, fine.luma[p]);
            Write(device, coarse.downsampleSet[p], { &fine.luma[p] }, coarse.luma[p]);
            // The coarse level has no coarser one: its field stands in, unread (coarseScale 0).
            Write(device, coarse.searchSet[p], { &coarse.luma[p ^ 1], &coarse.luma[p], &coarse.field, &coarse.field }, coarse.raw);
            Write(device, fine.searchSet[p], { &fine.luma[p ^ 1], &fine.luma[p], &fine.field, &coarse.field }, fine.raw);
            Write(device, g.warpSet[p], { &g.history[p ^ 1], &g.history[p], &fine.field, &coarse.field }, g.made);
        }
        for (Level& level : g.levels) Write(device, level.smoothSet, { &level.raw }, level.field);

        g.width = width; g.height = height;
        LOGI("framegen: %ux%u frames; blocks of %ux%u and %ux%u", width, height, fine.gridWidth, fine.gridHeight,
             coarse.gridWidth, coarse.gridHeight);
        return true;
    }

    void Dispatch(VkCommandBuffer commands, Pass pass, VkDescriptorSet set, const void* constants, uint32_t bytes,
                  uint32_t width, uint32_t height)
    {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipelines[pass]);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(commands, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, constants);
        vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);
    }
}

bool vk::framegen::Wanted(uint32_t limit, double displayPeriodNs)
{
    static const bool asked = env::Flag("PES17_FRAMEGEN");
    if (!asked || (g.tried && !g.ready)) return false;
#ifdef PES17_ANDROID
    // `files/framegen_off`: live, looked at twice a second, for A/B runs.
    static std::atomic<uint32_t> looked{ 0 };
    static std::atomic<bool> off{ false };
    if (looked++ % 30 == 0) off = access((android::GetPaths().external + "/framegen_off").c_str(), F_OK) == 0;
    if (off) return false;
#endif
    // A made frame for every real one doubles the rate, so the title has to be at half the display's:
    // faster and the made frames only delay the real ones, slower (30 on a 120 Hz screen) and the
    // made frame gets one blank of the four, which looks worse than none.
    if (!limit || displayPeriodNs <= 0) return false;
    const double blanks = 1e9 / double(limit) / displayPeriodNs;
    return blanks >= 1.9 && blanks <= 2.1;
}

bool vk::framegen::WantsBall()
{
    static const bool asked = env::Flag("PES17_FRAMEGEN");
    return asked;
}

void vk::framegen::NoteBall(uint64_t serial, bool seen, float x, float y, float radius)
{
    std::lock_guard lock(g.ballLock);
    g.balls[serial % kBalls] = { serial, seen, x, y, radius };
    // Now and then, so a log shows whether the renderer finds the ball and where.
    static uint32_t frames = 0, found = 0;
    found += seen;
    if (++frames % 600 == 0)
    {
        LOGI("framegen: ball seen in %u of the last 600 frames%s", found, seen ? "" : " (not this one)");
        if (seen) LOGI("framegen: ball at %.1f,%.1f radius %.1f", x, y, radius);
        found = 0;
    }
}

vk::framegen::Result vk::framegen::Record(VkDevice device, VkPhysicalDevice physical, VkCommandBuffer commands,
                                          VkImage source, uint32_t width, uint32_t height, int64_t nowNs,
                                          double displayPeriodNs, uint64_t serial)
{
    const Ball ball = BallOf(serial);
    const Ball previousBall = g.previousBall;
    g.previousBall = ball;
    if (!g.tried) g.ready = Setup(device);
    if (!g.ready || !source || !width || !height) return {};
    if (!Resize(device, physical, width, height)) { g.ready = false; return {}; }
    if (g.reset.exchange(false)) g.havePrevious = g.fieldValid = false;

    // A frame long after the last -- a pause, a load, the window gone and back -- is not paired with it.
    // Menus and their transitions (slides, fades, wipes) have no ball and no camera motion for the
    // search to follow; warping them tears the interface. Without the ball in the last few seconds
    // of real frames the made frame uses no motion at all: still parts are kept, what changes is held.
    g.framesWithoutBall = ball.seen ? 0 : std::min(g.framesWithoutBall + 1, 1000u);
    const bool inMatch = g.framesWithoutBall < kMenuFrames;
    const bool pair = g.havePrevious && nowNs - g.previousAtNs < int64_t(5.0 * displayPeriodNs);
    g.parity ^= 1;
    const uint32_t p = g.parity;
    Image& current = g.history[p];
    Image& previous = g.history[p ^ 1];

    Discard(commands, current, VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy copy{};
    copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = { width, height, 1 };
    vkCmdCopyImage(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, current.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    Transition(commands, current, VK_IMAGE_LAYOUT_GENERAL);

    // This frame's luma pyramid, which the next pair's search needs whether or not this one makes a frame.
    uint32_t aboveWidth = width, aboveHeight = height;
    for (uint32_t i = 0; i < 2; i++)
    {
        Level& level = g.levels[i];
        if (i) ComputeToCompute(commands);
        Discard(commands, level.luma[p], VK_IMAGE_LAYOUT_GENERAL);
        struct { int32_t size[2]; float aboveTexel[2]; int32_t lumaInput; } down{
            { int32_t(level.width), int32_t(level.height) },
            { 1.0f / float(aboveWidth), 1.0f / float(aboveHeight) }, int32_t(i) };
        Dispatch(commands, kDownsample, level.downsampleSet[p], &down, sizeof(down), level.width, level.height);
        aboveWidth = level.width; aboveHeight = level.height;
    }

    Result result;
    if (pair)
    {
        ComputeToCompute(commands);
        // Coarse first: its field is a starting point for the fine search.
        for (int i = 1; i >= 0; i--)
        {
            Level& level = g.levels[i];
            Transition(commands, level.luma[p ^ 1], VK_IMAGE_LAYOUT_GENERAL);
            if (!g.fieldValid)
            {
                // No motion known: the search starts from none.
                Discard(commands, level.field, VK_IMAGE_LAYOUT_GENERAL);
                const VkClearColorValue zero{};
                const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdClearColorImage(commands, level.field.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            }
            Transition(commands, level.field, VK_IMAGE_LAYOUT_GENERAL);
            Discard(commands, level.raw, VK_IMAGE_LAYOUT_GENERAL);
            const Level& coarse = g.levels[1];
            struct { int32_t grid[2]; float lumaTexel[2]; float smoothCost, range, coarseScale, pad; float coarseTexel[2]; } search{
                { int32_t(level.gridWidth), int32_t(level.gridHeight) },
                { 1.0f / float(level.width), 1.0f / float(level.height) }, kSmoothCost, i ? kCoarseRange : kFineRange,
                i ? 0.0f : float(kLumaDivisor), 0.0f,
                { 1.0f / float(coarse.gridWidth * kBlock * kLumaDivisor), 1.0f / float(coarse.gridHeight * kBlock * kLumaDivisor) } };
            Dispatch(commands, kSearch, level.searchSet[p], &search, sizeof(search), level.gridWidth, level.gridHeight);

            // The search read the old field; the median writes the new one over it.
            Transition(commands, level.raw, VK_IMAGE_LAYOUT_GENERAL);
            Transition(commands, level.field, VK_IMAGE_LAYOUT_GENERAL);
            struct { int32_t grid[2]; } smooth{ { int32_t(level.gridWidth), int32_t(level.gridHeight) } };
            Dispatch(commands, kSmooth, level.smoothSet, &smooth, sizeof(smooth), level.gridWidth, level.gridHeight);
            ComputeToCompute(commands);
        }
        g.fieldValid = true;

        const Level& fine = g.levels[0];
        Transition(commands, previous, VK_IMAGE_LAYOUT_GENERAL);
        Discard(commands, g.made, VK_IMAGE_LAYOUT_GENERAL);
        const Level& coarse = g.levels[1];
        const uint32_t coarsePixels = kLumaDivisor * kLumaDivisor;          // the frame's pixels per coarse pixel
        // The ball in both frames, when the renderer saw it in both and it did not jump.
        const bool ballKnown = previousBall.seen && ball.seen &&
                               std::hypot(ball.x - previousBall.x, ball.y - previousBall.y) < kBallReach;
        struct { int32_t size[2]; float texel[2]; int32_t grid[2]; float blockPixels, lumaScale; float coarseTexel[2];
                 float coarseScale, ballRadius; float ballPrevious[2], ballCurrent[2]; float menu, pad; } warp{
            { int32_t(width), int32_t(height) }, { 1.0f / float(width), 1.0f / float(height) },
            { int32_t(fine.gridWidth), int32_t(fine.gridHeight) }, float(kBlock * kLumaDivisor), float(kLumaDivisor),
            { 1.0f / float(coarse.gridWidth * kBlock * coarsePixels), 1.0f / float(coarse.gridHeight * kBlock * coarsePixels) },
            float(coarsePixels), ballKnown ? std::max(ball.radius, previousBall.radius) : 0.0f,
            { previousBall.x, previousBall.y }, { ball.x, ball.y }, inMatch ? 0.0f : 1.0f, 0.0f };
        Dispatch(commands, kWarp, g.warpSet[p], &warp, sizeof(warp), width, height);
        Transition(commands, g.made, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        result.made = g.made.image;
        g.made_.fetch_add(1, std::memory_order_relaxed);
    }
    else g.fieldValid = false;

    Transition(commands, current, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    result.real = current.image;
    g.havePrevious = true;
    g.previousAtNs = nowNs;
    return result;
}

void vk::framegen::Reset() { g.reset = true; }

uint64_t vk::framegen::MadeFrames() { return g.made_.load(std::memory_order_relaxed); }

VkImage vk::framegen::DebugField(uint32_t& width, uint32_t& height, uint32_t level)
{
    const Level& l = g.levels[level & 1];
    width = l.gridWidth;
    height = l.gridHeight;
    return l.field.image;
}

void vk::framegen::Destroy(VkDevice device)
{
    DestroyImages(device);
    for (uint32_t i = 0; i < kPasses; i++)
    {
        if (g.pipelines[i]) vkDestroyPipeline(device, g.pipelines[i], nullptr);
        if (g.modules[i]) vkDestroyShaderModule(device, g.modules[i], nullptr);
    }
    if (g.layout) vkDestroyPipelineLayout(device, g.layout, nullptr);
    if (g.pool) vkDestroyDescriptorPool(device, g.pool, nullptr);
    if (g.setLayout) vkDestroyDescriptorSetLayout(device, g.setLayout, nullptr);
    if (g.sampler) vkDestroySampler(device, g.sampler, nullptr);
    g.tried = g.ready = false;
    g.sampler = VK_NULL_HANDLE; g.setLayout = VK_NULL_HANDLE; g.pool = VK_NULL_HANDLE; g.layout = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < kPasses; i++) { g.pipelines[i] = VK_NULL_HANDLE; g.modules[i] = VK_NULL_HANDLE; }
    for (Level& level : g.levels) level = Level{};
    g.warpSet[0] = g.warpSet[1] = VK_NULL_HANDLE;
}
#endif
