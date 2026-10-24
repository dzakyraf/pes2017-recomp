#include "upscaler.h"

#ifndef PES17_HAVE_VULKAN
bool vk::upscale::Enabled()
{
    // Off unless asked for: the two passes cost about 5 fps (2.3 ms a frame) on the Snapdragon 865 once it
    // has throttled, for a picture only a little sharper than the blit at the window's 1.25x scale.
    // PES17_UPSCALER=fsr, or `touch files/upscaler_on`, turns it on.
    static const bool wanted = [] {
        const char* mode = std::getenv("PES17_UPSCALER");
        return mode && std::strcmp(mode, "fsr") == 0;
    }();
    if (g.tried && !g.ready) return false;
#ifdef PES17_ANDROID
    // `files/upscaler_on` / `files/upscaler_off`: live, looked at twice a second.
    static std::atomic<uint32_t> asked{ 0 };
    static std::atomic<bool> on{ false }, off{ false };
    if (asked++ % 30 == 0)
    {
        on = access((android::GetPaths().external + "/upscaler_on").c_str(), F_OK) == 0;
        off = access((android::GetPaths().external + "/upscaler_off").c_str(), F_OK) == 0;
    }
    if (off) return false;
    return wanted || on;
#else
    return wanted;
#endif
}

VkImage vk::upscale::Record(VkDevice, VkPhysicalDevice, VkCommandBuffer, uint32_t, VkImage, uint32_t,
                            uint32_t, uint32_t, uint32_t) { return VK_NULL_HANDLE; }
