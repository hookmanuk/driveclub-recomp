// SPDX-License-Identifier: GPL-2.0-or-later
// VR (dcport-vr): stereo from the centre view. Driveclub's first Scaleform draw of a frame is where its scene is complete
// and its HUD not yet drawn: there the scene and its depth are copied out and the target cleared, so the HUD draws alone;
// at the display pass the HUD is copied out too. The presenter then builds each eye from the scene and its depth
// (vr_stereo.comp) and hands the eyes and the HUD to the headset as separate layers.

#pragma once

#include <string>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class TextureCache;
struct Image;
} // namespace VideoCore

namespace Shader {
struct Info;
} // namespace Shader

namespace AmdGpu {
struct BlendControl;
} // namespace AmdGpu

namespace Vulkan {
class Scheduler;
class Runtime;
struct GraphicsPipelineKey;
} // namespace Vulkan

namespace Vulkan::Vr::Stereo {

/// After the device is made (with the OpenXR session).
void Init(VkPhysicalDevice physical_device, VkDevice device);

/// Rasterizer, each draw (its pipeline key, colour targets 0 and 2, the depth target). G-buffer passes (3+ targets)
/// give the frame's scene depth. Driveclub composites its frame into the display buffer, then draws every HUD element
/// over it: at the second draw of such a run the scene and its depth are copied out and the target is cleared, so the
/// HUD draws alone.
/// Development (BB_VR_FS_IMAGES=<vs hex>, or the FSIMG trigger): the images draws with that vertex shader sample in
/// their pixel shader.
void LogFragmentImages(const Shader::Info& vs, const Shader::Info* fs, u32 depth_control = 0,
                       u32 stencil_control = 0, u32 stencil_ref_front = 0, u32 indices = 0,
                       u32 instances = 0);
void WatchFragmentImages(u64 vs_hash);

/// Development (TEXDUMP / TEXREFRESH <vs>): over that vertex shader's next 40 draws, each distinct texture its pixel
/// shader samples is written raw to %TEMP%\dcvr\tex_<address>.raw (mode 1) or marked for upload again from guest
/// memory (mode 2).
void TextureAction(u64 vs_hash, int mode);

/// Development (SKIP trigger: shader hashes in hex, separated by spaces or commas; an empty file clears it): draws
/// whose vertex or pixel shader is listed draw nothing (live, also while the game is paused).
void SetSkippedShaders(const std::string& list);
bool SkipDraw(u64 vs_hash, u64 fs_hash);

void OnDraw(Scheduler& scheduler, Runtime& runtime, VideoCore::TextureCache& texture_cache, const Shader::Info& vs,
            const GraphicsPipelineKey& key,
            VideoCore::ImageId color, VideoCore::ImageId target2, VideoCore::ImageId depth);

/// Pipeline cache, a colour target's blend state. While the HUD draws alone (over transparent black in the display
/// buffer), alpha must hold its coverage for the HUD layer (premultiplied): Driveclub blends the minimap with
/// SrcA/1-SrcA on alpha too, which leaves a^2 (faded parts vanished). Alpha follows the colour's own transmittance:
/// dst factor 1-SrcA: a = srcA + (1 - srcA) * dstA; dst factor One (additive): a unchanged. The flat picture is the
/// same (the presenter drops alpha).
void AdjustBlend(AmdGpu::BlendControl& control, u64 target_address);

/// Development (DRAWLOG trigger): log the next 5000 single-target draws.
void LogDraws();
/// Development (DRAWLOG): compute dispatches are logged too (their shader).
void LogDispatch(u64 cs_hash);

/// Development (TSHOT trigger): during the next 3000 draws, the RGBA8 target at `guest_address` is saved before each
/// draw whose vertex shader differs from the previous one (%TEMP%\dcvr\tshot_<n>_<vs>.raw: width, height, then RGBA).
void ShootTarget(u64 guest_address);

/// Development (EYES trigger): the next stereo frame's eyes and scene, full size, to %TEMP%\dcvr\eye_<n>.raw (n: 0 left,
/// 1 right, 2 the centre scene; width, height, then RGBA).
void DumpEyes();
/// Development (dev key FRAMELOG, its content the frame count): the left eye of each of the next `frames` frames the
/// headset shows, as a 192x192 thumbnail (BB_FRAMELOG_SIZE: another side), into %TEMP%\dcvr\framelog.raw (u32
/// width, height, count, the RGBA thumbnails, a u64 steady-clock microsecond time per frame, then each frame's scene
/// size as u32 width << 16 | height). Every frame, no GPU sync.
void StartFrameLog(u32 frames);
/// The headset thread: a shown frame for the frame log (the stereo scene, or the flat screen: menus, videos).
void FrameLog(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, u32 width, u32 height);

/// Dev hook at the TAA resolve (once per rendered frame, before its scene copy): the camera's world position the GPU
/// renders this frame with. Picks the head pose that view was made with (Camera::RenderedAt) for the frame's layer.
void OnFrameCamera(const float* position);

/// Presenter, PrepareFrame: the display buffer as the game finished it (the HUD over transparent black, with its alpha)
/// into the newest complete frame's HUD.
void OnDisplayBuffer(Runtime& runtime, vk::CommandBuffer cmd, VideoCore::Image& display);

/// Presenter: the oldest complete frame, if any (its scene, depth and HUD are ready in submission order).
struct Frame {
    u32 slot = 0;
    u32 width = 0, height = 0;
    bool has_pose = false; // the head pose and field of view it was rendered with
    float layer_pose[7];
    float tangents[4];
};
bool TakeFrame(Frame& frame);

/// The headset's thread (its own queue): both eyes into `eyes` (sRGB swapchain images of the scene's size, any layout)
/// and the frame's HUD into `hud` (sRGB, the scene's size). k: pixels of shift per unit of reversed depth for each eye
/// (the depth reprojection, see vr_stereo.comp).
void RecordEyes(vk::CommandBuffer cmd, const Frame& frame, const VkImage (&eyes)[2], const float (&k)[2], VkImage hud);

/// The headset's thread, F11: TV gamma on or off (see vr_stereo.cpp TvGammaPower).
void ToggleTvGamma();

/// Presenter: the desktop window's image (`window`, TRANSFER_DST): the frame's scene with its HUD, fitted.
void RecordMirror(vk::CommandBuffer cmd, const Frame& frame, VkImage window, u32 window_width, u32 window_height);

} // namespace Vulkan::Vr::Stereo
