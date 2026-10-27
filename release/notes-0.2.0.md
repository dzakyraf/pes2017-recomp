# PES 2017 Android 0.2.0

Signed with the same key as 0.1.0 and 0.1.1, so it installs over them and keeps your settings and saves. Same
requirements and Xbox 360 Europe game files as 0.1.1.

## What's new

- **Frame generation, experimental (Settings > Graphics, off by default).** The game draws 30 frames a second and a
  frame made from each two real ones is shown between them, so a 60 Hz screen stays smooth for about half the
  game and scene work, which is heat. The ball is placed from its own draw so it moves at 60. Menus and cutscenes make
  frames without motion instead of warping. Turning it on also sets the 30 fps limit and the 60 Hz screen. Fast limbs
  and the shadow can look soft, and the game has to hold a steady 30: on a phone that dips below it, the screen
  shows fewer than 60. `files/framegen_off` turns it off live for A/B runs.
- **Scoreboard and interface glitches in made frames.** Pixels that did not change between two real frames (score,
  clock, labels, the radar) are now kept as they are, and a moving player passing behind them no longer drags their
  letters along. Fixes labels such as "Replay" coming out garbled.
- **Frame generation on 120 Hz phones (POCO X7 Pro).** The game asks Android for a fixed 60 Hz (`Surface.setFrameRate`
  with `FIXED_SOURCE`), switches off the touch boost to 120 Hz, and asks again if HyperOS moves the panel away.
  The pacing follows the rate the panel really runs at, so the user does not have to change the display rate by hand.
  "Prefer 60 Hz" uses the same request.
- **Presents on their own queue.** Where the GPU family has a second queue, `vkQueuePresentKHR` runs on it with its own
  lock, so a present waiting for the screen no longer holds up the GPU thread's submissions. On the X7 Pro this
  took presents from about 41 to about 57 a second with frame generation on.
- **Less heat:** the game's spin waits sleep up to 100 us while a frame limit below 60 is set, or when Android reports
  thermal status MODERATE or worse. Anisotropic filtering is capped at 4x on Android.
- **Black frames and black kits on the Snapdragon 8 Gen 3 (Adreno 735 / 750) phones.** The frame arena is seen through
  one window per frame slot (a dynamic storage buffer offset), so it can be larger than the 128 MB storage range
  those phones read, and the unused 512 MB guest-memory shadow is dropped.
- **Bug-hunting tools, hidden until Settings > Advanced > Developer tools is on:** the black-frame guard, a black-frame
  probe (`files/black_probe`), and an Experiments section.

## Known issues

- Frame generation is experimental. On the Galaxy S20 FE with another game process left running in the background
  it averaged about 48 fps, because the game itself fell to 24 to 27 frames a second. Close other apps first.
- On the X7 Pro it shows about 57 presents a second, not a locked 60; the 30 fps game and the made frame between
  are paced by the 60 Hz panel and can still hitch when a real frame comes late.
- The scoreboard fix is checked offline against recorded frames, not yet on a phone during a replay.
- The black-probe file `files/black_probe` reads every frame back and costs about 10 fps while it exists.
- In busy scenes (goal replays, the pre-match intro) the game itself runs below 30.
- The half-time stats board can still show crowd through its text.
- Frame generation's effect on heat is not measured yet.
- Not yet checked on the S20 FE with the second present queue and the window hints.

## Not in this build

The MediaTek / Mali improvements, the quarter-step internal render multiplier, and the gamepad work in progress.

## Package

- `pes2017-recomp-0.2.0.apk`, package `com.pes2017.recomp`, version 0.2.0 (versionCode 40). Built from `main` at
  `425cb0d` plus the uncommitted frame generation, 60 Hz and present-queue changes, so the build id shown in the app
  is `425cb0d`.
- SHA-256: `843e276c2f86adc6057c0ed5288ab64110195342ac6bbeec1e6ce663b83ce1d6`
- Signing certificate SHA-256: `3B:4B:41:3C:34:30:6B:2E:2A:9E:EE:46:4A:BD:39:91:D1:7F:3F:E6:2F:D5:41:3F:EB:18:66:26:B1:56:E0:EC` (same as 0.1.0 and 0.1.1)

This project is not affiliated with Konami, Microsoft or Xbox. Use it only with a game you own.
