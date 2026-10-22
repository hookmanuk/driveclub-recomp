// SPDX-License-Identifier: GPL-2.0-or-later
// VR (dcport-vr): DLSS for each eye. In multiview the game's anti-aliasing inputs and output are two-layer images
// (one layer per eye); DriveAA's pass runs here once per eye instead, each eye with its own DLSS feature (its own
// history, bridge ABI 3), its own camera motion (the eye's clip shift taken out) and its own layer of the motion and
// result images. Upscaling: the result is the game's output size, and what samples the anti-aliased picture by UV
// reads both eyes of it through a two-layer view.

#pragma once

#include <array>

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

namespace Vulkan {
class Instance;
class Scheduler;
class Runtime;
class DriveMotion;
class Dlss;
} // namespace Vulkan

namespace Vulkan::Vr::Dlss {

/// DriveAA's pass goes through here (multiview stereo is on).
bool Active();

/// The rasterizer, each compute dispatch (its shader and bound images): the deferred lighting's combine (0x2c918c06)
/// writes the frame's lit scene (R11G11B10F, the render size), where the transparent draws go on after it.
void OnDispatch(VideoCore::TextureCache& texture_cache, const Shader::Info& cs, const VideoCore::ImageId* images,
                size_t count);

/// The rasterizer, before each draw: in VR the game's own anti-aliasing is off (with motion blur and depth of field,
/// the VR patch set), so DLSS runs on the lit scene when it is finished: at the first draw of the bloom (vertex shader
/// 0xb834cdf9) after the main view's G-buffer. The lit scene then, else none.
/// Once the scene's composite has been seen, DLSS runs on it instead, at the next draw after the one drawing it (pixel
/// shader 0x9eff1777 copies the finished lit scene into it, at the render size, for the display pass). On the lit
/// scene, an upscaled result went through that copy back down to the render size and up again in the display pass:
/// blurred, and fine detail doubled. BB_DLSS_AT=lit: the lit scene always.
VideoCore::ImageId AaInput(u64 vs_hash, u64 ps_hash = 0, VideoCore::ImageId color0 = {});

/// Stereo::OnDraw, each G-buffer draw: its vertex shader's 512-byte camera constants (dwords 0-15 the camera-relative
/// view-projection, row vectors; 96-97 the target size; 108-110 the camera's world position). The main view's (the
/// render size) give the camera motion of each frame.
void OnCameraConstants(const float* constants);

struct Inputs {
    VideoCore::Image* color;  ///< the lit HDR picture the game's anti-aliasing read (two layers)
    VideoCore::Image* depth;  ///< the scene depth (two layers)
    VideoCore::Image* output; ///< what the game's anti-aliasing wrote (two layers, storage)
    std::array<float, 2> jitter;
    bool reset;
    float sharpness;
    bool upscale; ///< the result is the game's output size (DriveAA's upscaling), else the render size (DLAA)
};

/// DriveAA::OnTaa in VR: records DLSS for both eyes. False: it could not (DriveAA resets and tries again).
bool Record(const Instance& instance, Scheduler& scheduler, Runtime& runtime, DriveMotion& motion,
            Vulkan::Dlss& dlss, const Inputs& inputs);

/// DriveAA::Redirect in VR: both eyes of the upscaled picture (General layout), read with `mapping`, the swizzle of the
/// game's own view of the picture it replaces (its lit scene's view swaps red and blue). False before any.
bool Redirect(vk::ImageView& view, vk::ImageLayout& layout, vk::ComponentMapping mapping);

} // namespace Vulkan::Vr::Dlss