void vk::upscale::Destroy(VkDevice) {}
#else

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>
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
#include "ffx/guest_output_triangle_strip_rect_vs.h"
#include "ffx/guest_output_ffx_fsr_easu_ps.h"
#include "ffx/guest_output_ffx_fsr_rcas_ps.h"
    }

    constexpr uint32_t kSlots = 2;                       // the presenter's frames in flight
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

    struct Rect { float x, y, width, height; };          // the vertex shader's push constants, in NDC
    struct Easu { float ratio[2]; float inputSizeInv[2]; };
    struct Rcas { int32_t outputOffset[2]; float sharpness; };

    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };

    struct Slot
    {
        Image easu, rcas;                                // EASU's output (sampled by RCAS), RCAS's output
        VkDescriptorSet sourceSet = VK_NULL_HANDLE, easuSet = VK_NULL_HANDLE;
    };

    struct State
    {
        bool tried = false, ready = false;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkPipelineLayout easuLayout = VK_NULL_HANDLE, rcasLayout = VK_NULL_HANDLE;
        VkRenderPass easuPass = VK_NULL_HANDLE, rcasPass = VK_NULL_HANDLE;
        VkShaderModule vs = VK_NULL_HANDLE, easuPs = VK_NULL_HANDLE, rcasPs = VK_NULL_HANDLE;
        VkPipeline easuPipeline = VK_NULL_HANDLE, rcasPipeline = VK_NULL_HANDLE;
        Slot slots[kSlots];
        uint32_t width = 0, height = 0;                  // the size the slots' images were made for
        std::map<VkImage, VkImageView> sourceViews;      // the renderer's present images, viewed once
        float sharpness = 1.0f;
    } g;

    bool Check(VkResult result, const char* what)
    {
        if (result == VK_SUCCESS) return true;
        LOGW("upscaler: %s failed (%d); falling back to the bilinear blit", what, int(result));
        return false;
    }

    VkShaderModule Module(VkDevice device, const uint32_t* code, size_t bytes)
    {
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = bytes;
        info.pCode = code;
        VkShaderModule module = VK_NULL_HANDLE;
        return Check(vkCreateShaderModule(device, &info, nullptr, &module), "a shader module") ? module : VK_NULL_HANDLE;
    }

    // One colour attachment, its old contents dropped, left in `finalLayout` for the next reader.
    VkRenderPass Pass(VkDevice device, VkImageLayout finalLayout, VkAccessFlags nextAccess, VkPipelineStageFlags nextStage)
    {
        VkAttachmentDescription attachment{};
        attachment.format = kFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = finalLayout;
        VkAttachmentReference colour{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colour;
        VkSubpassDependency dependencies[2]{};
        // Before: the last frame's reads of this image (a sampler, or the presenter's blit).
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // After: whoever reads what was drawn.
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask = nextStage;
        dependencies[1].dstAccessMask = nextAccess;
        VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 2;
        info.pDependencies = dependencies;
        VkRenderPass pass = VK_NULL_HANDLE;
        return Check(vkCreateRenderPass(device, &info, nullptr, &pass), "a render pass") ? pass : VK_NULL_HANDLE;
    }

    VkPipelineLayout Layout(VkDevice device, uint32_t effectBytes)
    {
        VkPushConstantRange ranges[2]{};
        ranges[0] = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Rect) };
        ranges[1] = { VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(Rect), effectBytes };
        VkPipelineLayoutCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        info.setLayoutCount = 1;
        info.pSetLayouts = &g.setLayout;
        info.pushConstantRangeCount = 2;
        info.pPushConstantRanges = ranges;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        return Check(vkCreatePipelineLayout(device, &info, nullptr, &layout), "a pipeline layout") ? layout : VK_NULL_HANDLE;
    }

    VkPipeline Pipeline(VkDevice device, VkShaderModule ps, VkPipelineLayout layout, VkRenderPass pass)
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g.vs, "main", nullptr };
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main", nullptr };
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;
        VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        info.renderPass = pass;
        VkPipeline pipeline = VK_NULL_HANDLE;
        return Check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "a pipeline")
            ? pipeline : VK_NULL_HANDLE;
    }

    bool Setup(VkDevice device)
    {
        g.tried = true;
        const double stops = std::getenv("PES17_FSR_SHARPNESS") ? std::atof(std::getenv("PES17_FSR_SHARPNESS")) : 0.2;
        g.sharpness = std::exp2f(-float(stops));          // FsrRcasCon's const0.x

        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 0.0f;
        if (!Check(vkCreateSampler(device, &sampler, nullptr, &g.sampler), "the sampler")) return false;

        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        bindings[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &g.sampler };
        VkDescriptorSetLayoutCreateInfo setLayout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setLayout.bindingCount = 2;
        setLayout.pBindings = bindings;
        if (!Check(vkCreateDescriptorSetLayout(device, &setLayout, nullptr, &g.setLayout), "the descriptor set layout")) return false;

        constexpr uint32_t kSets = kSlots * 2;
        VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSets }, { VK_DESCRIPTOR_TYPE_SAMPLER, kSets } };
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
        for (uint32_t i = 0; i < kSlots; i++) { g.slots[i].sourceSet = sets[i * 2]; g.slots[i].easuSet = sets[i * 2 + 1]; }

        g.vs = Module(device, shaders::guest_output_triangle_strip_rect_vs, sizeof(shaders::guest_output_triangle_strip_rect_vs));
        g.easuPs = Module(device, shaders::guest_output_ffx_fsr_easu_ps, sizeof(shaders::guest_output_ffx_fsr_easu_ps));
        g.rcasPs = Module(device, shaders::guest_output_ffx_fsr_rcas_ps, sizeof(shaders::guest_output_ffx_fsr_rcas_ps));
        if (!g.vs || !g.easuPs || !g.rcasPs) return false;
        g.easuLayout = Layout(device, sizeof(Easu));
        g.rcasLayout = Layout(device, sizeof(Rcas));
        // EASU's output is sampled by RCAS; RCAS's is copied to the window by the presenter's blit.
        g.easuPass = Pass(device, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        g.rcasPass = Pass(device, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (!g.easuLayout || !g.rcasLayout || !g.easuPass || !g.rcasPass) return false;
        g.easuPipeline = Pipeline(device, g.easuPs, g.easuLayout, g.easuPass);
        g.rcasPipeline = Pipeline(device, g.rcasPs, g.rcasLayout, g.rcasPass);
        if (!g.easuPipeline || !g.rcasPipeline) return false;
        LOGI("upscaler: FSR 1 (EASU + RCAS, sharpness %.2f stops) for the window", stops);
        return true;
    }

    void DestroyImage(VkDevice device, Image& image)
    {
        if (image.framebuffer) vkDestroyFramebuffer(device, image.framebuffer, nullptr);
        if (image.view) vkDestroyImageView(device, image.view, nullptr);
        if (image.image) vkDestroyImage(device, image.image, nullptr);
        if (image.memory) vkFreeMemory(device, image.memory, nullptr);
        image = {};
    }

    VkImageView View(VkDevice device, VkImage image)
    {
        VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = kFormat;
        info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        return Check(vkCreateImageView(device, &info, nullptr, &view), "an image view") ? view : VK_NULL_HANDLE;
    }

    bool MakeImage(VkDevice device, VkPhysicalDevice physical, Image& out, VkRenderPass pass,
                   uint32_t width, uint32_t height, VkImageUsageFlags usage)
    {
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = kFormat;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(vkCreateImage(device, &info, nullptr, &out.image), "an image")) return false;
        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(device, out.image, &needs);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = vk::util::FindMemory(memory, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX ||
            !Check(vkAllocateMemory(device, &allocate, nullptr, &out.memory), "image memory") ||
            !Check(vkBindImageMemory(device, out.image, out.memory, 0), "image memory"))
            return false;
        out.view = View(device, out.image);
        if (!out.view) return false;
        VkFramebufferCreateInfo framebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebuffer.renderPass = pass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &out.view;
        framebuffer.width = width;
        framebuffer.height = height;
        framebuffer.layers = 1;
        return Check(vkCreateFramebuffer(device, &framebuffer, nullptr, &out.framebuffer), "a framebuffer");
    }

    // The slots' images, made again when the window's picture changes size. In-flight frames may still
    // use the old ones, so the device is let go idle first; this happens on a rotation, not per frame.
    bool Resize(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height)
    {
        if (g.width == width && g.height == height) return true;
        vkDeviceWaitIdle(device);
        for (auto& slot : g.slots) { DestroyImage(device, slot.easu); DestroyImage(device, slot.rcas); }
        g.width = g.height = 0;
        for (auto& slot : g.slots)
        {
            if (!MakeImage(device, physical, slot.easu, g.easuPass, width, height, VK_IMAGE_USAGE_SAMPLED_BIT) ||
                !MakeImage(device, physical, slot.rcas, g.rcasPass, width, height, VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
                return false;
            VkDescriptorImageInfo image{ VK_NULL_HANDLE, slot.easu.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = slot.easuSet;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.pImageInfo = &image;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
        g.width = width;
        g.height = height;
        LOGI("upscaler: %ux%u", width, height);
        return true;
    }

    void Draw(VkCommandBuffer commands, VkRenderPass pass, VkFramebuffer framebuffer, VkPipeline pipeline,
              VkPipelineLayout layout, VkDescriptorSet set, const void* constants, uint32_t bytes)
    {
        VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.renderPass = pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = { { 0, 0 }, { g.width, g.height } };
        vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{ 0.0f, 0.0f, float(g.width), float(g.height), 0.0f, 1.0f };
        const VkRect2D scissor{ { 0, 0 }, { g.width, g.height } };
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        const Rect whole{ -1.0f, -1.0f, 2.0f, 2.0f };
        vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(whole), &whole);
        vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(Rect), bytes, constants);
        vkCmdDraw(commands, 4, 1, 0, 0);
        vkCmdEndRenderPass(commands);
    }
}

bool vk::upscale::Enabled()
{
    // Off unless asked for, as in the header: the Settings switch sends PES17_UPSCALER=fsr when it is
    // on and leaves it unset when it is off (nativeSetOption unsets an empty value), so unset is off.
    static const bool wanted = [] {
        const char* mode = std::getenv("PES17_UPSCALER");
        return mode && std::strcmp(mode, "fsr") == 0;
    }();
    if (g.tried && !g.ready) return false;
#ifdef PES17_ANDROID
    // `files/upscaler_on` / `files/upscaler_off`: live, looked at twice a second.
    static std::atomic<uint32_t> asked{ 0 };
    static std::atomic<bool> on{ false }, off{ false };
    if (asked++ % 30 == 0)
    {
        on = access((android::GetPaths().external + "/upscaler_on").c_str(), F_OK) == 0;
        off = access((android::GetPaths().external + "/upscaler_off").c_str(), F_OK) == 0;
    }
    if (off) return false;
    return wanted || on;
#else
    return wanted;
#endif
}

VkImage vk::upscale::Record(VkDevice device, VkPhysicalDevice physical, VkCommandBuffer commands, uint32_t slot,
                            VkImage source, uint32_t sourceWidth, uint32_t sourceHeight,
                            uint32_t width, uint32_t height)
{
    if (!g.tried) g.ready = Setup(device);
    if (!g.ready || slot >= kSlots || !source || !sourceWidth || !sourceHeight || !width || !height) return VK_NULL_HANDLE;
    if (!Resize(device, physical, width, height)) { g.ready = false; return VK_NULL_HANDLE; }
    Slot& s = g.slots[slot];

    VkImageView& view = g.sourceViews[source];
    if (!view) view = View(device, source);
    if (!view) return VK_NULL_HANDLE;
    VkDescriptorImageInfo image{ VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = s.sourceSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // The renderer's frame is read by a shader, then handed back in the layout its owner expects.
    vk::util::Barrier(commands, source, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    const Easu easu{ { float(sourceWidth) / float(width), float(sourceHeight) / float(height) },
                     { 1.0f / float(sourceWidth), 1.0f / float(sourceHeight) } };
    Draw(commands, g.easuPass, s.easu.framebuffer, g.easuPipeline, g.easuLayout, s.sourceSet, &easu, sizeof(easu));
    const Rcas rcas{ { 0, 0 }, g.sharpness };
    Draw(commands, g.rcasPass, s.rcas.framebuffer, g.rcasPipeline, g.rcasLayout, s.easuSet, &rcas, sizeof(rcas));

    vk::util::Barrier(commands, source, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    return s.rcas.image;
}

void vk::upscale::Destroy(VkDevice device)
{
    for (auto& slot : g.slots) { DestroyImage(device, slot.easu); DestroyImage(device, slot.rcas); }
    for (auto& [image, view] : g.sourceViews) if (view) vkDestroyImageView(device, view, nullptr);
    g.sourceViews.clear();
    if (g.easuPipeline) vkDestroyPipeline(device, g.easuPipeline, nullptr);
    if (g.rcasPipeline) vkDestroyPipeline(device, g.rcasPipeline, nullptr);
    if (g.easuLayout) vkDestroyPipelineLayout(device, g.easuLayout, nullptr);
    if (g.rcasLayout) vkDestroyPipelineLayout(device, g.rcasLayout, nullptr);
    if (g.easuPass) vkDestroyRenderPass(device, g.easuPass, nullptr);
    if (g.rcasPass) vkDestroyRenderPass(device, g.rcasPass, nullptr);
    for (VkShaderModule m : { g.vs, g.easuPs, g.rcasPs }) if (m) vkDestroyShaderModule(device, m, nullptr);
    if (g.pool) vkDestroyDescriptorPool(device, g.pool, nullptr);
    if (g.setLayout) vkDestroyDescriptorSetLayout(device, g.setLayout, nullptr);
    if (g.sampler) vkDestroySampler(device, g.sampler, nullptr);
    g = State{};
}
#endif
