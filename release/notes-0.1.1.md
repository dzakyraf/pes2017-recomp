# PES 2017 Android 0.1.1

A small update to 0.1.0. It installs over 0.1.0 and keeps your settings and saves. Same requirements and
install steps as 0.1.0, and the same Xbox 360 Europe game files.

## What's new

- **Faster match intro on Mali phones** (POCO X7 Pro and similar): images are now copied out of the staging buffer
  in one go, which removes most of the slow first seconds after kick-off.
- **Crowd on Mali:** the crowd is drawn from the game's own memory instead of an uncached copy, so it appears
  reliably.
- **FSR switch now does what it says.** Settings > Sharper upscaling (FSR) was ignored: the upscaler ran on
  every phone even with the switch off. It is now off unless you turn it on, so phones set to High get the
  plain picture and a little more speed.
- **Android 16 performance hint on by default** for the graphics pipeline (ADPF session), with a "big cores"
  pinning option for testing.
- **Better bug reports:** the CAPTURE button (Settings > Advanced > Developer tools) now also saves what the
  engine was actually running with, the screen's refresh rate and modes, the Vulkan device and driver, how
  earlier runs ended, and a small snapshot of the screen every half second.
- **Manage captures** (Settings > Storage): see your captures, **Share** one as a zip, **Save to Downloads**
  (`Download/PES17/`), or delete one or all.
- If the GPU is lost, the game log now says where and why, where the driver can tell.

## Known issues

Same as 0.1.0, plus:

- On some phones with a Snapdragon 8 Gen 3 (Adreno 750), the picture may flash black on one frame in three and
  some players look fully black. Under investigation: please send a capture (Manage captures > Share) with
  your report.
- The half-time stats board can still show crowd through its text.

## Package

- `pes2017-recomp-0.1.1.apk`, package `com.pes2017.recomp`, version 0.1.1 (versionCode 18, build `86678ff`)
- SHA-256: `b8dfe52ab5fce3bcc466c2484b34a3fe9e04a0481dc43af18036f2a18baff1d9`
- Signing certificate SHA-256: `3B:4B:41:3C:34:30:6B:2E:2A:9E:EE:46:4A:BD:39:91:D1:7F:3F:E6:2F:D5:41:3F:EB:18:66:26:B1:56:E0:EC` (same as 0.1.0)

This project is not affiliated with Konami, Microsoft or Xbox. Use it only with a game you own.
