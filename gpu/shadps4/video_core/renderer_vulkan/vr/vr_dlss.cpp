// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vr/vr_dlss.h"

#include <algorithm>
#include <span>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "video_core/host_shaders/drive_aa_copy_comp.h"
#include "video_core/host_shaders/vr_sharpen_comp.h"
#include "video_core/host_shaders/vr_sharpen_r11_comp.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_drive_motion.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vr/vr_fixes.h"
#include "video_core/renderer_vulkan/vr/vr_multiview.h"
#include "video_core/renderer_vulkan/vr/vr_object_motion.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "shader_recompiler/info.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::Vr::Dlss {

namespace {

struct CachedView {
    vk::Image image;
    u64 uid = 0;
    vk::Format format{};
    vk::ImageAspectFlags aspect{};
    u32 layer = 0;
    vk::ImageView view;
    u64 last_use = 0;
};

struct State {
    bool logged_unavailable = false;
    u32 width = 0, height = 0, out_width = 0, out_height = 0;
    VideoCore::UniqueImage motion_image, result_image;
    std::array<vk::UniqueImageView, 2> motion_views, result_views; // one layer each
    vk::UniqueImageView result_array;                              // both layers (what the game's shaders read)
    // Both layers with the swizzle of the game's view they stand in for (made on first use, per result image).
    vk::ComponentMapping redirect_mapping{};
    vk::UniqueImageView redirect_view;
    const Instance* instance = nullptr;
    bool result_ready = false;
    std::array<CachedView, 16> views{};
    u64 view_uses = 0;
    vk::UniqueDescriptorSetLayout copy_desc_layout;
    vk::UniquePipelineLayout copy_pipeline_layout;
    vk::UniquePipeline copy_pipeline;
    // Sharpening after DLSS (BB_VR_DLSS_SHARPNESS 0..1, the launcher's row; DLSS 3.1+ ignores its own): into the lit
    // scene (DLAA) or into a second picture the game's passes read (upscaling).
    vk::UniquePipeline sharpen_pipeline, sharpen_r11_pipeline;
    VideoCore::UniqueImage sharp_image;
    std::array<vk::UniqueImageView, 2> sharp_views;
    vk::Image redirect_image{}; // the picture redirect_view shows
    std::chrono::steady_clock::time_point last_frame{};
    // The main view's camera: the latest frame's (from the G-buffer draws) and the one the last motion used.
    std::mutex camera_mutex;
    std::array<float, 16> view_proj{}, used_view_proj{};
    std::array<float, 3> position{}, used_position{};
    bool have_camera = false, have_used = false;
    // Armed by the main view's camera (its G-buffer draws), spent by the frame's DLSS.
    std::atomic<bool> armed{false};
    VideoCore::ImageId lit{}; // the frame's lit scene (the rasterizer's thread)
    VideoCore::ImageId composite{}; // drawn by the last draw, DLSS at this one (AaInput)
    bool composite_seen = false;    // the game draws the composite (else DLSS stays on the lit scene)
} g;

using Mat = std::array<float, 16>;

Mat Multiply(const Mat& a, const Mat& b) {
    Mat out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = sum;
        }
    }
    return out;
}

bool Invert(const Mat& m, Mat& out) {
    // Gauss-Jordan with partial pivoting (double precision: the projection's entries span 1e-2 to 1e0).
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[r * 4 + c];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) {
            if (std::abs(a[r][c]) > std::abs(a[pivot][c])) {
                pivot = r;
            }
        }
        if (std::abs(a[pivot][c]) < 1e-12) {
            return false;
        }
        std::swap(a[c], a[pivot]);
        const double inv = 1.0 / a[c][c];
        for (double& v : a[c]) {
            v *= inv;
        }
        for (int r = 0; r < 4; ++r) {
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = float(a[r][c + 4]);
        }
    }
    return true;
}

