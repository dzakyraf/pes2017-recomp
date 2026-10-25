// Runs the frame generator (runtime/gpu/vulkan/framegen.cpp, the same code the game uses) on frames
// from disk, on whatever Vulkan device the machine has (MoltenVK on a Mac), and scores the made frame
// against the real one between its two inputs.
//
//   tools/framegen_check.sh before.ppm truth.ppm after.ppm [made.ppm]
//
// Prints PSNR against `truth` for the made frame, for a plain average of the two inputs, and for
// showing `before` again (what a 30 fps picture shows in that blank). Binary PPM (P6) in and out.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "../runtime/gpu/vulkan/framegen.h"
#include "../runtime/gpu/vulkan/upscaler.h"

// framegen.cpp drops the upscaler's views when it remakes its images; there is no upscaler here.
void vk::upscale::Destroy(VkDevice) {}

namespace
{
    struct Picture { uint32_t width = 0, height = 0; std::vector<uint8_t> rgba; };

    bool ReadPpm(const char* path, Picture& out)
    {
        std::FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        unsigned w = 0, h = 0, max = 0;
        if (std::fscanf(f, "P6 %u %u %u", &w, &h, &max) != 3 || max != 255) { std::fclose(f); return false; }
        std::fgetc(f);
        std::vector<uint8_t> rgb(size_t(w) * h * 3);
        const bool ok = std::fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
        std::fclose(f);
        if (!ok) return false;
        out.width = w; out.height = h;
        out.rgba.resize(size_t(w) * h * 4);
        for (size_t i = 0; i < size_t(w) * h; i++)
        {
            out.rgba[i * 4 + 0] = rgb[i * 3 + 0];
            out.rgba[i * 4 + 1] = rgb[i * 3 + 1];
            out.rgba[i * 4 + 2] = rgb[i * 3 + 2];
            out.rgba[i * 4 + 3] = 255;
        }
        return true;
    }

    void WritePpm(const char* path, const Picture& p)
    {
        std::FILE* f = std::fopen(path, "wb");
        if (!f) return;
        std::fprintf(f, "P6\n%u %u\n255\n", p.width, p.height);
        for (size_t i = 0; i < size_t(p.width) * p.height; i++) std::fwrite(&p.rgba[i * 4], 1, 3, f);
        std::fclose(f);
    }

    double Psnr(const Picture& a, const Picture& b)
    {
        double sum = 0;
        for (size_t i = 0; i < a.rgba.size(); i++)
        {
            if (i % 4 == 3) continue;
            const double d = double(a.rgba[i]) - double(b.rgba[i]);
            sum += d * d;
        }
        const double mse = sum / (double(a.width) * a.height * 3);
        return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    }

    void Fail(const char* what) { std::fprintf(stderr, "framegen_check: %s\n", what); std::exit(1); }

    VkDevice device;
    VkPhysicalDevice physical;
    VkQueue queue;
    VkCommandPool pool;
    VkPhysicalDeviceMemoryProperties memory;

    uint32_t Memory(uint32_t allowed, VkMemoryPropertyFlags want)
    {
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
            if ((allowed & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) return i;
        Fail("no memory type");
        return 0;
    }

    struct Buffer { VkBuffer buffer; VkDeviceMemory memory; void* mapped; };

    Buffer HostBuffer(VkDeviceSize bytes)
    {
        Buffer b{};
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(device, &info, nullptr, &b.buffer);
        VkMemoryRequirements needs;
        vkGetBufferMemoryRequirements(device, b.buffer, &needs);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = Memory(needs.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &allocate, nullptr, &b.memory);
        vkBindBufferMemory(device, b.buffer, b.memory, 0);
        vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
        return b;
    }

    VkCommandBuffer Begin()
    {
        VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer commands;
        vkAllocateCommandBuffers(device, &allocate, &commands);
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(commands, &begin);
        return commands;
    }

    void Submit(VkCommandBuffer commands)
    {
        vkEndCommandBuffer(commands);
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) Fail("submit");
        vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(device, pool, 1, &commands);
    }

