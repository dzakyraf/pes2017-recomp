# Frame generation

The idea: the title runs at the 30 fps limit (`PES17_FPS_LIMIT=30`) and every second
displayed frame is made between two real ones, so the screen shows 60. The guest's CPU
work and the scene's GPU work halve. One extra pass is added per generated frame. In a
match the limit is heat ([performance.md](performance.md)), so this is a heat lever
first and an fps lever second.

## Status (2026-10-26): built, not yet measured on a phone

`PES17_FRAMEGEN=1` (Settings > Graphics > Frame generation, which also sets the 30 fps limit and
the 60 Hz screen) turns on `runtime/gpu/vulkan/framegen.{h,cpp}`. The presenter
(`DrawOneFrame`) then presents each real frame twice: first the frame made between it and the
one before, then the real frame one blank later (FIFO paces them). It only runs when the title's
limit is half the display's rate (1.9 to 2.1 blanks a frame) and a frame came within five blanks
of the last; otherwise frames are shown as before. `files/framegen_off` turns it off live, and
the presenter report has a `framegen:` line with the count of made frames.

How a frame is made (compute shaders in `runtime/gpu/vulkan/framegen/`, compiled into
`framegen/shaders.h` by `tools/build_framegen_shaders.sh`):

1. Copy the renderer's frame into a history image (the renderer gets its image back at once).
2. Luma at 1/4 and 1/16 of the frame (`downsample.comp`).
3. Symmetric block matching (`search.comp`) on 4x4 blocks: the 1/16 level first (window of +-8
   of its pixels, +-128 of the frame's, enough for a camera pan), then the 1/4 level starting
   from it (+-2 window). A vector pays for straying from the predicted one, or flat grass picks
   noise. Last frame's field is a candidate too.
4. 3x3 median of each field (`smooth.comp`).
5. Warp (`warp.comp`): previous frame at -v/2, current at +v/2, averaged. "No motion" wins only
   when clearly better, which keeps the HUD still over a moving pitch. Where the two ends
   disagree, the previous frame is taken instead of the average (fetched along the coarse,
   camera-only vector where the fine field follows the camera), so whatever the field could not
   follow is shown whole at the title's 30 a second rather than as two half-strength copies.

The ball (seen on the S20 FE, 2026-10-26, Samsung screen recording at 60 fps): with plain
averaging every made frame broke the ball into fragments and split its shadow, even at a slow 5
pixels a real frame, so it shimmered. The ball is about 7 pixels, under half a 16-pixel block, so
the field there follows the pitch, and at the ball's halfway point that vector lands on grass at
both ends and agrees. Taking the previous frame where the ends disagree keeps the ball solid;
the ball then moves at 30 while the pitch moves at 60.

The ball at 60 (2026-10-26, checked on the S20 FE): the renderer knows the ball's draw (4020
indices, vertex shader 5867DE0B52B6EFCE, the one `PES17_BALL_SCALE` enlarges), so for that draw it
runs 24 of the ball's vertices through the vertex shader on the CPU (`NoteBall` in
renderer_draw.cpp), divides by w and applies the viewport: the ball's exact centre and radius in
the frame's pixels, handed to framegen by frame serial. The warp then copies the ball from the
current frame to the midpoint of its two centres and fills the spots it left and reaches from
the frame where it is not. In live play the ball is found in about 600 of every 600 frames
(radius 6-9 pixels); where it is not (kick-off, replays) the hold above applies. Its shadow is
still held at 30.

Checked on a Mac (`tools/framegen_check.sh before.ppm truth.ppm after.ppm [made.ppm]`, which runs
the same framegen.cpp on MoltenVK) with frames from a phone recording of live play: the made
frame keeps pitch lines single and players in place, where a plain blend doubles them; fast
limbs come out soft. PSNR against the real middle frame: 28.0 dB made, 27.0 average, 25.9 repeat
(h264 footage limits all three); synthetic pans 37.9 vs 27.4. GPU time 0.23 ms on an M3 Pro,
so expect a few ms on a throttled Adreno 650.

Tried and dropped: choosing per pixel among the nearby block vectors by how well their ends agree
(flat grass agrees with any vector, so lines and players tore apart); a single quarter-size level
(camera pans moved more than its window); a forward search from the previous frame for small
objects (blocks that do not follow the background), splatted to where they are halfway, with the
spots they left and reached taken from one frame (it found the synthetic ball in only two or
three of five cases, and took grass texture and parts of players for objects). Not done: scene cuts (a cut gives one blended frame),
a separate interface layer.

## What our renderer can feed a generator

- **Colour without the interface.** The title resolves the screen into a 1280x720
  texture *before* it draws the interface, for the next frame's blur
  ([rendering.md](rendering.md)). That is the clean scene a generator wants. The
  interface would then be drawn over the generated frame from the last real frame, or
  the generator works on the final frame and the static HUD (score, radar) comes out
  as zero motion.
- **Depth.** Scene depth is resolved and sampled as a texture by the title already.
- **Motion vectors: none.** This is a 2006-era console renderer, with no velocity
  buffer. So any technique that needs game motion vectors either gets zeros (it then
  leans entirely on optical flow) or we derive them. Camera-only vectors could be
  rebuilt from depth and the view-projection matrix in the world shaders' float
  constants. Players and ball would still need optical flow.
