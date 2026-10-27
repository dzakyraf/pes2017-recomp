# PES 2017 Android 0.1.0

The first public build of *Pro Evolution Soccer 2017* (Xbox 360) recompiled to native ARM64 code for Android.
It is a work in progress: the game boots, installs from your own disc and plays full matches, but expect rough
edges and uneven performance between phones.

**You need your own copy of the game.** Nothing from the game is included. The app needs the
**Xbox 360, Europe** release (`default.xex` of about 32 MB), as a disc image (`.iso`) or an extracted folder.
Other regions, versions and platforms will not work; the app checks the file and warns you.

## Requirements

- 64-bit ARM Android phone, Android 8.0 or newer, with a working Vulkan driver
- About 9 GB of free storage (the game files are about 6.5 GB)
- 6 GB of RAM or more recommended
- Tested on a Galaxy S20 FE (Snapdragon 865 / Adreno 650) and a POCO X7 Pro (Dimensity 8400 / Mali-G720)

## Install

1. Download `pes2017-recomp-0.1.0.apk` below and install it (allow installs from your browser or file manager if asked).
2. Open the app, tap **INSTALL FROM DISC IMAGE**, pick your ISO or the folder that holds `default.xex`, and tap **Install**. An interrupted copy carries on where it stopped.
3. Open **Settings** and accept the suggested preset (High, Medium or Low).
4. Tap **KICK OFF**.

If you installed an earlier test build you were given privately, uninstall it first: it was signed with a
different key, so Android will refuse to update it. Back up your saves first (**Settings > Back up saves**).

## What's in it

- **Installer:** install from an ISO or an extracted folder, or play a detected extracted folder in place.
- **Picture:** display resolution, internal render scale (1x to 3x), match render quality, FSR upscaling,
  multisampling for clean pitch lines, widescreen that fills the screen, and a larger match ball option.
- **Performance:** High / Medium / Low presets chosen for your phone, adaptive quality that lowers the render
  scale when the phone gets hot, FPS limit (30 to 60), a crowd on/off option, and a shipped shader cache so
  the first match stutters less.
- **Android performance hints:** the game's main thread and its rendering threads ask the system for the
  CPU and GPU clocks they need to keep the frame rate, which helps most on MediaTek phones.
- **Controls:** movable, resizable touch buttons with pad-style shoulders and Select/Start, which hide
  automatically when a physical controller is connected; gamepad support including a second pad for player 2.
- **GPU drivers:** import or download a Turnip driver on supported Adreno phones.
- **Launcher:** in-app update check, in-match graphics settings with restart, save backups, and a log and bug
  report you can share.
- **Mali GPUs:** rendering fixes for the team board over the crowd, the black bars in the stands and the
  crowd itself; a load stall on Mali after kick-off fixed; the pipeline cache is kept between runs.

## Known issues

- Performance depends heavily on the phone and its temperature. Snapdragon 865-class phones hold 60 fps in a
  match when cool; Mali phones such as the Dimensity 8400 run about 30 to 50 fps, limited by the CPU.
- With the FPS limit at 60 on Mali, the first half-minute after kick-off can drop to a few frames a second
  while the game prepares the match; it recovers on its own. A 30 fps limit avoids most of it.
- The first second of the match intro drops frames.
- The language selection screens show on every launch.
- Some cutscenes flicker.
- Custom drivers can help one phone and break another; switch back to the system driver if the game misbehaves.

When reporting a problem, include your phone model, Android version, the app version (shown in the launcher),
your settings preset and the bug report from **Log**.

## Package

- `pes2017-recomp-0.1.0.apk`, package `com.pes2017.recomp`, version 0.1.0 (build `609fa78`)
- SHA-256: `0e68286c5c19de220cfa2690903bd6ee3f297b2b7ccde12501a55970ace4db42`
- Signing certificate SHA-256: `3B:4B:41:3C:34:30:6B:2E:2A:9E:EE:46:4A:BD:39:91:D1:7F:3F:E6:2F:D5:41:3F:EB:18:66:26:B1:56:E0:EC`

This project is not affiliated with Konami, Microsoft or Xbox. Use it only with a game you own.

APK versionCode: 17
