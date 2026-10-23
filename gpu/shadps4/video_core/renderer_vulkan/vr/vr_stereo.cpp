// SPDX-License-Identifier: GPL-2.0-or-later
// VR (dcport-vr): stereo from the centre view. See vr_stereo.h.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "video_core/host_shaders/vr_luma_sharpen_comp.h"
#include "video_core/host_shaders/vr_stereo_mirror_comp.h"
#include "video_core/host_shaders/vr_stereo_resolve_comp.h"
#include "video_core/host_shaders/vr_stereo_splat_comp.h"
#include "video_core/amdgpu/regs_color.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/ui_composition.h"
#include "video_core/renderer_vulkan/vr/vr_camera.h"
#include "video_core/renderer_vulkan/vr/vr_multiview.h"
#include "video_core/renderer_vulkan/vr/vr_openxr.h"
#include "video_core/renderer_vulkan/vr/vr_stereo.h"
#include "video_core/renderer_vulkan/vr/vr_dlss.h"
#include "video_core/renderer_vulkan/vr/vr_fixes.h"
#include "video_core/texture_cache/texture_cache.h"
#include <vulkan/vulkan_format_traits.hpp>

namespace Vulkan::Vr::Stereo {
namespace {

#define VKD VULKAN_HPP_DEFAULT_DISPATCHER

void Log(const char* text) {
    std::printf("VR stereo: %s\n", text);
    std::fflush(stdout);
}

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkFormat format{};
    u32 width = 0, height = 0;
};

struct Slot {
    Image scene; // RGBA8 UNORM: the bits of the game's scene (gamma-encoded)
    Image depth; // the scene depth's format (sampled through its depth aspect)
    Image hud;   // RGBA8 UNORM: the HUD over transparent black
    Image scene_right; // multiview: the right eye's scene (layer 1 of the display buffer)
    bool has_right = false;
    u32 width = 0, height = 0;
    bool has_depth = false;
    bool has_hud = false;
    bool has_pose = false; // the pose this frame was rendered with (OnFrameCamera)
    float layer_pose[7];
    float tangents[4];
};

// The headset's thread reads a slot on its own queue while the game fills others: four, so the slot it reads is not
// refilled for three game frames.
constexpr u32 SlotCount = 4;

struct State {
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    bool ready = false;
    Slot slots[SlotCount];
    u32 next_slot = 0;
    s32 open_slot = -1; // the frame whose UI phase is running
    std::deque<u32> complete; // frames ready for the presenter, oldest first
    std::mutex mutex;

