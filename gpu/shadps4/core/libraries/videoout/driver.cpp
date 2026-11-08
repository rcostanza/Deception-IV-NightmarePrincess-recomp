// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <thread>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "common/assert.h"
#include "deception_toggles.h"
#include "deception_settings.h"
#include "deception_overlay.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {
extern std::atomic<u64> g_deception_compile_ns;
extern std::atomic<u32> g_deception_compiles;
extern std::atomic<u64> g_deception_capture_ns, g_deception_async_ns, g_deception_compile_wait_ns;
extern std::atomic<u64> g_deception_snapshot_ns, g_deception_guest_wait_ns;
extern std::atomic<u32> g_deception_pending_draws, g_deception_build_waits;
extern std::atomic<u64> g_deception_build_wait_ns;
} // namespace Vulkan

namespace Libraries::VideoOut {

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    const char* separate = std::getenv("DECEPTION_PRESENT_THREAD");
    separate_swap = !(separate && separate[0] == '0');
    if (separate_swap) {
        swap_thread = std::jthread([&](std::stop_token token) { SwapThread(token); });
    }
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

void VideoOutDriver::RunPresenter(std::function<void()> work, bool if_idle) {
    if (!separate_swap) {
        work();
        return;
    }
    {
        std::scoped_lock lock{swap_mutex};
        if (if_idle && (swap_busy || !swap_queue.empty())) {
            return; // a redraw of the last frame is pointless while frames are queued
        }
        swap_queue.push_back(std::move(work));
    }
    swap_cv.notify_one();
}

void VideoOutDriver::SwapThread(std::stop_token token) {
    Common::SetCurrentThreadName("deception:Present");
    while (true) {
        std::function<void()> work;
        {
            std::unique_lock lock{swap_mutex};
            swap_busy = false;
            if (!swap_cv.wait(lock, token, [this] { return !swap_queue.empty(); })) {
                return;
            }
            work = std::move(swap_queue.front());
            swap_queue.pop_front();
            swap_busy = true;
        }
        work();
    }
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* params) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    liverpool->SetVoPort(&main_port);
    return 1;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};

    // Mark as closed
    main_port.is_open = false;
    main_port.flip_rate = 0;
    main_port.prev_index = -1;

    // Clear port information
    std::memset(main_port.buffer_labels.data(), 0, sizeof(main_port.buffer_labels));
    std::memset(main_port.groups.data(), 0, sizeof(main_port.groups));
    std::memset(&main_port.vblank_status, 0, sizeof(main_port.vblank_status));
    main_port.flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(main_port.buffer_slots.data(), 0, sizeof(main_port.buffer_slots));
    for (auto& buffer : main_port.buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : main_port.flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.flip_events.clear();
    for (auto event : main_port.vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    // deception: the size the game draws its frames at, for the log.
    std::printf("VideoOut: game registered %d display buffer(s) of %ux%u (%s)\n", bufferNum,
                attribute->width, attribute->height, GetPixelFormatString(attribute->pixel_format).data());
    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };
        Vulkan::FrameCapture::AddDisplayBuffer(address);

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        presenter->RegisterVideoOutSurface(group, address);
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

// deception: frames handed to the presenter, for the loader's hang dump (deception_gpu_flips).
static std::atomic<u64> g_deception_flips_shown;

void VideoOutDriver::Flip(const Request& req) {
    g_deception_flips_shown.fetch_add(1, std::memory_order_relaxed);
    // Update HDR status before presenting, then present the frame (deception: on the swap thread).
    RunPresenter([this, frame = req.frame, hdr = req.port->is_hdr] {
        presenter->SetHDR(hdr);
        presenter->Present(frame);
    });
    Vulkan::FrameCapture::OnFlip(req.index >= 0 ? req.port->buffer_slots[req.index].address_left
                                                : 0);

    // deception: DECEPTION_FRAME_STATS=1 prints flip rate and frame time spread every 5 seconds.
    static const bool frame_stats = EmulatorSettingsImpl::Flag("DECEPTION_FRAME_STATS", false);
    if (frame_stats) {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point window_start = Clock::now(), last = window_start;
        static u32 frames;
        static double worst_ms;
        const auto now = Clock::now();
        const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count();
        worst_ms = std::max(worst_ms, frame_ms);
        static std::vector<double> intervals;
        intervals.push_back(frame_ms);
        // Where each flip lands in the display's refresh (Windows): a thread stamps every refresh.
        static std::atomic<s64> refresh_ns{0};
        static const bool refresh_stamps = [] {
            std::thread([] {
                Common::SetCurrentThreadName("deception:RefreshStamps");
                while (DeceptionWaitForVblank()) {
                    refresh_ns = Clock::now().time_since_epoch().count();
                }
            }).detach();
            return true;
        }();
        static std::vector<double> phases;
        if (refresh_stamps && refresh_ns.load()) {
            const double period_ms = EmulatorSettings.GetVblankPeriod().count() / 1e6;
            double phase = (now.time_since_epoch().count() - refresh_ns.load()) / 1e6;
            phase = std::fmod(phase + period_ms / 2, period_ms) - period_ms / 2;
            phases.push_back(phase);
        }
        last = now;
        // Stall diagnostics: what happened during a long frame.
        static u64 last_gpu_ns, last_images, last_image_bytes, last_buffer_bytes;
        static u64 last_t[6], last_minflt, last_sigf, last_pc, last_pp, last_rc, last_rp;
        const u64 pc = DeceptionStats::protect_calls.load(), pp = DeceptionStats::protect_pages.load(),
                  rc = DeceptionStats::protect_revoke_calls.load(), rp = DeceptionStats::protect_revoke_pages.load();
        const u64 minflt = DeceptionStats::gpu_minor_faults.load(), sigf = DeceptionStats::gpu_signal_faults.load();
        static u64 last_copy_cpu, last_copy_sys, last_copy_flt;
        const u64 copy_cpu = DeceptionStats::t_copy_cpu.load(), copy_sys = DeceptionStats::copy_sys_us.load(),
                  copy_flt = DeceptionStats::copy_minflt.load();
        static u64 last_proc_flt, last_copy_ns, last_copy_bytes, last_rf, last_trf, last_twf;
        const u64 rf = DeceptionStats::read_faults.load(), trf = DeceptionStats::t_read_faults.load(),
                  twf = DeceptionStats::t_write_faults.load();
        // deception: every frame over 40 ms (DECEPTION_STALL_MS) after the first, at most 10 per 5 s window.
        static const double stall_ms = [] {
            const char* value = std::getenv("DECEPTION_STALL_MS");
            return value ? std::atof(value) : 40.0;
        }();
        static bool primed = false;
        static u32 stalls_shown = 0;
        const bool stall = primed && frame_ms > stall_ms && stalls_shown < 10;
        stalls_shown += stall;
        primed = true;
        // Waits and work inside the long frame (window counters are reset every 5 s, so the
        // previous values are reset with them).
        static u64 last_wait[7];
        const u64 wait_now[7] = {DeceptionStats::t_readback_wait.load(), DeceptionStats::tick_wait_ns.load(),
                                 DeceptionStats::sync_recording_ns.load(), DeceptionStats::host_copies_wait_ns.load(),
                                 u64(Vulkan::g_deception_compile_ns.load()), u64(Vulkan::g_deception_compiles.load()),
                                 DeceptionStats::t_gc.load()};
        if (stall) {
            std::printf("Stall: %.1f ms frame; waits: readback %.1f ms, GPU ticks %.1f ms, "
                        "recorder %.1f ms, host copies %.1f ms; %llu compiles %.1f ms; GC %.1f ms\n",
                        frame_ms, (wait_now[0] - last_wait[0]) / 1e6, (wait_now[1] - last_wait[1]) / 1e6,
                        (wait_now[2] - last_wait[2]) / 1e6, (wait_now[3] - last_wait[3]) / 1e6,
                        static_cast<unsigned long long>(wait_now[5] - last_wait[5]),
                        (wait_now[4] - last_wait[4]) / 1e6, (wait_now[6] - last_wait[6]) / 1e6);
        }
        std::copy(std::begin(wait_now), std::end(wait_now), std::begin(last_wait));
        if (stall) {
            std::printf("       fault handlers: %llu read faults %.1f ms, write faults %.1f ms\n",
                        static_cast<unsigned long long>(rf - last_rf), (trf - last_trf) / 1e6,
                        (twf - last_twf) / 1e6);
        }
        last_rf = rf;
        last_trf = trf;
        last_twf = twf;
        const u64 copy_ns = DeceptionStats::t_copy.load(), copy_bytes = DeceptionStats::copy_bytes.load();
        u64 proc_flt = 0;
#ifndef _WIN32
        if (rusage usage{}; getrusage(RUSAGE_SELF, &usage) == 0) {
            proc_flt = usage.ru_minflt;
        }
#endif
        const u64 t_now[6] = {DeceptionStats::t_resident.load(), DeceptionStats::t_protect.load(),
                              DeceptionStats::t_image_create.load(), DeceptionStats::t_refresh.load(),
                              DeceptionStats::t_staging.load(), DeceptionStats::t_host_wait.load()};
        static u64 last_draws, last_dispatches, last_subs, last_sys, last_user, last_invol, last_vol;
        const u64 draws = DeceptionStats::draws.load(), dispatches = DeceptionStats::dispatches.load(),
                  subs = DeceptionStats::submissions.load(), sys_us = DeceptionStats::gpu_sys_us.load(),
                  user_us = DeceptionStats::gpu_user_us.load(), invol = DeceptionStats::gpu_invol_switches.load(),
                  vol = DeceptionStats::gpu_vol_switches.load();
        u64 gpu_ns = 0;
#ifdef _WIN32
        // deception: Windows has no per-thread clock id; the thread's user + kernel time instead.
        static HANDLE gpu_thread = nullptr;
        if (!gpu_thread) {
            if (const unsigned long id = DeceptionStats::gpu_thread_id.load()) {
                gpu_thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
            }
        }
        if (FILETIME created, exited, kernel, user;
            gpu_thread && GetThreadTimes(gpu_thread, &created, &exited, &kernel, &user)) {
            const auto ns = [](const FILETIME& t) {
                return ((u64(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100;
            };
            gpu_ns = ns(kernel) + ns(user);
        }
#else
        if (const int clock = DeceptionStats::gpu_thread_clock.load(); clock != -1) {
            timespec ts{};
            clock_gettime(static_cast<clockid_t>(clock), &ts);
            gpu_ns = u64(ts.tv_sec) * 1000000000ull + u64(ts.tv_nsec);
        }
#endif
        const u64 images = DeceptionStats::images_registered.load();
        const u64 image_bytes = DeceptionStats::image_upload_bytes.load();
        const u64 buffer_bytes = DeceptionStats::buffer_upload_bytes.load();
        if (stall) {
            std::printf("       GPU thread on CPU %.1f ms; %llu images registered, "
                        "%.1f MB image uploads, %.1f MB buffer uploads\n",
                        (gpu_ns - last_gpu_ns) / 1e6,
                        static_cast<unsigned long long>(images - last_images),
                        (image_bytes - last_image_bytes) / 1e6,
                        (buffer_bytes - last_buffer_bytes) / 1e6);
            std::printf("       %llu draws, %llu dispatches, %llu submissions; GPU thread user %.1f ms, "
                        "kernel %.1f ms, %llu preempted, %llu waits\n",
                        static_cast<unsigned long long>(draws - last_draws),
                        static_cast<unsigned long long>(dispatches - last_dispatches),
                        static_cast<unsigned long long>(subs - last_subs), (user_us - last_user) / 1e3,
                        (sys_us - last_sys) / 1e3, static_cast<unsigned long long>(invol - last_invol),
                        static_cast<unsigned long long>(vol - last_vol));
        }
        if (stall) {
            std::printf("       ms in: resident %.1f, protect %.1f, image create %.1f, "
                        "image refresh %.1f, staging %.1f, waiting for host copies %.1f\n",
                        (t_now[0] - last_t[0]) / 1e6, (t_now[1] - last_t[1]) / 1e6,
                        (t_now[2] - last_t[2]) / 1e6, (t_now[3] - last_t[3]) / 1e6,
                        (t_now[4] - last_t[4]) / 1e6, (t_now[5] - last_t[5]) / 1e6);
            std::printf("       GPU thread page faults %llu (process %llu), protection faults %llu; "
                        "protect calls %llu (%llu pages), of which write-revoking %llu (%llu pages)\n",
                        static_cast<unsigned long long>(minflt - last_minflt),
                        static_cast<unsigned long long>(proc_flt - last_proc_flt),
                        static_cast<unsigned long long>(sigf - last_sigf),
                        static_cast<unsigned long long>(pc - last_pc),
                        static_cast<unsigned long long>(pp - last_pp),
                        static_cast<unsigned long long>(rc - last_rc),
                        static_cast<unsigned long long>(rp - last_rp));
        }
        last_pc = pc;
        last_pp = pp;
        last_rc = rc;
        last_rp = rp;
        last_minflt = minflt;
        if (stall && copy_ns > last_copy_ns) {
            std::printf("       guest copies %.1f MB in %.1f thread-ms, %.1f ms on CPU (kernel %.1f ms, "
                        "%llu page faults in large copies) (%.2f GB/s per thread)\n",
                        (copy_bytes - last_copy_bytes) / 1e6, (copy_ns - last_copy_ns) / 1e6,
                        (copy_cpu - last_copy_cpu) / 1e6, (copy_sys - last_copy_sys) / 1e3,
                        static_cast<unsigned long long>(copy_flt - last_copy_flt),
                        double(copy_bytes - last_copy_bytes) / double(copy_ns - last_copy_ns));
        }
        last_copy_ns = copy_ns;
        last_copy_cpu = copy_cpu;
        last_copy_sys = copy_sys;
        last_copy_flt = copy_flt;
        last_copy_bytes = copy_bytes;
        last_proc_flt = proc_flt;
        last_sigf = sigf;
        std::copy(std::begin(t_now), std::end(t_now), std::begin(last_t));
        last_draws = draws;
        last_dispatches = dispatches;
        last_subs = subs;
        last_sys = sys_us;
        last_user = user_us;
        last_invol = invol;
        last_vol = vol;
        last_gpu_ns = gpu_ns;
        last_images = images;
        last_image_bytes = image_bytes;
        last_buffer_bytes = buffer_bytes;
        ++frames;
        const double window = std::chrono::duration<double>(now - window_start).count();
        if (window >= 5.0) {
            const u32 compiles = Vulkan::g_deception_compiles.exchange(0);
            const u64 compile_ns = Vulkan::g_deception_compile_ns.exchange(0);
            const double capture_ms = Vulkan::g_deception_capture_ns.exchange(0) / 1e6;
            const double async_ms = Vulkan::g_deception_async_ns.exchange(0) / 1e6;
            const double wait_ms = Vulkan::g_deception_compile_wait_ns.exchange(0) / 1e6;
            const double snapshot_ms = Vulkan::g_deception_snapshot_ns.exchange(0) / 1e6;
            const double guest_wait_ms = Vulkan::g_deception_guest_wait_ns.exchange(0) / 1e6;
            const u32 pending_draws = Vulkan::g_deception_pending_draws.exchange(0);
            const u32 build_waits = Vulkan::g_deception_build_waits.exchange(0);
            const double build_wait_ms = Vulkan::g_deception_build_wait_ns.exchange(0) / 1e6;
            const u64 direct = Vulkan::Scheduler::direct_recordings.exchange(0);
            const u64 faults = DeceptionStats::tracker_faults.exchange(0);
            // GPU command thread CPU time per draw: comparable between builds even when the scene
            // (and so the frame rate) differs a little.
            static u64 window_draws = 0, window_gpu_us = 0;
            const u64 all_draws = DeceptionStats::draws.load();
            const u64 gpu_us = DeceptionStats::gpu_user_us.load() + DeceptionStats::gpu_sys_us.load();
            const double us_per_draw = all_draws > window_draws
                                           ? double(gpu_us - window_gpu_us) / (all_draws - window_draws)
                                           : 0.0;
            const double draws_per_frame = frames ? double(all_draws - window_draws) / frames : 0.0;
            window_draws = all_draws;
            window_gpu_us = gpu_us;
            std::printf("Frame stats: %.1f FPS, worst frame %.1f ms (vblank %u Hz); "
                        "%u shader/pipeline compiles, %.1f ms; async: capture %.1f ms, workers "
                        "%.1f ms, sync wait %.1f ms, snapshots %.1f ms, GPU capture wait %.1f ms, "
                        "%u pending draws, %u build waits %.1f ms; %llu recorder syncs; "
                        "%.0f write faults/s (%.0f/s merged), %lld hot pages; GPU thread %.2f us/draw, "
                        "%.0f draws/frame, idle %.1f%%; blocked: recorder %.1f%%, host copies "
                        "%.1f%% (%.0f/frame), copy threads %.1f%%, GPU ticks %.1f%%\n",
                        frames / window, worst_ms, EmulatorSettings.GetVblankFrequency(), compiles,
                        compile_ns / 1e6, capture_ms, async_ms, wait_ms, snapshot_ms, guest_wait_ms,
                        pending_draws, build_waits, build_wait_ms,
                        static_cast<unsigned long long>(direct),
                        faults / window, DeceptionStats::merged_writes.exchange(0) / window,
                        static_cast<long long>(DeceptionStats::hot_pages.load()),
                        us_per_draw, draws_per_frame,
                        DeceptionStats::gpu_idle_ns.exchange(0) / (window * 1e7),
                        DeceptionStats::sync_recording_ns.exchange(0) / (window * 1e7),
                        DeceptionStats::host_copies_wait_ns.exchange(0) / (window * 1e7),
                        frames ? double(DeceptionStats::host_copy_waits.exchange(0)) / frames : 0.0,
                        DeceptionStats::copy_threads_wait_ns.exchange(0) / (window * 1e7),
                        DeceptionStats::tick_wait_ns.exchange(0) / (window * 1e7));
            presenter->GetRasterizer().GetPipelineCache().ReportCompilerStats();
            Vulkan::WaitDiagnostics::Report();
            // Frame pacing: spread of the guest flip intervals (judder that the mean hides).
            if (intervals.size() > 2) {
                std::vector<double> sorted = intervals;
                std::sort(sorted.begin(), sorted.end());
                const double median = sorted[sorted.size() / 2];
                double sum = 0, sq = 0;
                u32 spikes = 0;
                for (const double ms : intervals) {
                    sum += ms;
                    sq += ms * ms;
                    spikes += ms > 1.5 * median;
                }
                const double mean = sum / intervals.size();
                // The wall clock lines a window up with outside events (a popup, a reconnect).
                const std::time_t wall = std::time(nullptr);
                char clock[16] = "";
                if (const std::tm* local = std::localtime(&wall)) {
                    std::strftime(clock, sizeof(clock), "%H:%M:%S", local);
                }
                std::printf("Frame pacing (%s): median %.2f ms, stddev %.2f ms, p99 %.2f ms, "
                            "%u frames over 1.5x median\n",
                            clock, median, std::sqrt(std::max(0.0, sq / intervals.size() - mean * mean)),
                            sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)], spikes);
            }
            // Flip phase from the start of a refresh, wrapped to half a refresh either side: a
            // spread or a drift between windows means flips cross refreshes (judder at VSync).
            if (phases.size() > 2) {
                std::vector<double> sorted = phases;
                std::sort(sorted.begin(), sorted.end());
                std::printf("Refresh phase: median %+.2f ms, p1 %+.2f, p99 %+.2f ms\n", sorted[sorted.size() / 2],
                            sorted[sorted.size() / 100], sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)]);
            }
            phases.clear();
            intervals.clear();
            window_start = now;
            frames = 0;
            worst_ms = 0;
            stalls_shown = 0;
            std::fill(std::begin(last_wait) + 1, std::begin(last_wait) + 6, 0);
        }
    }

    // Update flip status.
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    RunPresenter([this] {
        const auto empty_frame = presenter->PrepareBlankFrame(true);
        presenter->Present(empty_frame, false, false);
    }, true);
}

