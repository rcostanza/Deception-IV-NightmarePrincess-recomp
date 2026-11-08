// SPDX-License-Identifier: GPL-2.0-or-later
// deception: frame analyzer. Creating the file named by DECEPTION_CAPTURE_TRIGGER records the next full
// frame: every render pass (targets, depth, draw count, shaders, sampled textures) and compute
// dispatch, in order, plus the buffer that was presented: how the game builds its frame.

#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "common/types.h"

namespace VideoCore {
struct ImageInfo;
}

namespace Vulkan {

class FrameCapture {
public:
    /// VideoOut thread, per flip: frame boundary, presented buffer, trigger file check.
    static void OnFlip(VAddr presented_address);

    /// VideoOut: a display buffer the game registered. A pass drawing into one starts a frame
    /// in the GPU command stream (the flip itself runs a frame behind).
    static void AddDisplayBuffer(VAddr address);
    static bool IsDisplayBuffer(VAddr address);

    /// GPU thread: whether passes are being looked at (armed or recording; cheap).
    static bool Active() {
        return state.load(std::memory_order_relaxed) != Idle;
    }
    /// GPU thread: whether a frame is being recorded right now.
    static bool IsRecording() {
        return state.load(std::memory_order_relaxed) == Recording;
    }
    /// GPU thread, at every draw/dispatch: starts or ends a recording at frame boundaries.
    static void Poll();

    /// `state`: the pass's census line (vk_pass_census.h), kept with the first pipeline that
    /// opened the pass.
    /// `asked`/`views`: per color target, the format the guest's registers ask for and the
    /// format of the view bound (vk::Format values; 0 = not known).
    static void BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                          const VideoCore::ImageInfo* depth, std::string census,
                          const u32* asked = nullptr, const u32* views = nullptr);
    /// `detail`: blend and depth state, listed with the draw (the first draws of each pass).
    static void Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances,
                     const char* detail = "");
    /// GPU thread: a draw the game issued that the renderer did not record, and why. Listed in
    /// the pass that was open at the time, so a missing object shows whether it was ever drawn.
    static void Dropped(const char* reason, u64 vs_hash, u64 ps_hash, u32 num_indices);
    static void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);
    /// GPU thread: a dispatch the renderer did not record, and why (counted with dropped draws).
    static void DroppedDispatch(const char* reason, u64 cs_hash, u32 x, u32 y, u32 z);
    /// `asked`: the format of the guest's descriptor, `view`: of the view bound (vk::Format).
    static void Sampled(const VideoCore::ImageInfo& info, bool storage, u32 asked = 0, u32 view = 0);
    /// Contents of a bound buffer (first 1 KiB), kept for small passes and a pass's first draw.
    static void Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size);
    static void Note(const char* text);

    /// In-game menu: records the next full frame (as the trigger file does).
    static void Request();
    /// What the last request did, for the menu: empty, "Capturing...", the file or an error.
    static std::string LastResult();

private:
    enum : u32 { Idle, Armed, Recording };
    static inline std::atomic<u32> state{Idle};
    static inline std::atomic<u64> flips{0};
    static inline std::atomic<VAddr> last_presented{0};
    static inline std::atomic<bool> requested{false};
};

} // namespace Vulkan