// This frame's clip to the previous frame's (row vectors): back to camera-relative space, across the camera's move,
// into the previous view-projection. Identity until two frames are known.
Mat ClipToPrevious() {
    Mat identity{};
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
    std::scoped_lock lock{g.camera_mutex};
    if (!g.have_camera) {
        return identity;
    }
    Mat current_inverse{};
    const bool ok = g.have_used && Invert(g.view_proj, current_inverse);
    Mat out = identity;
    if (ok) {
        Mat move = identity; // (current - previous) camera position: a row-vector translation
        for (int i = 0; i < 3; ++i) {
            move[12 + i] = g.position[i] - g.used_position[i];
        }
        out = Multiply(Multiply(current_inverse, move), g.used_view_proj);
    }
    g.used_view_proj = g.view_proj;
    g.used_position = g.position;
    g.have_used = true;
    return out;
}

// A one-layer view of a stereo image (the views are kept: a frame uses six).
vk::ImageView LayerView(const Instance& instance, Scheduler& scheduler, const VideoCore::Image& image,
                        vk::Format format, vk::ImageAspectFlags aspect, u32 layer) {
    const vk::Image handle = image.GetImage();
    CachedView* oldest = &g.views[0];
    for (auto& entry : g.views) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid && entry.format == format &&
            entry.aspect == aspect && entry.layer == layer) {
            entry.last_use = ++g.view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .layer = layer,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, layer, 1},
        })),
        .last_use = ++g.view_uses,
    };
    return oldest->view;
}

void CreateCopyPipeline(const Instance& instance) {
    if (g.copy_pipeline) {
        return;
    }
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {{
        {.binding = 0, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    g.copy_desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0,
                                      .size = 3 * sizeof(float)}; // the copy's size, the sharpening's strength
    g.copy_pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*g.copy_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto make = [&](std::span<const u32> code, vk::UniquePipeline& pipeline) {
        const auto module = CompileSPV(code, device);
        pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
                    .layout = *g.copy_pipeline_layout,
                }));
        device.destroyShaderModule(module);
    };
    make(DRIVE_AA_COPY_COMP, g.copy_pipeline);
    make(VR_SHARPEN_COMP, g.sharpen_pipeline);
    make(VR_SHARPEN_R11_COMP, g.sharpen_r11_pipeline);
}

float Sharpness() {
    static const float value = [] {
        // Development only (BB_VR_RCAS_SHARPNESS, 0..10; above 1 RCAS's strongest change scaled, vr_sharpen.comp):
        // the launcher's sharpening is LumaSharpen on the final picture now (vr_stereo.cpp, Matt 2026-10-22).
        const char* v = std::getenv("BB_VR_RCAS_SHARPNESS");
        return v ? std::clamp(float(std::atof(v)), 0.0f, 10.0f) : 0.0f;
    }();
    return value;
}

bool EnsureImages(const Instance& instance, Scheduler& scheduler, u32 w, u32 h, u32 ow, u32 oh) {
    if (g.motion_image && w == g.width && h == g.height && ow == g.out_width && oh == g.out_height) {
        return true;
    }
    scheduler.Finish();
    g.result_ready = false;
    const auto device = instance.GetDevice();
    const auto make = [&](VideoCore::UniqueImage& image, std::array<vk::UniqueImageView, 2>& views,
                          vk::UniqueImageView* array, vk::Format format, vk::ImageUsageFlags usage, u32 iw, u32 ih) {
        for (auto& view : views) {
            view.reset();
        }
        if (array) {
            array->reset();
        }
        image = VideoCore::UniqueImage(device, instance.GetAllocator());
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {iw, ih, 1},
            .mipLevels = 1,
            .arrayLayers = 2,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        for (u32 layer = 0; layer < 2; ++layer) {
            views[layer] = Check(device.createImageViewUnique({
                .image = vk::Image(image),
                .viewType = vk::ImageViewType::e2D,
                .format = format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, layer, 1},
            }));
        }
        if (array) {
            *array = Check(device.createImageViewUnique({
                .image = vk::Image(image),
                .viewType = vk::ImageViewType::e2DArray,
                .format = format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2},
            }));
        }
    };
    make(g.motion_image, g.motion_views, nullptr, vk::Format::eR16G16Sfloat,
         vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled, w, h);
    make(g.result_image, g.result_views, &g.result_array, vk::Format::eR16G16B16A16Sfloat,
         vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
         ow, oh);
    if (Sharpness() > 0.0f && (ow != w || oh != h)) {
        make(g.sharp_image, g.sharp_views, nullptr, vk::Format::eR16G16B16A16Sfloat,
             vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled, ow, oh);
    }
    g.redirect_view.reset();
    g.instance = &instance;
    g.width = w;
    g.height = h;
    g.out_width = ow;
    g.out_height = oh;
    CreateCopyPipeline(instance);
    std::printf("VR DLSS: %ux%u -> %ux%u per eye%s\n", w, h, ow, oh, w == ow ? " (DLAA)" : "");
    std::fflush(stdout);
    return true;
}

} // namespace