void VideoOutDriver::DrawLastFrame() {
    RunPresenter([this] {
        const auto frame = presenter->PrepareLastFrame();
        if (frame != nullptr) {
            presenter->Present(frame, true);
        }
    }, true);
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop);
    }

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    {
        std::scoped_lock lock{mutex};
        requests.push({
            .frame = frame,
            .port = port,
            .flip_arg = flip_arg,
            .index = index,
            .eop = is_eop,
        });
    }
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    // deception: the vblank follows settings.ini frame_rate, which the Display Settings panel
    // changes while the game runs: the period, timer and display lock are redone at a change.
    std::chrono::nanoseconds vblank_period = EmulatorSettings.GetVblankPeriod();

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(vblank_period);

    Common::AccurateTimer timer{vblank_period};

    // deception: one flip per vblank (settings.ini frame_rate: 60 to 180; at 30 the game itself flips every second vblank) on any display.
    // Deception runs its game logic once per flip at its own target rate (set to match,
    // src/runtime_game.c), so following a faster display ran the game at double speed.
    int vblank_hz = int(EmulatorSettings.GetVblankFrequency());
    std::printf("VideoOut: vblank %.3f Hz\n", 1e9 / vblank_period.count());
    // deception: DECEPTION_VBLANK_HZ keeps the vblank off frame_rate while the engine still follows
    // it: any other rate changes the game's speed. Said loudly, since an environment variable
    // left from a measurement outlives it.
    if (const char* value = std::getenv("DECEPTION_VBLANK_HZ"); value && value[0]) {
        std::printf("VideoOut: WARNING: DECEPTION_VBLANK_HZ=%s overrides frame_rate; the game runs at "
                    "another speed unless they match (measurements only)\n", value);
    }

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };

    // deception: with VSync on and one flip per vblank, a tick starts with the display's own
    // refresh (DeceptionWaitForVblank) instead of the timer at its nominal rate: the timer's
    // flips drifted ~0.04 ms/s against the real refresh (crossing it every few minutes) with a
    // 0.5 ms spread, so the guest's frame times wavered; locked, the spread is 0.04 ms and the
    // phase stays within 0.1 ms of the refresh. A display at another rate than the vblank, or
    // a wait that fails or returns at once (minimized), keeps the timer. DECEPTION_VBLANK_TIMER=1
    // always uses it (measurements).
    const bool follow_display_at_start = !EmulatorSettingsImpl::Flag("DECEPTION_VBLANK_TIMER", false) &&
                                    std::getenv("DECEPTION_VBLANK_HZ") == nullptr;
    bool follow_display = follow_display_at_start && std::abs(DeceptionWindowRefreshRate() - vblank_hz) < 0.5;
    bool on_display = false;
    auto last_tick = std::chrono::steady_clock::now();

    while (!token.stop_requested()) {
        if (const int hz = int(EmulatorSettings.GetVblankFrequency()); hz != vblank_hz) {
            vblank_hz = hz;
            vblank_period = EmulatorSettings.GetVblankPeriod();
            timer = Common::AccurateTimer{vblank_period};
            // The timer from here on: locked to a variable-refresh display again after a change
            // (60 to 120 on a 120 Hz G-Sync panel), the display followed our flips and the two
            // settled at 10.3 ms frames (97 FPS, the game at 81% speed); the timer held 120.0.
            follow_display = false;
            on_display = false;
            std::printf("VideoOut: vblank %.3f Hz (frame_rate %d)\n", 1e9 / vblank_period.count(), hz);
        }
        bool display_tick = follow_display && DeceptionSettings::Get().vsync &&
                            DeceptionWaitForVblank();
        if (display_tick && std::chrono::steady_clock::now() - last_tick < vblank_period / 2) {
            display_tick = false; // no refresh to wait for: the timer keeps the rate
        }
        if (!display_tick) {
            if (on_display) {
                timer = Common::AccurateTimer{vblank_period};
            }
            timer.Start();
        }
        on_display = display_tick;
        last_tick = std::chrono::steady_clock::now();

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;
        if (vblank_status.count % (main_port.flip_rate + 1) == 0) {
            const auto request = receive_request();
            if (!request) {
                if (display_tick || timer.GetTotalWait().count() < 0) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                Flip(request);
                FRAME_END;
            }
        }

        {
            // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
            std::scoped_lock lock{main_port.vo_mutex};

            // Trigger flip events for the port
            for (auto event : main_port.vblank_events) {
                auto equeue = Kernel::GetEqueue(event);
                if (equeue != nullptr) {
                    equeue->TriggerEvent(
                        static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                        Kernel::OrbisKernelEvent::Filter::VideoOut,
                        reinterpret_cast<void*>(
                            static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                            (vblank_status.count << 16)));
                }
            }

            // Update vblank status
            vblank_status.count++;
            vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
            vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
            main_port.vblank_cv.notify_all();
        }

        timer.End();
    }
}

} // namespace Libraries::VideoOut

// deception: frames shown since start (the loader's hang dump watches it).
extern "C" unsigned long long deception_gpu_flips(void) {
    return Libraries::VideoOut::g_deception_flips_shown.load(std::memory_order_relaxed);
}