    void Barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to)
    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
    }

    // A renderer present image: RGBA8, left in TRANSFER_SRC as the renderer hands it over.
    VkImage Upload(const Picture& p)
    {
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = { p.width, p.height, 1 };
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage image;
        vkCreateImage(device, &info, nullptr, &image);
        VkMemoryRequirements needs;
        vkGetImageMemoryRequirements(device, image, &needs);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = Memory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory mem;
        vkAllocateMemory(device, &allocate, nullptr, &mem);
        vkBindImageMemory(device, image, mem, 0);

        Buffer staging = HostBuffer(p.rgba.size());
        std::memcpy(staging.mapped, p.rgba.data(), p.rgba.size());
        VkCommandBuffer commands = Begin();
        Barrier(commands, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { p.width, p.height, 1 };
        vkCmdCopyBufferToImage(commands, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Submit(commands);
        return image;
    }

    Picture Download(VkImage image, uint32_t width, uint32_t height)
    {
        Picture p;
        p.width = width; p.height = height;
        p.rgba.resize(size_t(width) * height * 4);
        Buffer staging = HostBuffer(p.rgba.size());
        VkCommandBuffer commands = Begin();
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { width, height, 1 };
        vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
        Submit(commands);
        std::memcpy(p.rgba.data(), staging.mapped, p.rgba.size());
        return p;
    }
}

int main(int argc, char** argv)
{
    if (argc < 4) Fail("usage: framegen_check before.ppm truth.ppm after.ppm [made.ppm]");
    Picture before, truth, after;
    if (!ReadPpm(argv[1], before) || !ReadPpm(argv[2], truth) || !ReadPpm(argv[3], after)) Fail("cannot read a PPM");
    if (before.width != after.width || before.height != after.height || truth.width != before.width ||
        truth.height != before.height)
        Fail("the three pictures differ in size");

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_1;
    const char* instanceExtensions[] = { VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME };
    VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    VkInstance instance;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) Fail("no Vulkan instance");
    uint32_t count = 1;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (!count) Fail("no Vulkan device");
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = 0;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = { "VK_KHR_portability_subset" };
    VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS)
    {
        deviceInfo.enabledExtensionCount = 0;
        if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS) Fail("no Vulkan device");
    }
    vkGetDeviceQueue(device, 0, 0, &queue);
    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    vkCreateCommandPool(device, &poolInfo, nullptr, &pool);

    const VkImage first = Upload(before), second = Upload(after);
    const double period = 1e9 / 60.0;
    // FRAMEGEN_BALL="x0,y0,x1,y1,radius": where the renderer would say the ball is in each frame.
    if (const char* ball = std::getenv("FRAMEGEN_BALL"))
    {
        float x0, y0, x1, y1, radius;
        if (std::sscanf(ball, "%f,%f,%f,%f,%f", &x0, &y0, &x1, &y1, &radius) == 5)
        {
            vk::framegen::NoteBall(1, true, x0, y0, radius);
            vk::framegen::NoteBall(2, true, x1, y1, radius);
        }
    }

    VkCommandBuffer commands = Begin();
    vk::framegen::Result r = vk::framegen::Record(device, physical, commands, first, before.width, before.height, 0, period, 1);
    Submit(commands);
    if (!r.real || r.made) Fail("the first frame should be copied and nothing made");

    commands = Begin();
    VkQueryPool timer;
    VkQueryPoolCreateInfo queryInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2;
    vkCreateQueryPool(device, &queryInfo, nullptr, &timer);
    vkCmdResetQueryPool(commands, timer, 0, 2);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timer, 0);
    r = vk::framegen::Record(device, physical, commands, second, after.width, after.height, int64_t(2 * period), period, 2);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timer, 1);
    Submit(commands);
    if (!r.made || !r.real) Fail("the second frame should make one");
    uint64_t stamps[2]{};
    vkGetQueryPoolResults(device, timer, 0, 2, sizeof stamps, stamps, sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    // FRAMEGEN_TIMING=n: n more pairs, the fastest GPU time of them (a laptop GPU clocks down between small jobs).
    double medianMs = 0;
    if (const char* runs = std::getenv("FRAMEGEN_TIMING"))
    {
        std::vector<double> times;
        for (int i = 0; i < std::atoi(runs); i++)
        {
            commands = Begin();
            vkCmdResetQueryPool(commands, timer, 0, 2);
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timer, 0);
            vk::framegen::Record(device, physical, commands, (i & 1) ? second : first, after.width, after.height,
                                 int64_t((3 + i) * 2 * period), period);
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timer, 1);
            Submit(commands);
            vkGetQueryPoolResults(device, timer, 0, 2, sizeof stamps, stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            times.push_back(double(stamps[1] - stamps[0]) * properties.limits.timestampPeriod / 1e6);
        }
        std::sort(times.begin(), times.end());
        if (!times.empty()) medianMs = times[0];
        // Back to the pair being scored.
        commands = Begin();
        vk::framegen::Reset();
        vk::framegen::Record(device, physical, commands, first, before.width, before.height, 0, period, 1);
        Submit(commands);
        commands = Begin();
        r = vk::framegen::Record(device, physical, commands, second, after.width, after.height, int64_t(2 * period), period, 2);
        Submit(commands);
        std::printf("fastest GPU %.2f ms over %d pairs\n", medianMs, std::atoi(runs));
    }

    const Picture made = Download(r.made, before.width, before.height);
    const Picture real = Download(r.real, before.width, before.height);
    Picture average = before;
    for (size_t i = 0; i < average.rgba.size(); i++)
        average.rgba[i] = uint8_t((unsigned(before.rgba[i]) + unsigned(after.rgba[i]) + 1) / 2);

    std::printf("device %s, %ux%u, GPU %.2f ms for copy + made frame\n", properties.deviceName, before.width,
                before.height, double(stamps[1] - stamps[0]) * properties.limits.timestampPeriod / 1e6);
    std::printf("PSNR vs truth: made %.2f dB, average %.2f dB, repeat %.2f dB (real copy exact: %s)\n",
                Psnr(made, truth), Psnr(average, truth), Psnr(before, truth),
                real.rgba == after.rgba ? "yes" : "NO");
    if (argc > 4) WritePpm(argv[4], made);
    // FRAMEGEN_FIELD=1: the motion each level found, in the frame's pixels, as rows of text.
    if (std::getenv("FRAMEGEN_FIELD"))
        for (uint32_t level = 0; level < 2; level++)
        {
            uint32_t fw = 0, fh = 0;
            const VkImage field = vk::framegen::DebugField(fw, fh, level);
            Buffer staging = HostBuffer(VkDeviceSize(fw) * fh * 8);
            commands = Begin();
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { fw, fh, 1 };
            vkCmdCopyImageToBuffer(commands, field, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &copy);
            Submit(commands);
            const uint16_t* half = static_cast<const uint16_t*>(staging.mapped);
            auto f = [](uint16_t h) {
                const int e = (h >> 10) & 31, m = h & 1023;
                const float v = e == 0 ? m / 1024.0f / 16384.0f : std::ldexp(1.0f + m / 1024.0f, e - 15);
                return (h & 0x8000) ? -v : v;
            };
            const float scale = level ? 16.0f : 4.0f;
            std::printf("level %u field %ux%u (frame pixels, every 4th block)\n", level, fw, fh);
            for (uint32_t y = 0; y < fh; y += level ? 1 : 4)
            {
                for (uint32_t x = 0; x < fw; x += level ? 1 : 4)
                    std::printf("%4.0f,%-4.0f", f(half[(y * fw + x) * 4]) * scale, f(half[(y * fw + x) * 4 + 1]) * scale);
                std::printf("\n");
            }
        }
    vk::framegen::Destroy(device);
    return 0;
}