bool Active() {
    return Multiview::Enabled();
}

void OnDispatch(VideoCore::TextureCache& texture_cache, const Shader::Info& cs, const VideoCore::ImageId* images,
                size_t count) {
    if (cs.pgm_hash != 0x2c918c06ULL || !Active()) {
        return;
    }
    u32 width = 0, height = 0;
    if (!Fixes::RenderSize(width, height)) {
        return;
    }
    for (size_t i = 0; i < count && i < cs.images.size(); ++i) {
        if (!images[i] || !cs.images[i].is_written) {
            continue;
        }
        const auto& image = texture_cache.GetImage(images[i]);
        if (image.info.pixel_format == vk::Format::eB10G11R11UfloatPack32 && image.info.size.width == width &&
            image.info.size.height == height) {
            g.lit = images[i];
        }
    }
}

VideoCore::ImageId AaInput(u64 vs_hash, u64 ps_hash, VideoCore::ImageId color0) {
    static const bool at_lit = std::getenv("BB_DLSS_AT") && std::string_view(std::getenv("BB_DLSS_AT")) == "lit";
    if (!at_lit && ps_hash == 0x9eff1777ULL) {
        g.composite_seen = true;
    }
    if (!at_lit && g.composite_seen) {
        if (!g.lit || !g.armed.load(std::memory_order_relaxed) || !Active()) {
            return {};
        }
        if (ps_hash == 0x9eff1777ULL) {
            g.composite = color0;
            return {};
        }
        return std::exchange(g.composite, VideoCore::ImageId{});
    }
    if (vs_hash != 0xb834cdf9ULL || !g.lit || !g.armed.load(std::memory_order_relaxed) || !Active()) {
        return {};
    }
    return g.lit;
}

void OnCameraConstants(const float* constants) {
    u32 width = 0, height = 0;
    if (!Active() || !Fixes::RenderSize(width, height) || constants[96] != float(width) ||
        constants[97] != float(height)) {
        return; // the mirror's, a reflection's
    }
    std::scoped_lock lock{g.camera_mutex};
    std::memcpy(g.view_proj.data(), constants, sizeof(g.view_proj));
    std::memcpy(g.position.data(), constants + 108, sizeof(g.position));
    g.have_camera = true;
    g.armed.store(true, std::memory_order_relaxed);
}

