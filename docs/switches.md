# Switches

The runtime is configured through `PES17_*` environment variables. A flag is on
when set to anything but empty or `0` (`runtime/env.h`).

## The settings file

A file named `.env` beside the executables (in the current directory for a
development run) holds switches as `NAME=value`, one a line. The runtime reads
it first thing (`runtime/settings.cpp`) and sets each `PES17_` switch the
environment does not already have, so a variable given on the command line
decides. The launcher writes its own settings there (RESOLUTION is
`PES17_SCALE`, FPS LIMIT is `PES17_FPS_LIMIT`) and keeps the other lines. When the
file has no `PES17_FPS_LIMIT`, the launcher writes the screen's refresh rate as
it opens (60 for a 60 Hz screen, which is the console's pacing). `PES17_LAUNCHER_SOUNDS=0`, written there
by hand, keeps the launcher's own menus silent.

Switches marked **R** are read by every build. The rest are diagnostic: a build
without diagnostics (`RELEASE=1 ./build.sh`, `-DPES17_DIAGNOSTICS=OFF`) treats
them as unset whatever the environment says, and also drops the keys `F7`, `F10` and `F11`, `kill -USR2` and the end-of-run reports.

## Running

| switch | | effect |
|---|---|---|
| `PES17_WINDOW=0\|1` | R | the window, which every build opens; `PES17_WINDOW=0` keeps it shut, for a headless run. Closing it ends the run as `PES17_RUN_SECONDS` does, with the reports. The frame keeps its proportions in any window shape |
| `PES17_FULLSCREEN=1` | R | starts fullscreen, borderless at the desktop's resolution. `F8` toggles it |
| `PES17_LOG_FILE=<path>` | R | writes the log there, creating its folder. The log goes to stderr, so `> file` does not catch it. |
| `PES17_REPORT=1` | R | what a bug report needs, as the launcher's REPORT A BUG sets it: the log names the graphics driver and ends with the run's frame rate, frame time spread and what was compiled at a draw |
| `PES17_LOG_TIME=1` | | stamps every log line with the time since start |
| `PES17_LOG_ALL=1` | | writes every line as it comes. Without it a line that repeats is written once and then counted -- the 1st, 2nd, 4th, 8th ... with the size of the repetition beside it, which is never above the truth and within a factor of two of it -- and two lines that differ only in an address, a size or a shader hash are one kind of line. A run that logs one thing a thousand times buries the line that says what it was |
| `PES17_NO_AUTO_SAVE_DEVICE=1` | R | leaves the save-device choice to the title's prompt instead of choosing the hard disk |
| `PES17_NO_AUDIO=1` | R | opens no playback device; the mixer and decoder still run ([audio.md](audio.md)) |
| `PES17_RUN_SECONDS=<n>` | | reports and exits after n seconds |
| `PES17_WATCHDOG=<seconds>` | | when it fires: the `PES17_DUMP` blocks, what every guest thread is blocked on (handle and caller) or spinning on, and every thread's backtrace, which is the guest call stack; then exits |
| `kill -USR2 <pid>` | | every thread's stack, as the watchdog prints them, and the run continues (Linux). Sent a few times, it tells a stuck thread from a looping one |
| `PES17_INPUT_SCRIPT="20:start,26:a"` | | presses pad inputs at wall-clock seconds. An entry names a button (`a b x y lb rb start back up down left right lthumb rthumb`), a trigger (`lt`, `rt`) or a stick direction (`lx+ lx- ly+ ly- rx+ rx- ry+ ry-`), with an optional hold time: `"40:lt:25"` holds the left trigger 25 s. Default hold 0.2 s. A name ending in `@2`, `@3` or `@4` is that player's controller, which the script then stands in for: `"5:a@2"`. `guide` is the Guide button, which opens the sign-in screen |

## The guest

| switch | effect |
|---|---|
| `PES17_TRACE_INDIRECT` | CMake option, on by default: reports an indirect call with no recompiled target instead of jumping to null |
| `PES17_TRACE_FILES=1` | every file opened, read and written. The files asked for and not found are listed at exit regardless |
| `PES17_TRACE_XMA=1` | every XMA decoder kick with its buffers, and FFmpeg's log |
| `PES17_DUMP=<hex>[:words][,...]` | blocks of guest memory to print when the watchdog fires (16 words by default) |
| `PES17_WATCH=<hex>[:bytes]` | a write watchpoint (Linux): the page is made read-only, and every guest write into the range is reported with its call stack and thread, up to `PES17_WATCH_REPORTS` (20); the command processor's writes there are reported too. It logs when it gives up on a busy page |

## The command stream

| switch | effect |
|---|---|
| `PES17_DUMP_SHADERS=<dir>` | every distinct shader's microcode, for `tools/xenos_shader.py` and `translate-shader` |
| `PES17_DUMP_TEXTURES=<dir>` | every distinct bound texture, untiled, as PNM with each mip level, its fetch constant logged. `PES17_DUMP_TEXTURES_RAW=1` also writes the guest bytes; `PES17_DUMP_TEXTURES_AFTER=<seconds>` starts later, since a streamed texture is empty at its first bind |
| end-of-run reports | packets by opcode, frame times, draws and presents per second, arena copies, draws per render target, texture and shader work |

## Drawing

| switch | | effect |
|---|---|---|
| `PES17_SCALE=<2 or 3>` | R | draws every surface that many times wider and taller than the title's: a 2560x1440 or 3840x2160 frame. The launcher's RESOLUTION entry sets it in the settings file ([rendering.md](rendering.md#resolution-scale)) |
| `PES17_ASPECT=<w/h>` | R | a screen wider than 16:9 (up to 3): the title's cameras are given that aspect (the 16:9 floats at `T_DATA_Aspect16x9`/`b`, `title.h`) and the 1280x720 frame is shown stretched to it, so the view is wider and the pitch keeps its shape; the interface is stretched. The Android app sets it from the screen under Settings > Widescreen |
| `PES17_FPS_LIMIT=<n>` | R | how many frames a second the title draws at most. Unset or 60 is the console's pacing, a frame at every blank. Any other number takes the title off the blank, with that number as its own limiter's; 0 is no limit. The limiter counts whole milliseconds a frame, so 120 gives 125 and 144 gives 166. The launcher's FPS LIMIT entry sets it in the settings file ([rendering.md](rendering.md#more-than-60-frames-a-second)) |
| `PES17_MSAA=<n>` | R | draws every surface the title multisamples at n samples, rounded down to what the device offers |
| `PES17_NO_MSAA=1` | R | draws the title's 2x and 4x surfaces at one sample |
| `PES17_FRAMEGEN=1` | R | frame generation ([frame-generation.md](frame-generation.md)): with the title at half the display's rate (`PES17_FPS_LIMIT=30` on 60 Hz) the window shows a frame made from each two real ones between them. `files/framegen_off` turns it off live. Settings > Graphics > Frame generation sets it and the 30 limit |
| `PES17_MAX_ANISO=<n>` | R | caps the title's anisotropic filtering at n:1 (1 is none; 16 by default, 4 on Android) |
| `PES17_YIELD_SLEEP_US=<n>` | R | the most a guest spin wait sleeps per yield (`runtime/kernel/yield_backoff.h`); 0 is plain yields. Unset: 100 under an FPS limit below 60 or while Android reports thermal status MODERATE or worse, plain yields otherwise |
| `PES17_ARENA_MB=<n>` | R | the upload arena for constants, vertices and indices, shared by the frame slots (512) |
| `PES17_TEXTURE_BUDGET_MB=<n>` | R | the texture cache's budget, past which it lets go of what has not been bound lately, even textures whose memory is unchanged (defaults to half the device-local memory within 256 MB to 2 GB on desktop, and 128 MB on Android) |
| `PES17_PIPELINE_CACHE=<file>` | R | keeps the driver's compiled pipelines across runs |
| `PES17_SHADER_CACHE=<file>` | R | records every pipeline a run needed and builds them at the next start-up |
| `PES17_NO_PIPELINE_LIBRARIES=1` | R | builds each pipeline whole at its first draw instead of linking it from shaders compiled at load ([rendering.md](rendering.md)) |
| `PES17_TRACE_SHADER_LOADS=1` | | each shader compiled at load and each the command stream loads, with its hash |
| `PES17_TRACE_DRAWS=<n>` | | describes n draw packets in full; `PES17_TRACE_DRAWS_AFTER=<presents>` starts later |
| `PES17_TRACE_FRAME=<n>` | | narrates n frames' render targets and resolves |
| `PES17_TRACE_RENDER_AFTER=<frames>` | | where the traces below start |
| `PES17_TRACE_PASSES=<n>` | | a frame's passes in order: target, colour mask, depth state, scissor |
| `PES17_TRACE_SHADER_VERTS=<hex>[,...]` | | for draws with those shaders: the first float constants, the fetch constants and the vertices they name |
| `PES17_RENDER=0` | | executes and counts the stream but draws nothing |
| `PES17_ONLY_SHADER=<hex>[,...]`, `PES17_SKIP_SHADER=<hex>[,...]` | | draws only, or all but, the draws using those shaders |
| `PES17_NO_CULL=1`, `PES17_NO_DEPTH_TEST=1` | | take culling or the depth test out of every pipeline |
| `PES17_NO_SHADOW=1` | | no shadow of guest memory: every draw copies its vertex data into the arena. It is 512 MB behind the arena, so Android sets it itself on a device of six gigabytes or less |
| `PES17_ADRENO_BC=hw\|dxt1` | | how much of the Snapdragon workaround applies when the driver's own BC sampling is on: `hw` keeps it entirely, `dxt1` keeps it and decodes only the BC1 family in software. Unset is every BC format in software, which is the default on Adreno ([textures.md](textures.md)) |
| `PES17_FORCE_BC_HW=1` | | on an Adreno device: leave BC sampling to the driver rather than the software decoder |
| `PES17_FORCE_BC_DECOMPRESS=1` | | decode every BC texture in software on any device, which is how a desktop driver is made to exercise the decoder the Snapdragon path uses |
| `PES17_RECORD_THREAD=0` | | makes the Vulkan calls on the ring consumer instead of the recorder thread |
| `PES17_TIME_RENDER=1` | | where the ring consumer's time goes, and how long it waits on the GPU |
| `PES17_TRACE_PACING=<file>` | | a timeline written at exit: the consumer's batches and swaps, the title's presents, and every guest thread's engine waits, kernel waits and sleeps with their callers |
| `PES17_STUTTERS=1` | | (at the console's pacing only, not under `PES17_FPS_LIMIT`) from the first 30 consecutive world frames on, logs `STUTTER` for every world frame 25 ms or more after the previous one, with what the renderer spent the gap on, and `DISPLAY` when a frame is held or dropped by the window; totals at exit. When play stops for a second it logs `STALL` with what every guest thread waits on and every thread's stack (Linux), once a stall. The pad's Y and `F7` log a `STUTTER MARK` |

## Frames and captures

| switch | effect |
|---|---|
| `PES17_DUMP_FRAMES=<dir>` | finished frames as PNM, every `PES17_DUMP_FRAME_EVERY` frames (600); `PES17_DUMP_FRAMES_AFTER_SECONDS` starts later, `PES17_DUMP_WORLD_FRAMES=1` keeps only frames that drew the world |
| `PES17_DUMP_TARGETS=1` | every colour surface beside the presented frame |
| `F7` | with the window open: marks a flash in the log. `PES17_FLASH_FRAMES=<n>` keeps the last n frames and writes them at each mark into `PES17_FLASH_DIR` (`diagnosis/flashes`). `PES17_FLASH_HUNT=1` (or `PES17_FLASH_DRAWS=1`) also writes every draw of the kept frames as `draws_rNNNNNN.txt`; `PES17_FLASH_HUNT=1` makes the pad's Y a mark and logs every texture drawn empty. `PES17_MARK_EMPTY=1` paints those magenta. `PES17_FLASH_MARK_AT=<seconds>` makes a mark nobody pressed. `tools/flash_hunt.sh` sets this up |
| `PES17_RENDERDOC=<seconds>[:<frames>]` | captures that many world frames (4) once the run is that old; `PES17_RENDERDOC_OUT` names the file prefix. RenderDoc must be injected before the Vulkan instance exists: run under `tools/capture_by_hand.sh` or `renderdoccmd capture` |
| `F10`, `F11` | with the window open: F10 captures the next four world frames; F11 captures every frame until pressed again (at most 120). The window runs a frame behind, so the picture in capture N was rendered in frame N-1 |
| validation | `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` enables the validation layer; its messages go to the log as `validation error:` / `validation warning:` lines, counted at exit. Add `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` for hazards between submissions |
