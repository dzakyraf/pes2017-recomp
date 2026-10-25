# In-match performance on the phone (Galaxy S20 FE, Adreno 650)

Measured with the `play` controller macro (Settings: CAPTURE button / `MACRO` broadcast) so every run
is the same input from game start to a match. Tools added for it:

- `files/gpu_timing` (or `PES17_GPU_TIMING=1`): GPU timestamps around every render pass; a
  `gpu:` line every 600 frames lists the costliest pass kinds (size, samples, depth) and ms a frame.
- `files/yield_tuning` ("<sleep us> <free yields>") and `PES17_YIELD_SLEEP_US`: back-off for the
  guest's `NtYieldExecution` spin loops (`runtime/kernel/yield_backoff.h`).
- `files/small_passes` (or `PES17_SMALL_PASSES=1`): render passes open over what their draws touch (a clearing rectangle is sized from its own corners, `ClearRectangleReach`). Off by default: it clipped the crowd to a rectangle.
- `files/scene_scale`, `files/no_msaa`: existing overrides.

## What was found

- The limit in a match is heat. At skin 47-48 C (thermal status CRITICAL) the GPU is held to 305-400 MHz and
  the big CPU cores to about 1.5 GHz, and fps swings 42-60 between identical runs. A cool phone ran 57-60.
- CPU: a quarter of all cycles are the guest's yield spin loops (kernel `sched_yield`) on the main thread and
  three workers. Sleeping in them cut cycles by a fifth at 100 us but cost about 6 fps (the waits are latency
  critical); a gentle variant (30 us after 20000 yields) changed nothing. Off by default.
- GPU: about 8 ms a frame in 20-22 passes. Three `960x1536` passes cost about 1 ms each and the `960x624`
  ones about 0.4 ms each. The `960x1536` surface is the scene's colour+depth at 0.75 scale, 2048 rows tall in
  the title's pixels, while the picture is 540. They began with a clear rectangle whose scissor is the whole
  surface, so passes now take their size from the rectangle's corners. The scene passes are 960x576 now
  (about 1 ms each, so most of that cost is the shading, not the load/store), total pass time fell about 20%
  (about 11-12 ms to 9-10 ms a frame), fps did not move beyond the run-to-run noise (47 vs 47, 40 vs 43),
  and the match picture in a CAPTURE screenshot looks right.
- Pipeline/shader cache writes were throttled to one in 30 s (they were every 5 pipelines, on the draw thread
  and under the pipeline-build lock): the cutscene start froze for about 5 s while a warmup compiled.

- `PES17_STREAM_TRACE=1` (or `files/stream_trace`): `stream:` lines for the title's own asset
  streaming (`runtime/stream_trace.h`): each guest file read (thread, core, file, fread ms), each
  texture first uploaded or restaged, and every 500 ms each thread's ms running and ms runnable
  (sampled from `/proc/self/task/*/stat`; this kernel has no schedstat, and a thread name is cut
  to 15 characters, so two guest threads can share one). Measured on the S20 FE, cold start: the
  loader thread (`dt*_x36.cpk` reads, 78 MB in 30 s after MatchSetup) spends 0.3 s in `fread` and
  sleeps on a futex for about 25 s of that, so streaming is paced by the game, not by the disk or
  by core starvation; `PES17_PINNING=off` changed nothing for it. Texture work on the command
  thread is small (decode+read+stage about 1.2 s for the whole session). What a player sees as
  popping is the game's own sequence of loads; the visible hitching and missing draws of a cold
  start came from pipeline builds (see the bundled shader seed in shaders.md).

## Heat cuts added 2026-10-26 (S20 FE)

Each trades a little quality or latency for less work, since heat is what limits a match. None
has had its effect on temperature measured yet (see the test below).

- **Spin waits sleep when the frame has room** (`runtime/kernel/yield_backoff.h`). A quarter of
  the CPU went to the guest's yield spin loops. With `PES17_YIELD_SLEEP_US` unset, they now sleep
  up to 100 us once they spin, but only under an FPS limit below 60 (the spin only waits out the
  limiter) or while Android reports thermal status MODERATE or worse (from the thermal listener,
  `nativeSetThermalStatus`). At 60 on a cool phone they stay plain yields, since sleeping there
  cost about 6 fps. An explicit `PES17_YIELD_SLEEP_US`, or `files/yield_tuning`, fixes the cap;
  0 turns it off. The game log's `yield:` line says which mode is on. The S20 FE still has a
  `files/yield_tuning` of `0 32` from earlier measuring, which keeps this off there.
- **Anisotropic filtering capped at 4x on Android** (`PES17_MAX_ANISO`, `texture_cache.cpp`,
  default set in `android/perf.cpp`). The title asks for up to 16x; the slanted pitch and stands
  are most of the frame. The far end of the pitch is a little softer. `PES17_MAX_ANISO=16` gives
  the old look, `1` the cheapest.
- **Frame generation** (`PES17_FRAMEGEN=1`, Settings > Graphics > Frame generation). The game
  draws 30 frames a second and the window shows a frame made from each two real ones between
  them, so the 60 Hz screen stays smooth for about half the guest and scene work. The made
  frame costs a few compute passes (0.23 ms on an M3 Pro; not timed on a phone yet) and adds one
  real frame of latency. Pitch, camera and players move at 60, and so does the ball: the
  renderer reads its exact position from its draw. What the motion search cannot follow (fast
  limbs, the ball's shadow) is shown whole at 30. The S20 FE showed 61 fps on screen with the
  game at 30 fps, and the user found the ball smooth. Design, results and what was dropped:
  [frame-generation.md](frame-generation.md).

The test still to run, on a cool, unplugged phone with the `play` macro, about 5 minutes in a
match each, temperatures from `dumpsys thermalservice` (SKIN, AP) every 10 s: 60 fps; 30 fps;
30 fps with frame generation. 30 against 60 says what halving the work saves; frame generation
against plain 30 says what the made frames cost. Note: the S20 FE dropped off USB three times
when hot and under `adb shell screenrecord`; record with the phone's own recorder instead.

## Leads not tried yet (workload and heat)

- **Sun shadow map at a lower resolution.** The shadow map is one 1024x2048 depth surface holding two
  cascades, filled by two resolves a frame ([rendering.md](rendering.md)). Targets already carry
  their own scale (`target.scale`, set from `SceneScaleFor` in renderer_targets.cpp), and texture
  fetches normalise over the image, so a half-size shadow image would be sampled in the right place.
  What has to move with it: the resolve rectangles and the row a second resolve lands on
  (`resolveSurfaces`, renderer_resolve.cpp), and the passes' viewport and scissor. The cost is the
  title's own filtering: its PCF offsets and depth bias are in 1024-texel units, so shadows get softer
  and may shimmer or acne. Check with a CAPTURE screenshot and `files/gpu_timing` (shadow passes'
  ms) before keeping it; a switch such as `PES17_SHADOW_SCALE=0.5` keeps it optional.
- **The ball's shadow under frame generation:** it is held at 30 while the ball moves at 60. The
  renderer could find the shadow's draw the way it finds the ball's and place it the same way.