    Image keys[2], eyes[2], mirror;
    VkSampler sampler{};
    VkDescriptorSetLayout splat_layout{}, resolve_layout{}, mirror_layout{};
    VkPipelineLayout splat_pipeline_layout{}, resolve_pipeline_layout{}, mirror_pipeline_layout{};
    VkPipeline splat{}, resolve{}, mirror_pipeline{};
    VkDescriptorSetLayout sharpen_layout{}; // LumaSharpen on each eye (vr_luma_sharpen.comp)
    VkPipelineLayout sharpen_pipeline_layout{};
    VkPipeline sharpen{};
};

State g;

u32 MemoryType(u32 bits) {
    VkPhysicalDeviceMemoryProperties memory{};
    VKD.vkGetPhysicalDeviceMemoryProperties(g.physical_device, &memory);
    for (u32 type = 0; type < memory.memoryTypeCount; ++type) {
        if ((bits & (1u << type)) && (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return type;
        }
    }
    return 0;
}

void Destroy(Image& image) {
    if (image.view) VKD.vkDestroyImageView(g.device, image.view, nullptr);
    if (image.image) VKD.vkDestroyImage(g.device, image.image, nullptr);
    if (image.memory) VKD.vkFreeMemory(g.device, image.memory, nullptr);
    image = {};
}

bool Make(Image& image, VkFormat format, u32 width, u32 height, VkImageUsageFlags usage,
          VkImageAspectFlags view_aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    if (image.image && image.format == format && image.width == width && image.height == height) {
        return true;
    }
    Destroy(image);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    if (VKD.vkCreateImage(g.device, &info, nullptr, &image.image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements{};
    VKD.vkGetImageMemoryRequirements(g.device, image.image, &requirements);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = MemoryType(requirements.memoryTypeBits);
    if (VKD.vkAllocateMemory(g.device, &alloc, nullptr, &image.memory) != VK_SUCCESS) {
        return false;
    }
    VKD.vkBindImageMemory(g.device, image.image, image.memory, 0);
    if (usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {view_aspect, 0, 1, 0, 1};
        VKD.vkCreateImageView(g.device, &view, nullptr, &image.view);
    }
    image.format = format;
    image.width = width;
    image.height = height;
    return true;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    VKD.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
}

void Copy(VkCommandBuffer cmd, VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout, u32 width,
          u32 height, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, u32 src_layer = 0) {
    VkImageCopy copy{};
    copy.srcSubresource = {aspect, 0, src_layer, 1};
    copy.dstSubresource = {aspect, 0, 0, 1};
    copy.extent = {width, height, 1};
    VKD.vkCmdCopyImage(cmd, src, src_layout, dst, dst_layout, 1, &copy);
}

bool MakePipeline(std::span<const u32> code, std::initializer_list<VkDescriptorType> types, VkDescriptorSetLayout& set,
                  VkPipelineLayout& layout, VkPipeline& pipeline) {
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    u32 count = 0;
    for (const VkDescriptorType type : types) {
        bindings[count] = {count, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        ++count;
    }
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    set_info.bindingCount = count;
    set_info.pBindings = bindings.data();
    if (VKD.vkCreateDescriptorSetLayout(g.device, &set_info, nullptr, &set) != VK_SUCCESS) {
        return false;
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (VKD.vkCreatePipelineLayout(g.device, &layout_info, nullptr, &layout) != VK_SUCCESS) {
        return false;
    }
    const vk::ShaderModule module = CompileSPV(code, vk::Device(g.device));
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = VkShaderModule(module);
    info.stage.pName = "main";
    info.layout = layout;
    const VkResult result = VKD.vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    VKD.vkDestroyShaderModule(g.device, VkShaderModule(module), nullptr);
    return result == VK_SUCCESS;
}

// VR fork: LumaSharpen on each eye's final picture (vr_luma_sharpen.comp). BB_VR_SHARPEN=<strength>[,<limit>]:
// LumaSharpen's sharp_strength (its default 0.65) and sharp_clamp (0.035); none or 0 off. It replaces the RCAS pass
// after DLSS (vr_sharpen.comp; Matt 2026-10-22: halos and noise at any strength), and works with DLSS off too.
struct SharpenSettings {
    float strength = 0.0f;
    float limit = 0.035f;
};
SharpenSettings Sharpening() {
    static const SharpenSettings settings = [] {
        SharpenSettings s;
        if (const char* v = std::getenv("BB_VR_SHARPEN"); v && v[0]) {
            s.strength = std::max(0.0f, float(std::atof(v)));
            if (const char* comma = std::strchr(v, ',')) {
                s.limit = std::clamp(float(std::atof(comma + 1)), 0.001f, 1.0f);
            }
        }
        char text[64];
        std::snprintf(text, sizeof(text), "LumaSharpen: strength %.2f, limit %.3f", s.strength, s.limit);
        Log(text);
        return s;
    }();
    return settings;
}

// VR fork: TV gamma, as upstream 0.3.0's flat presenter (feab068): DriveClub was graded for TVs decoding with gamma 2.4,
// headsets (as PC displays) decode sRGB, about 2.2, so the picture looked washed out. Each eye's encoded values to the
// power 2.4 / 2.2, in the LumaSharpen pass (the scene; the HUD layer keeps its own). BB_VR_TV_GAMMA=1 starts with it
// on (=<gamma>: that TV gamma instead of 2.4); F11 turns it on and off while playing (Matt 2026-10-23: an A/B).
std::atomic<bool> g_tv_gamma_on{[] {
    const char* v = std::getenv("BB_VR_TV_GAMMA");
    return v && v[0] && v[0] != '0';
}()};
float TvGammaPower() {
    static const float target = [] {
        const char* v = std::getenv("BB_VR_TV_GAMMA");
        const float g = v ? float(std::atof(v)) : 0.0f;
        return g >= 1.8f && g <= 3.0f ? g : 2.4f;
    }();
    return g_tv_gamma_on.load(std::memory_order_relaxed) ? target / 2.2f : 1.0f;
}

struct Push {
    s32 width, height, depth_width, depth_height;
    float k;
    float pad[3];
};

} // namespace

void Init(VkPhysicalDevice physical_device, VkDevice device) {
    if (const char* v = std::getenv("BB_VR_STEREO"); v && v[0] == '0') {
        Log("off (BB_VR_STEREO=0)");
        return;
    }
    g.physical_device = physical_device;
    g.device = device;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKD.vkCreateSampler(device, &sampler, nullptr, &g.sampler);
    g.ready = MakePipeline(VR_STEREO_SPLAT_COMP, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                           g.splat_layout, g.splat_pipeline_layout, g.splat) &&
              MakePipeline(VR_STEREO_RESOLVE_COMP,
                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                           g.resolve_layout, g.resolve_pipeline_layout, g.resolve) &&
              MakePipeline(VR_STEREO_MIRROR_COMP,
                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                           g.mirror_layout, g.mirror_pipeline_layout, g.mirror_pipeline) &&
              MakePipeline(VR_LUMA_SHARPEN_COMP, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                           g.sharpen_layout, g.sharpen_pipeline_layout, g.sharpen);
    Log(g.ready ? "ready" : "pipelines failed");
}

namespace {
// The scene copy: `image` (any layout the runtime tracks) into the open slot; the target cleared for the HUD.
VideoCore::ImageId g_hud_target{};
bool g_captured_this_frame = false;
u32 g_display_draws = 0; // draws into a display buffer since the last draw elsewhere
bool g_scene_taken = false; // this display-buffer run's scene copy was made

// Development (BB_VR_LOG_DISPLAY_SEQ=1): each display-buffer run's draws (vs hashes, repeats counted, * = the scene
// copy), logged when it differs from the last run's.
struct DisplaySeq {
    bool on = std::getenv("BB_VR_LOG_DISPLAY_SEQ") != nullptr;
    std::string seq, last;
    u64 last_vs = 0;
    u32 repeats = 0, runs = 0;
    void Flush() {
        if (repeats > 1) {
            seq += "x" + std::to_string(repeats);
        }
        repeats = 0;
    }
    void Draw(u64 vs, bool capture) {
        if (vs != last_vs || capture) {
            Flush();
            char text[24];
            std::snprintf(text, sizeof(text), " %s%llx", capture ? "*" : "", (unsigned long long)vs);
            seq += text;
            last_vs = vs;
        }
        ++repeats;
    }
    void End() {
        if (seq.empty()) {
            return;
        }
        Flush();
        ++runs;
        if (seq != last) {
            std::printf("VR display seq (run %u):%s\n", runs, seq.c_str());
            std::fflush(stdout);
            last = seq;
        }
        seq.clear();
        last_vs = 0;
    }
};
DisplaySeq g_display_seq;

// Screen-space scene layers drawn into the display buffer after the composite, before the HUD: they stay in the scene
// (per eye) rather than the flat HUD layer. vs 0x44606ffd: the rain drops and wiper streaks on the windscreen;
// vs 0xdc8e480a: perspective geometry drawn after the composite in some frames, before the drops (the copy was taken
// there, so the drops went into the HUD layer in those frames: they flickered).
bool SceneOverlay(u64 vs_hash) {
    return vs_hash == 0x44606ffd || vs_hash == 0xdc8e480a;
}
VideoCore::ImageId g_frame_depth{}; // this frame's main G-buffer depth (taken by the scene copy)
bool g_frame_pose_valid = false; // this frame's rendered pose (OnFrameCamera), taken by the scene copy
float g_frame_layer_pose[7], g_frame_tangents[4];
u64 g_frame_depth_area = 0;
std::atomic<int> g_log_draws{0}; // draws left to log
std::atomic<u64> g_shoot_address{0};
std::atomic<int> g_shoot_left{0};
u64 g_shoot_last_vs = 0;
int g_shoot_count = 0;

// A host-visible buffer for readbacks (development).
struct Readback {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize size = 0;
};
bool MakeReadback(Readback& r, VkDeviceSize size) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (VKD.vkCreateBuffer(g.device, &info, nullptr, &r.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    VKD.vkGetBufferMemoryRequirements(g.device, r.buffer, &req);
    VkPhysicalDeviceMemoryProperties memory{};
    VKD.vkGetPhysicalDeviceMemoryProperties(g.physical_device, &memory);
    u32 type = 0;
    for (; type < memory.memoryTypeCount; ++type) {
        const auto flags = memory.memoryTypes[type].propertyFlags;
        if ((req.memoryTypeBits & (1u << type)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            break;
        }
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    VKD.vkAllocateMemory(g.device, &alloc, nullptr, &r.memory);
    VKD.vkBindBufferMemory(g.device, r.buffer, r.memory, 0);
    r.size = size;
    return true;
}
void FreeReadback(Readback& r) {
    VKD.vkDestroyBuffer(g.device, r.buffer, nullptr);
    VKD.vkFreeMemory(g.device, r.memory, nullptr);
    r = {};
}
void CopyToReadback(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, const Readback& r, VkDeviceSize offset,
                    u32 w, u32 h, u32 layer = 0, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {aspect, 0, layer, 1};
    region.imageExtent = {w, h, 1};
    VKD.vkCmdCopyImageToBuffer(cmd, image, layout, r.buffer, 1, &region);
}
// %TEMP%\dcvr\<name>: width, height, then the RGBA bytes at `offset` in the readback.
void WriteReadback(const Readback& r, VkDeviceSize offset, u32 w, u32 h, const char* name) {
    void* data = nullptr;
    VKD.vkMapMemory(g.device, r.memory, 0, r.size, 0, &data);
    const char* temp = std::getenv("TEMP");
    char path[512];
    std::snprintf(path, sizeof(path), "%s\\dcvr\\%s", temp ? temp : ".", name);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(&w, 4, 1, f);
        std::fwrite(&h, 4, 1, f);
        std::fwrite(static_cast<const u8*>(data) + offset, 1, size_t(w) * h * 4, f);
        std::fclose(f);
    }
    VKD.vkUnmapMemory(g.device, r.memory);
}

// Reads an RGBA8 image back (the runtime keeps its layout) and writes it raw (8-byte texels: the header's width is
// doubled). Layer: -1 = BB_VR_TSHOT_LAYER's.
void SaveImage(Scheduler& scheduler, Runtime& runtime, VideoCore::Image& image, const char* name, int layer = -1) {
    const u32 w = image.info.size.width, h = image.info.size.height;
    const u32 bytes = image.info.pixel_format == vk::Format::eR16G16B16A16Sfloat ? 8 : 4;
    Readback r;
    if (!MakeReadback(r, VkDeviceSize(w) * h * bytes)) {
        return;
    }
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    scheduler.EndRendering();
    // Development: BB_VR_TSHOT_LAYER=1 dumps the right eye's layer of a two-layer target.
    static const bool right = std::getenv("BB_VR_TSHOT_LAYER") && std::getenv("BB_VR_TSHOT_LAYER")[0] == '1';
    CopyToReadback(VkCommandBuffer(scheduler.CommandBuffer()), VkImage(image.GetImage()),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r, 0, w, h,
                   layer >= 0 ? u32(layer) : (right && Multiview::Stereo(image) ? 1 : 0),
                   image.info.props.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    scheduler.Finish();
    WriteReadback(r, 0, w * bytes / 4, h, name);
    FreeReadback(r);
}

// EYES: requested, then recorded into a readback written some frames later (the presenter's work has finished by then).
std::atomic<bool> g_eye_dump_requested{false};
Readback g_eye_dump;
int g_eye_dump_frames = 0; // frames until the recorded readback is written
u32 g_eye_dump_w = 0, g_eye_dump_h = 0;
} // namespace

// Development (BB_VR_FINDCAM): the G-buffer draws' vertex shader buffers holding a recent rendered camera position.
int g_find_camera = [] { return std::getenv("BB_VR_FINDCAM") ? 400 : 0; }();
void FindCamera(const Shader::Info& vs) {
    float positions[32][3];
    const u32 count = Camera::RecentPositions(positions, 32);
    if (count == 0 || std::abs(positions[count - 1][0]) + std::abs(positions[count - 1][1]) < 1.f) {
        return;
    }
    --g_find_camera;
    for (u32 i = 0; i < vs.buffers.size(); ++i) {
        const auto sharp = vs.buffers[i].GetSharp(vs);
        const u64 address = sharp.base_address;
        const u32 size = sharp.GetSize();
        if (!address || size < 12 || size > 0x10000) {
            continue;
        }
        const auto* f = reinterpret_cast<const float*>(address);
        for (u32 d = 0; d + 3 <= size / 4; ++d) {
            for (u32 p = 0; p < count; ++p) {
                const float dx = f[d] - positions[p][0], dy = f[d + 1] - positions[p][1], dz = f[d + 2] - positions[p][2];
                if (dx * dx + dy * dy + dz * dz < 1e-6f && std::abs(positions[p][0]) + std::abs(positions[p][1]) > 1.f) {
                    std::printf("VR findcam: vs %#llx buffer %u (size %u) dword %u = history %u (%.3f %.3f %.3f)\n",
                                (unsigned long long)vs.pgm_hash, i, size, d, p, f[d], f[d + 1], f[d + 2]);
                    std::fflush(stdout);
                }
            }
        }
    }
}

void AdjustBlend(AmdGpu::BlendControl& control, u64 target_address) {
    static const bool keep = std::getenv("BB_VR_KEEP_SCENE") != nullptr;
    if (!g.ready || keep || !control.enable || !FrameCapture::IsDisplayBuffer(target_address)) {
        return;
    }
    using Factor = AmdGpu::BlendControl::BlendFactor;
    using Func = AmdGpu::BlendControl::BlendFunc;
    if (control.color_func != Func::Add) {
        return;
    }
    if (control.color_dst_factor == Factor::OneMinusSrcAlpha) {
        control.alpha_src_factor = Factor::One;
        control.alpha_dst_factor = Factor::OneMinusSrcAlpha;
    } else if (control.color_dst_factor == Factor::One) {
        control.alpha_src_factor = Factor::Zero;
        control.alpha_dst_factor = Factor::One;
    } else {
        return;
    }
    control.alpha_func = Func::Add;
    control.separate_alpha_blend = 1;
}

void LogDraws() {
    g_log_draws = 5000;
}

void OnFrameCamera(const float* position) {
    if (!g.ready) {
        return;
    }
    float distance = 0.f;
    g_frame_pose_valid = Camera::RenderedAt(position, g_frame_layer_pose, g_frame_tangents, distance) && distance < 1e-3f;
}

void DumpEyes() {
    g_eye_dump_requested = true;
}

// FRAMELOG (StartFrameLog): the headset thread blits each shown frame's left eye to a thumbnail and copies it into
// one readback; written a few frames after the last one (its copies done).
// The thumbnails' side, 192 (BB_FRAMELOG_SIZE to change it: 512 shows a flash's objects).
const u32 FrameLogSize = [] {
    const char* v = std::getenv("BB_FRAMELOG_SIZE");
    return v ? std::clamp<u32>(u32(std::atoi(v)), 64, 1024) : 192u;
}();
std::atomic<u32> g_flog_request{0};
u32 g_flog_total = 0, g_flog_done = 0, g_flog_write_in = 0;
std::vector<u64> g_flog_times;
std::vector<u32> g_flog_sizes; // each frame's scene size (width << 16 | height)

void StartFrameLog(u32 frames) {
    g_flog_request = std::clamp<u32>(frames, 1, 20000);
}

void ShootTarget(u64 guest_address) {
    g_shoot_last_vs = 0;
    g_shoot_count = 0;
    g_shoot_address = guest_address;
    g_shoot_left = guest_address == 1 ? 200000 : 3000; // 1: the display buffers, over many frames
}

namespace {

// A new scene (a race starting or restarting, the menus again): the main view's camera (the render size's camera
// constants: target size at dwords 96-97, world position at 108-110) jumps far between frames. The seat is recentred
// then, as with F8 (BB_VR_AUTO_RECENTER=0 turns it off). A reset to track or a camera change moves it far less.
void NoteMainCamera(const float* constants) {
    static const bool enabled = !(std::getenv("BB_VR_AUTO_RECENTER") && std::getenv("BB_VR_AUTO_RECENTER")[0] == '0');
    u32 width = 0, height = 0;
    if (!enabled || !Fixes::RenderSize(width, height) || constants[96] != float(width) ||
        constants[97] != float(height)) {
        return;
    }
    static std::array<float, 3> last{};
    static bool have = false;
    const float* position = constants + 108;
    if (have) {
        const float dx = position[0] - last[0], dy = position[1] - last[1], dz = position[2] - last[2];
        const float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (jump > 100.f) {
            std::printf("VR stereo: the camera moved %.0f m in a frame (a new scene): recentring\n", jump);
            std::fflush(stdout);
            Vr::Recenter();
        }
    }
    last = {position[0], position[1], position[2]};
    have = true;
    // Development (BB_VR_CAM_FILE=path): the camera's position every 4th frame, for test drivers.
    static const char* cam_file = std::getenv("BB_VR_CAM_FILE");
    static u32 cam_frames = 0;
    if (cam_file && (cam_frames++ & 3) == 0) {
        if (FILE* file = std::fopen(cam_file, "w")) {
            std::fprintf(file, "%.2f %.2f %.2f %u\n", position[0], position[1], position[2], cam_frames);
            std::fclose(file);
        }
    }
}

} // namespace

std::atomic<u64> g_fs_watch{[] {
    const char* v = std::getenv("BB_VR_FS_IMAGES");
    return v ? std::strtoull(v, nullptr, 16) : 0ull;
}()};
std::atomic<int> g_fs_watch_left{2000};

std::mutex g_skip_mutex;
std::vector<u64> g_skip;
std::atomic<bool> g_skip_any{false};

void SetSkippedShaders(const std::string& list) {
    std::vector<u64> hashes;
    std::string token;
    for (const char c : list + " ") {
        if (std::isxdigit(static_cast<unsigned char>(c)) || c == 'x' || c == 'X') {
            token += c;
        } else if (!token.empty()) {
            hashes.push_back(std::strtoull(token.c_str(), nullptr, 16));
            token.clear();
        }
    }
    std::scoped_lock lock{g_skip_mutex};
    g_skip = std::move(hashes);
    g_skip_any = !g_skip.empty();
    std::printf("VR skip: %zu shaders:%s\n", g_skip.size(), list.c_str());
    std::fflush(stdout);
}

void LogDispatch(u64 cs_hash) {
    if (g_log_draws.load(std::memory_order_relaxed) > 0 && g_log_draws.fetch_sub(1) > 0) {
        std::printf("VR dispatch: cs %#llx\n", (unsigned long long)cs_hash);
        std::fflush(stdout);
    }
}

// Instanced grass clumps (vs 0xb8b4c1cb, fs 0x5db503d3) are not drawn in VR, as in vr1.1 (8fc6409): the game reads a
// GPU flag for them (game +0x758a2d) that any readback mode short of exact-per-page serves a frame late now and then,
// and they were drawn floating at random places (the sky past the start); drawn, they also cost ~5 passes a frame at
// 3200 per eye (f8e1d5e drew them: vr1.1 64-67 FPS on a 7-car grid, every later build 55-60). The roadside grass is
// another shader and stays; Matt, with the clumps skipped: "road side grass is still visible, so it looks fine like
// this". BB_VR_GRASS_CLUMPS=1 draws them.
constexpr u64 GrassClumpsVs = 0xb8b4c1cb;
const bool g_skip_grass_clumps = [] {
    const char* vr = std::getenv("BB_VR");
    const char* keep = std::getenv("BB_VR_GRASS_CLUMPS");
    return vr && vr[0] == '1' && !(keep && keep[0] == '1');
}();

bool SkipDraw(u64 vs_hash, u64 fs_hash) {
    if (g_skip_grass_clumps && vs_hash == GrassClumpsVs) {
        return true;
    }
    if (!g_skip_any.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{g_skip_mutex};
    return std::find(g_skip.begin(), g_skip.end(), vs_hash) != g_skip.end() ||
           (fs_hash && std::find(g_skip.begin(), g_skip.end(), fs_hash) != g_skip.end());
}

void WatchFragmentImages(u64 vs_hash) {
    g_fs_watch_left = 2000;
    g_fs_watch = vs_hash;
}

std::atomic<u64> g_tex_vs{0};
std::atomic<int> g_tex_mode{0}, g_tex_left{0};
std::vector<VAddr> g_tex_pending; // the draw's textures (LogFragmentImages, then OnDraw on the same thread)
std::vector<VAddr> g_tex_done;

void TextureAction(u64 vs_hash, int mode) {
    g_tex_done.clear();
    g_tex_mode = mode;
    g_tex_left = 40;
    g_tex_vs = vs_hash;
}

// DRAWLOG: the draw's index and instance counts (LogFragmentImages runs just before OnDraw with them).
thread_local u32 g_log_indices = 0, g_log_instances = 0;

void LogFragmentImages(const Shader::Info& vs, const Shader::Info* fs, u32 depth_control, u32 stencil_control,
                       u32 stencil_ref_front, u32 indices, u32 instances) {
    g_log_indices = indices; // DRAWLOG
    g_log_instances = instances;
    const u64 vs_hash = vs.pgm_hash;
    g_tex_pending.clear();
    if (fs && g_tex_vs.load(std::memory_order_relaxed) == vs_hash) {
        for (const auto& res : fs->images) {
            g_tex_pending.push_back(res.GetSharp(*fs).Address());
        }
    }
    const u64 watch = g_fs_watch.load(std::memory_order_relaxed);
    if (!watch || vs_hash != watch || !fs || fs->images.empty() || g_fs_watch_left.fetch_sub(1) <= 0) {
        return;
    }
    std::printf("VR fs images: vs %#llx fs %#llx indices %u instances %u depth %#x stencil %#x ref %#x:",
                (unsigned long long)vs_hash, (unsigned long long)fs->pgm_hash, indices, instances,
                depth_control, stencil_control, stencil_ref_front);
    for (const auto& res : fs->images) {
        const auto image = res.GetSharp(*fs);
        std::printf(" [%#llx %ux%u dfmt %u nfmt %u]", (unsigned long long)image.Address(), u32(image.width) + 1,
                    u32(image.height) + 1, u32(image.GetDataFmt()), u32(image.GetNumberFmt()));
    }
    for (const auto* stage : {&vs, fs}) {
        std::printf(" | %s buffers:", stage == &vs ? "vs" : "fs");
        for (u32 i = 0; i < stage->buffers.size(); ++i) {
            const auto sharp = stage->buffers[i].GetSharp(*stage);
            std::printf(" %u=[%#llx+%#x]", i, (unsigned long long)sharp.base_address, u32(sharp.GetSize()));
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}

void OnDraw(Scheduler& scheduler, Runtime& runtime, VideoCore::TextureCache& texture_cache, const Shader::Info& vs,
            const GraphicsPipelineKey& key,
            VideoCore::ImageId color_id, VideoCore::ImageId target2_id, VideoCore::ImageId gbuffer_depth_id) {
    const u64 vs_hash = vs.pgm_hash;
    Multiview::SetDrawHash(vs_hash);
    if (!g_tex_pending.empty() && g_tex_vs.load(std::memory_order_relaxed) == vs_hash && g.device) {
        for (const VAddr address : g_tex_pending) {
            if (std::ranges::find(g_tex_done, address) != g_tex_done.end()) {
                continue;
            }
            g_tex_done.push_back(address);
            const VideoCore::ImageId id = texture_cache.FindImageFromRange(address, 1, false);
            if (!id) {
                std::printf("VR tex: %#llx not in the texture cache\n", (unsigned long long)address);
                continue;
            }
            auto& image = texture_cache.GetImage(id);
            if (g_tex_mode == 2) {
                texture_cache.InvalidateMemory(address, image.info.guest_size);
                std::printf("VR tex: %#llx (%ux%u) marked for upload again\n", (unsigned long long)address,
                            image.info.size.width, image.info.size.height);
                continue;
            }
            // Mip 0, layer 0, raw (compressed formats as their blocks).
            const auto format = image.info.pixel_format;
            const u32 w = image.info.size.width, h = image.info.size.height;
            const auto extent = vk::blockExtent(format);
            const u64 bytes = u64((w + extent[0] - 1) / extent[0]) * ((h + extent[1] - 1) / extent[1]) * vk::blockSize(format);
            Readback r;
            if (!MakeReadback(r, bytes)) {
                continue;
            }
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            scheduler.EndRendering();
            CopyToReadback(VkCommandBuffer(scheduler.CommandBuffer()), VkImage(image.GetImage()),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r, 0, w, h);
            scheduler.Finish();
            void* data = nullptr;
            VKD.vkMapMemory(g.device, r.memory, 0, r.size, 0, &data);
            char path[512];
            std::snprintf(path, sizeof(path), "%s\\dcvr\\tex_%llx.raw", std::getenv("TEMP") ? std::getenv("TEMP") : ".",
                          (unsigned long long)address);
            if (FILE* f = std::fopen(path, "wb")) {
                const u32 header[4] = {w, h, u32(format), u32(bytes)};
                std::fwrite(header, sizeof(header), 1, f);
                std::fwrite(data, 1, bytes, f);
                std::fclose(f);
            }
            VKD.vkUnmapMemory(g.device, r.memory);
            FreeReadback(r);
            std::printf("VR tex: %#llx %ux%u vk format %d written\n", (unsigned long long)address, w, h, int(format));
        }
        std::fflush(stdout);
        if (g_tex_left.fetch_sub(1) <= 1) {
            g_tex_vs = 0;
        }
    }
    const int targets = std::popcount(key.mrt_mask);
    // DRAWLOG: multi-target draws too (the G-buffer passes), marked "mrt N".
    if (targets >= 2 && g_log_draws.load(std::memory_order_relaxed) > 0 && g_log_draws.fetch_sub(1) > 0 && color_id) {
        const auto& image = texture_cache.GetImage(color_id);
        std::printf("VR draws: vs %#llx ps %#llx target %ux%u format %d at %#llx mrt %d idx %u inst %u\n",
                    (unsigned long long)vs_hash, (unsigned long long)key.stage_hashes[0], image.info.size.width,
                    image.info.size.height, int(image.info.pixel_format), (unsigned long long)image.info.guest_address,
                    targets, g_log_indices, g_log_instances);
    }
    if (targets >= 3) {
        if (g_find_camera > 0) {
            FindCamera(vs);
        }
        // The frame's camera constants (512 bytes, its world position at dword 108) on the G-buffer's vertex shader: the
        // head pose this frame was rendered with.
        for (u32 i = 0; i < vs.buffers.size() && g.ready && !g_frame_pose_valid; ++i) {
            const auto sharp = vs.buffers[i].GetSharp(vs);
            if (sharp.GetSize() == 512 && sharp.base_address) {
                Dlss::OnCameraConstants(reinterpret_cast<const float*>(sharp.base_address));
                NoteMainCamera(reinterpret_cast<const float*>(sharp.base_address));
                OnFrameCamera(reinterpret_cast<const float*>(sharp.base_address) + 108);
            }
        }
        // A G-buffer pass: the main view's is the largest one with a D32S8 depth (the mirror's and cube faces' are
        // smaller).
        if (g.ready && target2_id && gbuffer_depth_id) {
            const auto& depth = texture_cache.GetImage(gbuffer_depth_id);
            const u32 w = depth.info.size.width, h = depth.info.size.height;
            if (depth.info.pixel_format == vk::Format::eD32SfloatS8Uint &&
                u64(w) * h >= g_frame_depth_area) {
                g_frame_depth = gbuffer_depth_id;
                g_frame_depth_area = u64(w) * h;
            }
        }
        return;
    }
    if (targets != 1) {
        return;
    }
    const VideoCore::ImageId depth_id = g_frame_depth;
    if (g_log_draws.load(std::memory_order_relaxed) > 0 && g_log_draws.fetch_sub(1) > 0 && color_id) {
        const auto& image = texture_cache.GetImage(color_id);
        u32 blend = 0;
        std::memcpy(&blend, &key.blend_controls[0], sizeof(blend));
        std::printf("VR draws: vs %#llx ps %#llx target %ux%u format %d at %#llx blend %#x mask %#x idx %u inst %u%s%s\n",
                    (unsigned long long)vs_hash, (unsigned long long)key.stage_hashes[0], image.info.size.width,
                    image.info.size.height, int(image.info.pixel_format), (unsigned long long)image.info.guest_address,
                    blend, u32(key.write_masks[0]), g_log_indices, g_log_instances,
                    FrameCapture::IsDisplayBuffer(image.info.guest_address) ? " (display)" : "",
                    UiComposition::MovieShader(vs_hash) ? " (movie)" : "");
    }
    if (g_shoot_left.load(std::memory_order_relaxed) > 0 && color_id && g.device) {
        if (--g_shoot_left == 0) {
            std::printf("VR tshot: done (%d images)\n", g_shoot_count);
            std::fflush(stdout);
        }
        auto& image = texture_cache.GetImage(color_id);
        const auto f = image.info.pixel_format;
        // TSHOT "display": any display buffer (the frames alternate between them), over many frames.
        const bool shoot_display = g_shoot_address.load() == 1 && FrameCapture::IsDisplayBuffer(image.info.guest_address);
        if ((image.info.guest_address == g_shoot_address.load() || shoot_display) && vs_hash != g_shoot_last_vs &&
            (f == vk::Format::eR8G8B8A8Unorm || f == vk::Format::eR8G8B8A8Srgb || f == vk::Format::eB8G8R8A8Unorm ||
             f == vk::Format::eB8G8R8A8Srgb || f == vk::Format::eB10G11R11UfloatPack32 ||
             f == vk::Format::eR16G16B16A16Sfloat) && g_shoot_count < 40) {
            g_shoot_last_vs = vs_hash;
            char name[96];
            std::snprintf(name, sizeof(name), "tshot_%02d_%llx_%ux%u.raw", g_shoot_count++, (unsigned long long)vs_hash,
                          image.info.size.width, image.info.size.height);
            // Both eyes of a two-layer target: the right eye's as <name>_R.raw.
            const bool both = Multiview::Stereo(image) && !std::getenv("BB_VR_TSHOT_LAYER");
            SaveImage(scheduler, runtime, image, name, both ? 0 : -1);
            if (both) {
                std::string right_name = name;
                right_name.insert(right_name.size() - 4, "_R");
                SaveImage(scheduler, runtime, image, right_name.c_str(), 1);
            }
            // The draw's depth (32-bit float depth only) as <name>_D.raw / _DR.raw.
            if (gbuffer_depth_id) {
                auto& depth = texture_cache.GetImage(gbuffer_depth_id);
                const auto df = depth.info.pixel_format;
                if (df == vk::Format::eD32SfloatS8Uint || df == vk::Format::eD32Sfloat) {
                    std::string depth_name = name;
                    depth_name.insert(depth_name.size() - 4, "_D");
                    SaveImage(scheduler, runtime, depth, depth_name.c_str(), 0);
                    if (Multiview::Stereo(depth)) {
                        depth_name.insert(depth_name.size() - 4, "R");
                        SaveImage(scheduler, runtime, depth, depth_name.c_str(), 1);
                    }
                }
            }
        }
    }
    if (!g.ready || !color_id) {
        return;
    }
    auto& color = texture_cache.GetImage(color_id);
    // Driveclub draws its frame's composite into the display buffer, then every HUD element over it.
    if (!FrameCapture::IsDisplayBuffer(color.info.guest_address)) {
        if (g_display_seq.on && g_display_draws) {
            g_display_seq.End();
        }
        g_display_draws = 0;
        g_scene_taken = false;
        return;
    }
    const bool multiview = Multiview::Stereo(color);
    // The scene is copied before the first HUD draw: the composite and any scene overlays stay in it.
    const bool capture = !(++g_display_draws < 2 || g_scene_taken || SceneOverlay(vs_hash) || !depth_id);
    if (g_display_seq.on) {
        g_display_seq.Draw(vs_hash, capture);
    }
    if (!capture) {
        return; // no 3D this frame (2D menus): the whole frame for the flat screen
    }
    g_scene_taken = true;
    const vk::Format format = color.info.pixel_format;
    if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eR8G8B8A8Srgb &&
        format != vk::Format::eB8G8R8A8Unorm && format != vk::Format::eB8G8R8A8Srgb) {
        return;
    }
    g_captured_this_frame = true;
    // Development (BB_VR_PROFILE with BB_GPU_PROFILE=1): GPU time per pass, a frame being HUD start to HUD start (the
    // profiler's own frame start needs the upscaler's camera motion).
    static const bool profile = std::getenv("BB_VR_PROFILE") != nullptr;
    if (auto* profiler = GpuProfiler::Get(); profile && profiler) {
        profiler->BeginFrame();
    }
    g_frame_depth = {};
    g_frame_depth_area = 0;
    const bool frame_pose = std::exchange(g_frame_pose_valid, false);
    static u32 frames = 0, misses = 0;
    misses += !frame_pose;
    if (++frames % 1800 == 0) {
        std::printf("VR stereo: %u of the last 1800 frames with their rendered pose\n", 1800 - misses);
        std::fflush(stdout);
        misses = 0;
    }
    const u32 width = color.info.size.width, height = color.info.size.height;
    static int logged = 0;
    if (logged++ % 600 == 0) {
        std::printf("VR stereo: scene in display buffer %ux%u format %d at %#llx; scene depth ", width, height, int(format),
                    (unsigned long long)color.info.guest_address);
        if (depth_id) {
            const auto& depth = texture_cache.GetImage(depth_id);
            std::printf("%ux%u\n", depth.info.size.width, depth.info.size.height);
        } else {
            std::printf("missing\n");
        }
        std::fflush(stdout);
    }
    const u32 index = g.next_slot;
    Slot& slot = g.slots[index];
    const VkImageUsageFlags color_usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if (!Make(slot.scene, VK_FORMAT_R8G8B8A8_UNORM, width, height, color_usage) ||
        !Make(slot.hud, VK_FORMAT_R8G8B8A8_UNORM, width, height, color_usage)) {
        return;
    }
    slot.has_depth = false;
    slot.has_right = multiview && Make(slot.scene_right, VK_FORMAT_R8G8B8A8_UNORM, width, height, color_usage);
    VkImage depth_image{};
    u32 depth_width = 0, depth_height = 0;
    if (depth_id) {
        auto& depth = texture_cache.GetImage(depth_id);
        depth_width = depth.info.size.width;
        depth_height = depth.info.size.height;
        if (Make(slot.depth, VkFormat(depth.info.pixel_format), depth_width, depth_height,
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
            runtime.Transit(&depth, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            depth_image = VkImage(depth.GetImage());
        }
    }
    runtime.Transit(&color, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();
    scheduler.EndRendering();
    const VkCommandBuffer cmd = VkCommandBuffer(scheduler.CommandBuffer());
    Barrier(cmd, slot.scene.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    Copy(cmd, VkImage(color.GetImage()), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.scene.image,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, width, height);
    Barrier(cmd, slot.scene.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    if (slot.has_right) {
        Barrier(cmd, slot.scene_right.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        Copy(cmd, VkImage(color.GetImage()), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.scene_right.image,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, width, height, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        Barrier(cmd, slot.scene_right.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        depth_image = {}; // the eyes are rendered: no reprojection
    }
    if (depth_image) {
        Barrier(cmd, slot.depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_ASPECT_DEPTH_BIT);
        Copy(cmd, depth_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             depth_width, depth_height, VK_IMAGE_ASPECT_DEPTH_BIT);
        Barrier(cmd, slot.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_DEPTH_BIT);
        slot.has_depth = true;
    }
    // The HUD draws over transparent black from here (the image stays in TRANSFER_SRC as the runtime knows it).
    Barrier(cmd, VkImage(color.GetImage()), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue clear{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    static const bool keep = std::getenv("BB_VR_KEEP_SCENE") != nullptr; // development: the HUD over the scene
    if (!keep) {
        VKD.vkCmdClearColorImage(cmd, VkImage(color.GetImage()), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }
    Barrier(cmd, VkImage(color.GetImage()), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    slot.width = width;
    slot.height = height;
    std::scoped_lock lock{g.mutex};
    slot.has_hud = false;
    slot.has_pose = frame_pose;
    std::memcpy(slot.layer_pose, g_frame_layer_pose, sizeof(slot.layer_pose));
    std::memcpy(slot.tangents, g_frame_tangents, sizeof(slot.tangents));
    g.next_slot = (index + 1) % SlotCount;
    g.complete.push_back(index);
    while (g.complete.size() > SlotCount - 1) {
        g.complete.pop_front();
    }
}

bool TakeFrame(Frame& frame) {
    std::scoped_lock lock{g.mutex};
    if (g.complete.empty()) {
        return false;
    }
    frame.slot = g.complete.front();
    g.complete.pop_front();
    const Slot& slot = g.slots[frame.slot];
    frame.width = slot.width;
    frame.height = slot.height;
    frame.has_pose = slot.has_pose;
    std::memcpy(frame.layer_pose, slot.layer_pose, sizeof(frame.layer_pose));
    std::memcpy(frame.tangents, slot.tangents, sizeof(frame.tangents));
    return slot.has_depth || slot.has_right;
}

void OnDisplayBuffer(Runtime& runtime, vk::CommandBuffer command_buffer, VideoCore::Image& display) {
    std::scoped_lock lock{g.mutex};
    if (!g.ready || g.complete.empty()) {
        return;
    }
    Slot& slot = g.slots[g.complete.back()];
    if (slot.has_hud || display.info.size.width != slot.width || display.info.size.height != slot.height) {
        return;
    }
    runtime.Transit(&display, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    const VkCommandBuffer cmd = VkCommandBuffer(command_buffer);
    Barrier(cmd, slot.hud.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    Copy(cmd, VkImage(display.GetImage()), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.hud.image,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, slot.width, slot.height);
    // GENERAL from here on: the headset's queue copies it and the desktop mirror reads it, neither transitions it.
    Barrier(cmd, slot.hud.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    slot.has_hud = true;
}

Readback g_flog;
Image g_flog_thumb;

void FrameLog(VkCommandBuffer cmd, VkImage scene, VkImageLayout layout, u32 w, u32 h) {
    const VkDeviceSize Bytes = VkDeviceSize(FrameLogSize) * FrameLogSize * 4;
    if (g_flog_write_in > 0 && --g_flog_write_in == 0) {
        void* data = nullptr;
        VKD.vkMapMemory(g.device, g_flog.memory, 0, g_flog.size, 0, &data);
        const char* temp = std::getenv("TEMP");
        char path[512];
        std::snprintf(path, sizeof(path), "%s\\dcvr\\framelog.raw", temp ? temp : ".");
        if (FILE* f = std::fopen(path, "wb")) {
            const u32 header[3] = {FrameLogSize, FrameLogSize, g_flog_done};
            std::fwrite(header, 4, 3, f);
            std::fwrite(data, 1, size_t(Bytes) * g_flog_done, f);
            std::fwrite(g_flog_times.data(), 8, g_flog_times.size(), f);
            std::fwrite(g_flog_sizes.data(), 4, g_flog_sizes.size(), f);
            std::fclose(f);
        }
        VKD.vkUnmapMemory(g.device, g_flog.memory);
        FreeReadback(g_flog);
        Log("frame log written");
        g_flog_total = 0;
    }
    if (const u32 request = g_flog_request.exchange(0); request && g_flog_total == 0) {
        if (MakeReadback(g_flog, Bytes * request) &&
            Make(g_flog_thumb, VK_FORMAT_R8G8B8A8_UNORM, FrameLogSize, FrameLogSize,
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            g_flog_total = request;
            g_flog_done = 0;
            g_flog_times.clear();
            g_flog_sizes.clear();
            Log("frame log started");
        }
    }
    if (g_flog_total == 0 || g_flog_done >= g_flog_total || g_flog_write_in > 0) {
        return;
    }
    Barrier(cmd, g_flog_thumb.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {s32(w), s32(h), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {s32(FrameLogSize), s32(FrameLogSize), 1};
    VKD.vkCmdBlitImage(cmd, scene, layout, g_flog_thumb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
    Barrier(cmd, g_flog_thumb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CopyToReadback(cmd, g_flog_thumb.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_flog, Bytes * g_flog_done,
                   FrameLogSize, FrameLogSize);
    g_flog_times.push_back(u64(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()).count()));
    g_flog_sizes.push_back(w << 16 | h);
    if (++g_flog_done == g_flog_total) {
        g_flog_write_in = 8; // the last copies are done by then
    }
}

void RecordEyes(vk::CommandBuffer command_buffer, const Frame& frame, const VkImage (&eyes)[2], const float (&k)[2],
                VkImage hud) {
    const VkCommandBuffer cmd = VkCommandBuffer(command_buffer);
    Slot& slot = g.slots[frame.slot];
    const u32 w = frame.width, h = frame.height;
    if (g_eye_dump_frames > 0 && --g_eye_dump_frames == 0) {
        const VkDeviceSize size = VkDeviceSize(g_eye_dump_w) * g_eye_dump_h * 4;
        for (int n = 0; n < 3; ++n) {
            char name[32];
            std::snprintf(name, sizeof(name), "eye_%d.raw", n);
            WriteReadback(g_eye_dump, size * n, g_eye_dump_w, g_eye_dump_h, name);
        }
        FreeReadback(g_eye_dump);
        Log("eyes written");
    }
    FrameLog(cmd, slot.scene.image, VK_IMAGE_LAYOUT_GENERAL, w, h);
    const bool dump = g_eye_dump_frames == 0 && g_eye_dump_requested.exchange(false) &&
                      MakeReadback(g_eye_dump, VkDeviceSize(w) * h * 4 * 3);
    if (dump) {
        g_eye_dump_w = w;
        g_eye_dump_h = h;
        g_eye_dump_frames = 6;
        CopyToReadback(cmd, slot.scene.image, VK_IMAGE_LAYOUT_GENERAL, g_eye_dump, VkDeviceSize(w) * h * 4 * 2, w, h);
    }
    if (slot.has_right && !dump) {
        // Multiview: each eye's own scene straight into the headset's image (one copy per eye, bit for bit: the
        // scene is gamma-encoded already). With sharpening, LumaSharpen into an eye image first, copied from there.
        const SharpenSettings sharpen = Sharpening();
        const float power = TvGammaPower();
        static float logged_power = 1.0f;
        if (power != logged_power) {
            logged_power = power;
            char text[64];
            std::snprintf(text, sizeof(text), "TV gamma: power %.3f on each eye", power);
            Log(text);
        }
        for (int eye = 0; eye < 2; ++eye) {
            const VkImage scene = eye ? slot.scene_right.image : slot.scene.image;
            const VkImageLayout scene_layout = eye ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            VkImage source = scene;
            VkImageLayout source_layout = scene_layout;
            if ((sharpen.strength > 0.0f || power != 1.0f) && g.sharpen &&
                Make(g.eyes[eye], VK_FORMAT_R8G8B8A8_UNORM, w, h,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
                Barrier(cmd, scene, scene_layout, VK_IMAGE_LAYOUT_GENERAL);
                Barrier(cmd, g.eyes[eye].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                VkDescriptorImageInfo src_info{VK_NULL_HANDLE, eye ? slot.scene_right.view : slot.scene.view,
                                               VK_IMAGE_LAYOUT_GENERAL};
                VkDescriptorImageInfo dst_info{VK_NULL_HANDLE, g.eyes[eye].view, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet writes[2]{};
                writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &src_info, nullptr, nullptr};
                writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dst_info, nullptr, nullptr};
                const struct {
                    s32 width, height;
                    float strength, limit, power, pad[3];
                } push{s32(w), s32(h), sharpen.strength, sharpen.limit, power, {}};
                VKD.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.sharpen);
                VKD.vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.sharpen_pipeline_layout, 0, 2,
                                              writes);
                VKD.vkCmdPushConstants(cmd, g.sharpen_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                                       &push);
                VKD.vkCmdDispatch(cmd, (w + 15) / 16, (h + 7) / 8, 1);
                Barrier(cmd, scene, VK_IMAGE_LAYOUT_GENERAL, scene_layout);
                Barrier(cmd, g.eyes[eye].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                source = g.eyes[eye].image;
                source_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            }
            Barrier(cmd, eyes[eye], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            Copy(cmd, source, source_layout, eyes[eye], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);
            Barrier(cmd, eyes[eye], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
    }
    for (int eye = 0; eye < 2 && !(slot.has_right && !dump); ++eye) {
        if (!Make(g.keys[eye], VK_FORMAT_R32_UINT, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !Make(g.eyes[eye], VK_FORMAT_R8G8B8A8_UNORM, w, h,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            return;
        }
    }
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const Push push_base{s32(w), s32(h), s32(slot.depth.width), s32(slot.depth.height), 0.f, {}};
    for (int eye = 0; eye < 2 && !(slot.has_right && !dump); ++eye) {
        if (slot.has_right) {
            // Multiview: each eye was rendered; its scene into the eye image.
            Barrier(cmd, g.eyes[eye].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            Copy(cmd, eye ? slot.scene_right.image : slot.scene.image,
                 eye ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL, g.eyes[eye].image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);
            Barrier(cmd, g.eyes[eye].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        } else {
            // Keys: cleared, then each scene pixel's surface lands on its eye position.
            Barrier(cmd, g.keys[eye].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            const VkClearColorValue zero{};
            VKD.vkCmdClearColorImage(cmd, g.keys[eye].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            Barrier(cmd, g.keys[eye].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            Push push = push_base;
            push.k = k[eye];
            VkDescriptorImageInfo depth_info{g.sampler, slot.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo keys_info{VK_NULL_HANDLE, g.keys[eye].view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet writes[3]{};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depth_info, nullptr, nullptr};
            writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &keys_info, nullptr, nullptr};
            VKD.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.splat);
            VKD.vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.splat_pipeline_layout, 0, 2, writes);
            VKD.vkCmdPushConstants(cmd, g.splat_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            VKD.vkCmdDispatch(cmd, (w + 15) / 16, (h + 7) / 8, 1);
            Barrier(cmd, g.keys[eye].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

            // The eye: each pixel's source (holes from the background beside them).
            Barrier(cmd, g.eyes[eye].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            VkDescriptorImageInfo scene_info{VK_NULL_HANDLE, slot.scene.view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo eye_info{VK_NULL_HANDLE, g.eyes[eye].view, VK_IMAGE_LAYOUT_GENERAL};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &scene_info, nullptr, nullptr};
            writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &keys_info, nullptr, nullptr};
            writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &eye_info, nullptr, nullptr};
            VKD.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.resolve);
            VKD.vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.resolve_pipeline_layout, 0, 3, writes);
            VKD.vkCmdPushConstants(cmd, g.resolve_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            VKD.vkCmdDispatch(cmd, (w + 15) / 16, (h + 7) / 8, 1);
            Barrier(cmd, g.eyes[eye].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        if (dump) {
            CopyToReadback(cmd, g.eyes[eye].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_eye_dump,
                           VkDeviceSize(w) * h * 4 * eye, w, h);
        }
        // Into the headset's sRGB image, bit for bit (the scene is gamma-encoded already).
        Barrier(cmd, eyes[eye], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        Copy(cmd, g.eyes[eye].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, eyes[eye], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             w, h);
        Barrier(cmd, eyes[eye], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (hud) {
        Barrier(cmd, hud, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (slot.has_hud) {
            Copy(cmd, slot.hud.image, VK_IMAGE_LAYOUT_GENERAL, hud, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, w, h);
        } else {
            const VkClearColorValue clear{};
            VKD.vkCmdClearColorImage(cmd, hud, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        }
        Barrier(cmd, hud, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
}

void ToggleTvGamma() {
    const bool on = !g_tv_gamma_on.load(std::memory_order_relaxed);
    g_tv_gamma_on.store(on, std::memory_order_relaxed);
    char text[64];
    std::snprintf(text, sizeof(text), "TV gamma %s (power %.3f)", on ? "on" : "off", TvGammaPower());
    Log(text);
}

void RecordMirror(vk::CommandBuffer command_buffer, const Frame& frame, VkImage window, u32 window_width,
                  u32 window_height) {
    const VkCommandBuffer cmd = VkCommandBuffer(command_buffer);
    Slot& slot = g.slots[frame.slot];
    const u32 w = frame.width, h = frame.height;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const Push push_base{s32(w), s32(h), s32(slot.depth.width), s32(slot.depth.height), 0.f, {}};
    if (window && slot.has_hud && Make(g.mirror, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        // The desktop window: the HUD over the (left eye's) scene, fitted into the window (already cleared and holding
        // the HUD-only frame). The slot's scene and HUD stay in GENERAL (the headset's queue reads them too).
        Barrier(cmd, g.mirror.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkDescriptorImageInfo infos[3] = {{VK_NULL_HANDLE, slot.scene.view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, slot.hud.view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, g.mirror.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[3]{};
        for (u32 n = 0; n < 3; ++n) {
            writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, n, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[n], nullptr, nullptr};
        }
        VKD.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.mirror_pipeline);
        VKD.vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.mirror_pipeline_layout, 0, 3, writes);
        VKD.vkCmdPushConstants(cmd, g.mirror_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_base),
                               &push_base);
        VKD.vkCmdDispatch(cmd, (w + 15) / 16, (h + 7) / 8, 1);
        Barrier(cmd, g.mirror.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        // Fitted: the largest rectangle of the eye's shape, centred.
        const float scale = std::min(float(window_width) / float(w), float(window_height) / float(h));
        const s32 fw = s32(float(w) * scale), fh = s32(float(h) * scale);
        const s32 x0 = (s32(window_width) - fw) / 2, y0 = (s32(window_height) - fh) / 2;
        const VkClearColorValue black{{0.f, 0.f, 0.f, 1.f}};
        VKD.vkCmdClearColorImage(cmd, window, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        Barrier(cmd, window, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {s32(w), s32(h), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {x0 + fw, y0 + fh, 1};
        VKD.vkCmdBlitImage(cmd, g.mirror.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, window,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
}

} // namespace Vulkan::Vr::Stereo
