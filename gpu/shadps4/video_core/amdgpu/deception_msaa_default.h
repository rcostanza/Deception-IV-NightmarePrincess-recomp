// SPDX-License-Identifier: GPL-2.0-or-later
// deception: the Vulkan drivers where the game's multisampled images are created single-sampled
// unless DECEPTION_NO_MSAA says otherwise (deception_msaa.h). vk_instance.cpp decides with it.
//
// AMD's Windows driver: the RX 6600 family reads the 2-sample depth buffer wrong and the
// lighting shows black lines. Intel's Windows driver: the same lines on Arc A750, B580
// and 140V. NVIDIA, RADV, AMDVLK, Mesa ANV and MoltenVK read it correctly as far as
// known and keep the game's multisampling.
#pragma once

#include <vulkan/vulkan_core.h>

static inline int deception_no_msaa_default(VkDriverId driver) {
    return driver == VK_DRIVER_ID_AMD_PROPRIETARY || driver == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS;
}