bool Record(const Instance& instance, Scheduler& scheduler, Runtime& runtime, DriveMotion& motion,
            Vulkan::Dlss& dlss, const Inputs& in) {
    auto& color = *in.color;
    auto& depth = *in.depth;
    auto& output = *in.output;
    g.armed.store(false, std::memory_order_relaxed); // once a frame (sizes already match: the main view's pass)
    if (!Multiview::Stereo(color) || !Multiview::Stereo(depth) || !Multiview::Stereo(output)) {
        return false; // a mono pass (a menu's 3D): the next stereo frame starts over
    }
    const u32 w = color.info.size.width, h = color.info.size.height;
    u32 ow = w, oh = h;
    if (in.upscale && (!Fixes::OutputSize(ow, oh) || ow < w || oh < h)) {
        ow = w;
        oh = h;
    }
    if (!dlss.SelectFeature(1) || !dlss.SelectFeature(0)) {
        if (!g.logged_unavailable) {
            g.logged_unavailable = true;
            std::puts("VR DLSS: the DLSS bridge has one feature only (ABI 3 is needed for two eyes)");
        }
        return false;
    }
    if (!EnsureImages(instance, scheduler, w, h, ow, oh)) {
        return false;
    }
    const auto color_format = color.info.pixel_format;
    const auto depth_format = depth.info.pixel_format;
    // The motion pass's mask: each eye's stencil, where the game marks the player's car, the hands and the other cars
    // (bits 0x82, as frame generation reads them). Without it they took the world's motion from the camera: the car
    // you sit in moves with the camera, yet its pixels were said to move at the car's speed, and DLSS smeared the
    // cockpit and the bonnet while driving. Masked pixels keep their place (head turns in the car are the error left).
    // BB_VR_DLSS_CAR_MASK=0 turns it off.
    static const bool car_mask =
        !(std::getenv("BB_VR_DLSS_CAR_MASK") && std::getenv("BB_VR_DLSS_CAR_MASK")[0] == '0');
    const bool has_stencil = depth_format == vk::Format::eD32SfloatS8Uint ||
                             depth_format == vk::Format::eD24UnormS8Uint || depth_format == vk::Format::eD16UnormS8Uint;
    std::array<vk::ImageView, 2> color_views, depth_views, out_views, mask_views;
    for (u32 eye = 0; eye < 2; ++eye) {
        color_views[eye] = LayerView(instance, scheduler, color, color_format, vk::ImageAspectFlagBits::eColor, eye);
        depth_views[eye] = LayerView(instance, scheduler, depth, depth_format, vk::ImageAspectFlagBits::eDepth, eye);
        mask_views[eye] = car_mask && has_stencil ? LayerView(instance, scheduler, depth, depth_format,
                                                              vk::ImageAspectFlagBits::eStencil, eye)
                                                  : depth_views[eye];
        out_views[eye] = LayerView(instance, scheduler, output, vk::Format::eB10G11R11UfloatPack32,
                                   vk::ImageAspectFlagBits::eColor, eye);
    }

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&output, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    const auto barrier = [&](vk::Image image, vk::ImageLayout old_layout, vk::PipelineStageFlags2 src_stage,
                             vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                             vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    barrier(vk::Image(g.motion_image), vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    barrier(vk::Image(g.result_image), vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone, all, rw);

    // The cars' own motion where their draws wrote it this frame (vr_object_motion.h), an eye per layer.
    std::array<vk::ImageView, 2> object_views{};
    if (auto* objects = ObjectMotion::g_object_motion; objects && objects->Enabled()) {
        bool valid = false;
        objects->PrepareRead(cmdbuf, w, h, valid);
        if (valid && objects->Layers() == 2) {
            object_views = {objects->LayerView(0), objects->LayerView(1)};
        }
    }
    // Camera motion per eye: each eye's pixels are the centre view's shifted by its clip shift (left +, right -).
    const float shift = Multiview::Shift();
    const Mat clip_to_prev = ClipToPrevious();
    for (u32 eye = 0; eye < 2; ++eye) {
        motion.Record(cmdbuf, depth_views[eye], mask_views[eye], *g.motion_views[eye], w, h, w, h, false,
                      eye == 0 ? shift : -shift, clip_to_prev.data(), object_views[eye]);
    }
    barrier(vk::Image(g.motion_image), vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderWrite, all, vk::AccessFlagBits2::eShaderRead);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - g.last_frame).count();
    if (!(frame_ms > 0.0f && frame_ms < 200.0f)) {
        frame_ms = 11.1f;
    }
    g.last_frame = now;

    const float scale = float(ow) / float(w);
    const Vulkan::Dlss::FeatureDesc desc{w, h, ow, oh, ow == w ? 0 : Vulkan::Dlss::QualityForScale(scale), true, true};
    for (u32 eye = 0; eye < 2; ++eye) {
        dlss.SelectFeature(eye);
        bool reset = in.reset;
        if (!dlss.HasFeature(desc)) {
            scheduler.WaitSubmitted();
            dlss.ReleaseFeature();
            if (!dlss.CreateFeature(cmdbuf, desc)) {
                dlss.SelectFeature(0);
                std::puts("VR DLSS: the feature could not be made");
                return false;
            }
            reset = true;
        }
        const bool ok = dlss.Evaluate(
            cmdbuf, {.color = {color.GetImage(), color_views[eye], color_format, vk::ImageAspectFlagBits::eColor, w,
                               h, eye},
                     .depth = {depth.GetImage(), depth_views[eye], depth_format, vk::ImageAspectFlagBits::eDepth, w,
                               h, eye},
                     .motion = {vk::Image(g.motion_image), *g.motion_views[eye], vk::Format::eR16G16Sfloat,
                                vk::ImageAspectFlagBits::eColor, w, h, eye},
                     .output = {vk::Image(g.result_image), *g.result_views[eye], vk::Format::eR16G16B16A16Sfloat,
                                vk::ImageAspectFlagBits::eColor, ow, oh, eye},
                     .jitter_x = in.jitter[0],
                     .jitter_y = in.jitter[1],
                     .reset = reset,
                     .frame_ms = frame_ms,
                     .sharpness = in.sharpness});
        if (!ok) {
            dlss.SelectFeature(0);
            std::puts("VR DLSS: evaluation failed");
            return false;
        }
    }
    dlss.SelectFeature(0);

    // Each eye from `src` into `dst` with `pipeline` (the copy, or the sharpening at Sharpness()).
    const float sharpness = Sharpness();
    const auto per_eye = [&](vk::Pipeline pipeline, const std::array<vk::ImageView, 2>& src,
                             const std::array<vk::ImageView, 2>& dst, u32 dw, u32 dh) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        const std::array<float, 3> push{float(dw), float(dh), sharpness};
        for (u32 eye = 0; eye < 2; ++eye) {
            const vk::DescriptorImageInfo src_info{.imageView = src[eye], .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo dst_info{.imageView = dst[eye], .imageLayout = vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 2> writes = {{
                {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = &src_info},
                {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = &dst_info},
            }};
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *g.copy_pipeline_layout, 0, writes);
            cmdbuf.pushConstants(*g.copy_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                                 push.data());
            cmdbuf.dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
        }
    };
    const std::array<vk::ImageView, 2> result_views{*g.result_views[0], *g.result_views[1]};

    if (ow != w || oh != h) {
        // Upscaled: what samples the game's picture by UV from here on reads both eyes of this one (DriveAA), or of
        // its sharpened copy.
        if (sharpness > 0.0f && g.sharp_image) {
            barrier(vk::Image(g.result_image), vk::ImageLayout::eGeneral, all, rw,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
            barrier(vk::Image(g.sharp_image), vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
            per_eye(*g.sharpen_pipeline, result_views, {*g.sharp_views[0], *g.sharp_views[1]}, ow, oh);
        }
        const vk::MemoryBarrier2 ready{.srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                       .dstStageMask = all, .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &ready});
        g.result_ready = true;
        return true;
    }
    // DLAA: the reconstructed eyes replace the picture (in place: DLSS has read it, the copy writes it).
    barrier(vk::Image(g.result_image), vk::ImageLayout::eGeneral, all, rw, vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderRead);
    const vk::MemoryBarrier2 read_done{.srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
                                       .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                       .dstAccessMask = vk::AccessFlagBits2::eShaderWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &read_done});
    per_eye(sharpness > 0.0f ? *g.sharpen_r11_pipeline : *g.copy_pipeline, result_views, out_views, w, h);
    const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                  .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                  .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                  .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
    return true;
}

bool Redirect(vk::ImageView& view, vk::ImageLayout& layout, vk::ComponentMapping mapping) {
    if (!Active() || !g.result_ready || !g.result_array || !g.instance) {
        return false;
    }
    const vk::Image image = Sharpness() > 0.0f && g.sharp_image ? vk::Image(g.sharp_image) : vk::Image(g.result_image);
    if (!g.redirect_view || mapping != g.redirect_mapping || image != g.redirect_image) {
        // The old view may be in a command buffer being recorded: kept until the image is remade (one mapping in
        // practice).
        static std::vector<vk::UniqueImageView> retired;
        if (g.redirect_view) {
            retired.push_back(std::move(g.redirect_view));
        }
        g.redirect_mapping = mapping;
        g.redirect_image = image;
        g.redirect_view = Check(g.instance->GetDevice().createImageViewUnique({
            .image = image,
            .viewType = vk::ImageViewType::e2DArray,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .components = mapping,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2},
        }));
    }
    view = *g.redirect_view;
    layout = vk::ImageLayout::eGeneral;
    return true;
}

} // namespace Vulkan::Vr::Dlss