- **Pacing.** The presenter owns the swapchain (FIFO, four images,
  `runtime/gpu/vulkan/presenter.cpp`). A generator slots in at the swap: it keeps the
  previous real frame and shows interpolated, then real, at even intervals. The cost is
  one real frame of extra latency (33 ms at 30).

## Options, ranked for our phones

| option | devices | inputs | access | fit |
|---|---|---|---|---|
| Our own optical-flow interpolation (compute) | every Vulkan phone | two colour frames (+ depth) | ours | **best fit**: no vendor gate, works on Adreno 650 and Mali-G720 |
| AMD FidelityFX Frame Interpolation + Optical Flow (FSR 3.1) | any Vulkan 1.1+ with compute, 16-bit storage | two back buffers, dilated depth, dilated motion vectors, optical flow | MIT, Vulkan backend in the FidelityFX SDK | possible base for the above; built for desktop GPUs, mobile cost unknown, wants motion vectors |
| MediaTek MFRC (Frame Rate Converter) | Dimensity (MFRC 2.0+ on 9400-class; the X7 Pro's 8400 not confirmed) | colour, UI-separated or not; motion and depth optional | partner SDK, no public download | colour-only input suits us; needs a MediaTek developer contact |
| Qualcomm Adreno Neural Fusion (successor to Frame Motion Engine) | Snapdragon 8 Elite Gen 6 and newer only | colour, depth, motion vectors; UI drawn separately | `libanf.so` binary licence, BSD headers | neither test phone qualifies, nor 8 Gen 1/2/3 or 8 Elite; needs motion vectors. The older Frame Motion Engine (AFME 2.0, documented on 8 Gen 3) had no public SDK: OEMs and Qualcomm tuned it per game |
| Arm Neural Frame Rate Upscaling (NFRU) | Mali with neural accelerators (G2-Ultra NX, 2026) | through `VK_ARM_tensors`, `VK_ARM_data_graph`, `VK_ARM_data_graph_optical_flow` | Arm AI model licence; emulation layer today | no shipping driver for the G720; future option |
| Lossless Scaling (LSFG) / lsfg-vk / LSFG-Android | Adreno 7xx+ | final frame | needs the paid Lossless.dll; lsfg-vk is now CC BY-NC-ND | not usable in the app; screen capture adds 50-80 ms |
| Phone's own MEMC / "game frame interpolation" (HyperOS Game Turbo, display chips) | some Xiaomi/OnePlus/etc. | none from us | whitelisted packages only | the user can check Game Turbo; nothing to build |

## Recommended path

1. **Prove the gain first, without generation:** `PES17_FPS_LIMIT=30` against 60 on
   the same match, comparing skin temperature, battery current and clocks over 10
   minutes. If 30 does not run much cooler, generation is not worth building.
2. **Prototype a cheap interpolator** in the presenter, behind `PES17_FRAMEGEN=1`.
   Block-match optical flow at quarter resolution (8x8 blocks over 320x180), one
   smoothing pass, then a warp of both frames halfway with occlusion-aware blending.
   Work on the pre-interface resolve, and draw the last real interface on top, if the
   interface can be separated; otherwise use the final frame. Budget: under 2 ms on
   the Adreno 650 at its throttled clock. Use FSR 3.1's frame interpolation shaders
   (MIT) as the reference for the occlusion and disocclusion handling.
3. **Measure** with `files/gpu_timing` (add the interpolation pass to it), CAPTURE
   screenshots of generated frames during fast play (ball, players crossing, camera
   pans), and the skin temperature over a whole half.
4. **Vendor paths later:** ask MediaTek about MFRC for the Dimensity phones. Revisit
   ANF and NFRU when a test phone supports them.

Known risks: halos around players on a green pitch with little texture (flow finds
the pitch easily, but thin lines and the ball are small); scene cuts and replays
(detect a large frame difference and show the real frame twice); the title's
30 fps cutscenes (flip interval 2) already run at 30, and generation would lift them to 60.

## Sources

- Qualcomm Adreno Neural Fusion: https://github.com/SnapdragonGameStudios/adreno-neural-fusion,
  https://videocardz.com/newz/qualcomm-releases-adreno-neural-fusion-sdk-with-ai-upscaling-and-2x-frame-generation-for-snapdragon-gpus
- MediaTek MFRC: https://www.mediatek.com/tek-talk-blogs/mediatek-and-nte-neverness-to-everness-announce-deep-collaboration-to-advance-mobile-gaming-performance
- Arm NFRU: https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/introducing-neural-frame-rate-upscaling-in-arm-neural-graphics-sdk-for-game-engines-and-ue-plugins,
  https://learn.arm.com/learning-paths/mobile-graphics-and-gaming/nfru-unreal/2-set_up_the_development_environment/,
  https://huggingface.co/Arm/neural-frame-rate-upscaling
- AMD FidelityFX frame interpolation and optical flow: https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/,
  https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/, https://gpuopen.com/learn/amd_fsr_3_1_release/
- Native Vulkan FSR 3 frame interpolation module: https://github.com/FireBurn/FSR-Vulkan
- LSFG on Android and lsfg-vk licence: https://github.com/FrankBarretta/LSFG-Android,
  https://www.gamingonlinux.com/2026/08/lossless-scaling-frame-generation-on-linux-lsfg-vk-moves-away-from-github-changes-license-due-to-ai-slop-forks/
