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
- **Frame generation:** render 30, show 60. See [frame-generation.md](frame-generation.md).

