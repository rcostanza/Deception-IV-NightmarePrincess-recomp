// SPDX-License-Identifier: GPL-2.0-or-later
// deception: FSR 1 RCAS sharpening of the presented picture (key sharpening). The post process pass
// draws into an output-sized image this pass owns, and RCAS draws that into the frame.
#pragma once

#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {
struct Frame;
class Scheduler;
} // namespace Vulkan

namespace Vulkan::HostPasses {

class RcasPass {
public:
    /// FSR's sharpness: exp2(-0.25 stops), the attenuation its samples default to.
    static constexpr float DefaultSharpness = 0.8409f;

    RcasPass();
    ~RcasPass();

    void Create(vk::Device device, VmaAllocator allocator, vk::Format surface_format);

    /// The image post processing draws into instead of `output` (of its size and format), in
    /// ColorAttachmentOptimal. A replaced image is freed once the GPU is past `scheduler`'s tick.
    Frame& Input(vk::CommandBuffer cmdbuf, Scheduler& scheduler, const Frame& output);

    /// Sharpens Input() (in General, as the post process pass leaves it) into `output`, which is
    /// in ColorAttachmentOptimal and is left in General with its writes visible to fragment reads.
    void Render(vk::CommandBuffer cmdbuf, Frame& output, float sharpness);

private:
    vk::Device device{};
    VmaAllocator allocator{};
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
    std::unique_ptr<Frame> input; ///< image, view and size only; no fence or semaphore
    VmaAllocation input_allocation{};
};

} // namespace Vulkan::HostPasses
