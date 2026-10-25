#pragma once
// Frame generation: the title draws at half the display's rate (PES17_FPS_LIMIT=30 on a 60 Hz
// screen) and the window shows a made frame between each two real ones, so the screen still gets
// a new picture every blank while the guest and the scene cost half as much -- which on a phone
// is heat (docs/frame-generation.md).
//
// The made frame comes from the two real ones alone; this console renderer keeps no motion
// vectors. Compute passes (framegen/*.comp): a luma pyramid at a quarter and a sixteenth of the
// frame, symmetric block matching on 4x4 blocks of each level (coarse first, for camera pans; the
// fine level starts from it and pays for straying from it), a median over each field, and a warp
// of both frames halfway along the fine field that shows the previous frame where the two
// disagree (the ball), rather than two half-strength copies. About 0.25 ms on an M3 Pro (tools/framegen_check).
//
// Off unless PES17_FRAMEGEN=1; it also needs the title off the blank at no more than half the
// display's rate, and stays out of the way otherwise. `files/framegen_off` turns it off live.
#include <cstdint>
#include "loader.h"

namespace vk::framegen
{
    // Whether frames are to be made, for a title limited to `limit` fps on a display whose blank
    // is `displayPeriodNs`. Cheap; asked once a frame.
    bool Wanted(uint32_t limit, double displayPeriodNs);

    // Whether frame generation was asked for (PES17_FRAMEGEN), so the renderer finds the ball. Cheap.
    bool WantsBall();

    // Where frame `serial` drew the ball (centre and radius in the frame's pixels), from the
    // renderer, which knows the ball's draw. The made frame puts the ball halfway between its two
    // real positions itself: the motion field cannot follow something that small.
    void NoteBall(uint64_t serial, bool seen, float x, float y, float radius);

    struct Result
    {
        VkImage made = VK_NULL_HANDLE;   // the frame between the last real one and this, or none
        VkImage real = VK_NULL_HANDLE;   // this real frame, copied: what to show after `made`
    };

    // Records, into `commands`, the copy of the renderer's frame `source` (TRANSFER_SRC layout, left
    // in it) into the history and, when the frame before it is recent enough to pair with, the made
    // frame between them. Both returned images are in TRANSFER_SRC layout, `width` x `height`, and
    // stay valid until the next call. `nowNs` is when the frame was taken, on a steady clock.
    // A null `real` means frame generation cannot run (the caller shows `source` as before).
    Result Record(VkDevice device, VkPhysicalDevice physical, VkCommandBuffer commands,
                  VkImage source, uint32_t width, uint32_t height, int64_t nowNs, double displayPeriodNs,
                  uint64_t serial = 0);

    // Forgets the history, so the next frame is shown without a made one before it.
    void Reset();

    // The fine motion field of the last pair (RGBA16F, x and y in quarter-frame pixels, then the
    // match's cost) in TRANSFER_SRC-compatible GENERAL layout, and its size; for tools/framegen_check.
    VkImage DebugField(uint32_t& width, uint32_t& height, uint32_t level);

    // Frames made so far, for the presenter's report.
    uint64_t MadeFrames();

    // Everything it made. The device must be idle.
    void Destroy(VkDevice device);
}
