// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/host_passes/rcas_pass.h"

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/rcas_frag.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include <vk_mem_alloc.h>

namespace Vulkan::HostPasses {

namespace {

constexpr vk::ImageSubresourceRange kColor = {
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};

} // namespace

RcasPass::RcasPass() : input{std::make_unique<Frame>()} {}

RcasPass::~RcasPass() {
    if (input->image_view) {
        device.destroyImageView(input->image_view);
    }
    if (input->image) {
        vmaDestroyImage(allocator, input->image, input_allocation);
    }
}

// A full-screen triangle sampling binding 0, with a float of push constants, as the post process
// pass's pipeline.
void RcasPass::Create(vk::Device device_, VmaAllocator allocator_, vk::Format surface_format) {
    device = device_;
    allocator = allocator_;
    const vk::DescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    desc_set_layout = Check<"create rcas descriptor set layout">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1,
        .pBindings = &binding,
    }));
    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(float),
    };
    pipeline_layout = Check<"create rcas pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1U,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));

    const auto vs_module = CompileSPV(FS_TRI_VERT, device);
    ASSERT(vs_module);
    const auto fs_module = CompileSPV(RCAS_FRAG, device);
    ASSERT(fs_module);
    SetObjectName(device, fs_module, "rcas.frag");
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = vs_module, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = fs_module, .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering_ci{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &surface_format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{.viewportCount = 1, .scissorCount = 1};
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState attachment{
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo blending{
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };
    pipeline = Check<"create rcas pipeline">(device.createGraphicsPipelineUnique(
        {}, vk::GraphicsPipelineCreateInfo{
                .pNext = &rendering_ci,
                .stageCount = stages.size(),
                .pStages = stages.data(),
                .pVertexInputState = &vertex_input,
                .pInputAssemblyState = &input_assembly,
                .pViewportState = &viewport_info,
                .pRasterizationState = &raster,
                .pMultisampleState = &multisampling,
                .pColorBlendState = &blending,
                .pDynamicState = &dynamic_info,
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);

    // RCAS reads whole texels (texelFetch); the sampler only completes the descriptor.
    sampler = Check<"create rcas sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    }));
}

Frame& RcasPass::Input(vk::CommandBuffer cmdbuf, Scheduler& scheduler, const Frame& output) {
    if (input->width != output.width || input->height != output.height ||
        input->format != output.format || !input->image) {
        if (input->image) { // the previous frame may still read it
            scheduler.DeferOperation([device = device, allocator = allocator, image = input->image,
                                      view = input->image_view, allocation = input_allocation] {
                device.destroyImageView(view);
                vmaDestroyImage(allocator, image, allocation);
            });
        }
        const VkImageCreateInfo image_ci = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = output.format,
            .extent = {output.width, output.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
        });
        const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage image{};
        const VkResult result =
            vmaCreateImage(allocator, &image_ci, &alloc_ci, &image, &input_allocation, nullptr);
        ASSERT_MSG(result == VK_SUCCESS, "Failed allocating the RCAS input: {}",
                   vk::to_string(vk::Result{result}));
        input->image = vk::Image{image};
        SetObjectName(device, input->image, "RCAS input");
        input->image_view = Check<"create rcas input view">(device.createImageView({
            .image = input->image,
            .viewType = vk::ImageViewType::e2D,
            .format = output.format,
            .subresourceRange = kColor,
        }));
        input->width = output.width;
        input->height = output.height;
        input->format = output.format;
    }
    // The last frame's RCAS read it: wait for that before drawing over it.
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = input->image,
        .subresourceRange = kColor,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    return *input;
}

void RcasPass::Render(vk::CommandBuffer cmdbuf, Frame& output, float sharpness) {
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = "Host/RCAS"});
    }
    const vk::DescriptorImageInfo image_info{
        .sampler = *sampler,
        .imageView = input->image_view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    const vk::WriteDescriptorSet set_write{
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &image_info,
    };
    const vk::Extent2D extent{output.width, output.height};
    const vk::RenderingAttachmentInfo attachment{
        .imageView = output.image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    cmdbuf.setViewport(0, vk::Viewport{.width = float(extent.width),
                                       .height = float(extent.height),
                                       .minDepth = 0.0f,
                                       .maxDepth = 1.0f});
    cmdbuf.setScissor(0, vk::Rect2D{.extent = extent});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, set_write);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(float),
                         &sharpness);
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {.extent = extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();
    const vk::ImageMemoryBarrier2 post_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output.image,
        .subresourceRange = kColor,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &post_barrier});
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
}

} // namespace Vulkan::HostPasses
