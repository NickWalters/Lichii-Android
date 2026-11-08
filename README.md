<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->

<p align="center">
  <img src="dist/branding/lichii_icon_with_logo.png" alt="Lichii" width="600">
</p>

# Lichii for Android

An experimental Android fork of the [Lichii](https://github.com/zortos293/Lichii) Nintendo Switch / Switch 2 emulator, maintained by [Nick Walters](https://github.com/NickWalters).

This fork brings current Lichii NX2 changes together with Android-specific work on memory use, video decoding, synchronization and shader performance. Current development includes Ocarina of Time on Qualcomm Adreno / Mesa Turnip, while retaining the Exynos / Xclipse work from [pavelgajdo's Android port](https://github.com/pavelgajdo/Lichii-Android).

This is an independent, experimental fork. It is not affiliated with Nintendo or with the upstream projects it builds on.

## Downloads and installation

- **Releases:** [NickWalters/Lichii-Android releases](https://github.com/NickWalters/Lichii-Android/releases). Download an attached `.apk`, when available. Tagged `android-*` releases and the rolling test build are separate channels; check the commit and release notes before comparing builds.
- **Rolling test build:** [`android-latest`](https://github.com/NickWalters/Lichii-Android/releases/tag/android-latest) is updated after a successful Android workflow on `nx2`. It is experimental and may regress.
- **Branch builds:** successful [Android workflow runs](https://github.com/NickWalters/Lichii-Android/actions/workflows/android.yml) upload APK artifacts. GitHub sign-in may be required to download them.

Install the APK through your device's browser or file manager, allowing installs from that app when prompted. Add your games folder and configure your legally obtained keys and firmware. Back up saves and settings before changing builds. An APK signed with a different key may not install over an existing app; do not uninstall without a backup.

### Requirements

- An **arm64 Android device running Android 13 or newer** for the standard/mainline APK.
- A GPU and driver with **Vulkan 1.3** support. Driver compatibility matters; meeting the version requirement does not guarantee that a game works.
- Enough free memory and storage for the game. Switch 2 games are demanding: **16 GB RAM is recommended for OoT**, and even 12 GB devices can be closed by Android during scene loads.

Games, keys, firmware and GPU drivers are not bundled. Use only games and system files you have legally obtained. Other build flavors have different requirements and are not the default test target of this fork.

Download Android builds only from the GitHub releases linked above. Desktop builds come from [upstream Lichii's releases](https://github.com/zortos293/Lichii/releases). Unofficial download sites are not maintained by either project; one has been reported to distribute malware ([upstream #545](https://github.com/zortos293/Lichii/issues/545)).

## Status and test coverage

These are observations from specific builds and devices, not a compatibility guarantee for the latest commit.

| Test environment | Evidence and limits |
|---|---|
| Adreno 840, Android 16, Mesa Turnip | Local OoT work reached the treehouse and passed a short movement smoke test. Matrix fixtures passed paired GPU output comparisons. Sustained gameplay, multiple scenes and flash-free rendering still need validation. See the [Adreno review](docs/nx2/oot-adreno-review.md). |
| Samsung Galaxy S26 / Exynos 2600 / Xclipse 960, 12 GB, Android 17, custom Mesa RADV | Inherited Android 0.3.0 testing reported OoT gameplay at approximately 15–20 FPS at 0.5× resolution, with rendering and memory issues. These are historical results from the original Android port, not a fresh benchmark of this fork. |

Performance depends on the scene, build, accuracy settings, driver, temperature and device state. Historical desktop fixes and synthetic shader tests do not establish Android gameplay correctness or a particular FPS gain.

### Game notes

| Game | Current evidence / known limits |
|---|---|
| The Legend of Zelda: Ocarina of Time (Switch 2) | Reaches gameplay on tested Android builds; the old first-scene hang is no longer the blanket status. White glare/flashes and other rendering errors have been reported. The NX2 synchronization fixes need Android scene-by-scene verification; do not assume flash-free gameplay. |
| Pragmata | Inherited 0.3.0 notes report the startup audio error fixed, but no rendering. No new compatibility claim is made here. |
| Pokémon Pokopia | Android copy-back and frame-ahead work is retained. This is not a full-playthrough compatibility claim. |

## Android-specific work

- Hardware AV1 movie decoding through Android MediaCodec.
- Software tensor/HMMA paths, including guarded Turnip 32-lane B broadcasts and shared-B staging, with fallback paths when the required capabilities or control-flow conditions are absent.
- Macro DMA snapshots, GPU-written memory handling and NCE changes intended to reduce unnecessary synchronization without discarding required GPU dependencies.
- Memory controls for scene loads: chunked ASTC conversion, trimmed converted texture mip chains, bounded staging retention and a capped game-file decompression cache. Texture trimming can reduce image quality.
- Android driver loading, loose ExeFS game-folder support, frame-ahead settings and opt-in diagnostics through `env.txt`.

These changes coexist with upstream NX2 work; they are not all exclusive to this fork. The [0.3.0 notes](docs/android/RELEASE_NOTES.md) describe an earlier Android release, and the [Adreno review](docs/nx2/oot-adreno-review.md) records the scope and limits of the local matrix optimization.

## Starting settings

- **System:** start with Handheld mode.
- **CPU:** use the NCE backend on supported arm64 devices.
- **Resolution:** start at 0.5× for demanding Switch 2 games, then increase if memory use and performance allow it. This is not a universal recommendation for every game.
- **Driver:** use a compatible, trusted driver. Adreno testing here uses Mesa Turnip; Xclipse results used a custom RADV build. Do not install drivers intended for a different GPU family.
- **Accuracy:** compare the same scene at the same GPU accuracy. Accurate mode reduced flashes in historical OoT testing, but did not prove they were eliminated. Lower accuracy can change correctness as well as performance.

For useful comparisons, keep frame generation off, record the exact build and driver, and allow temperatures and shader-cache state to settle. Avoid carrying experimental environment overrides between builds.

## Reporting problems

[Open an issue on this fork](https://github.com/NickWalters/Lichii-Android/issues) with:

- the build/commit, phone or tablet model, chipset, RAM, Android version and GPU driver version;
- the game/update version, CPU backend, resolution, GPU accuracy, frame-generation setting and any environment overrides;
- what you expected, what happened and steps to reproduce it (a short video helps for intermittent flashes);
- the log from the affected session: `Android/data/dev.lichii.android/files/log/eden_log.txt` (`dev.lichii.android.relWithDebInfo` for test builds).

Check logs for personal information before uploading. Never upload games, keys, firmware or account data.

### Diagnostic switches

Android can't pass environment variables to an app, so Lichii reads them from `Android/data/<app id>/files/env.txt`, one `KEY=VALUE` per line (`#` starts a comment). They take effect the next time the app starts. The available switches are listed in [AGENTS.md](AGENTS.md#debugging-tools). Leave the file empty for normal play: most switches slow the emulator down.

## Building

```bash
cd src/android
./gradlew assembleMainlineRelWithDebInfo
```

Use **JDK 17**, the Android SDK platform **36**, build tools **36.0.0**, NDK **28.2.13676358** and CMake **3.31.5** (the version selected by Gradle). Set `ANDROID_HOME` to your SDK directory or configure `sdk.dir` in an untracked `src/android/local.properties`. Native dependency downloads and a substantial amount of disk space are required. On Windows, use `gradlew.bat` instead of `./gradlew`.

The APK is written to `src/android/app/build/outputs/apk/mainline/relWithDebInfo/`. This test variant uses `dev.lichii.android.relWithDebInfo`; the mainline release uses `dev.lichii.android`, so their saves, settings and logs are separate. To build the standard release and collect its outputs from the repository root:

```bash
.ci/android/build.sh -t standard -b Release
```

[Android CI](.github/workflows/android.yml) runs on every push. Successful runs upload APK artifacts; successful `nx2` runs also update the rolling `android-latest` prerelease. An `android-<version>` tag publishes a versioned release. A workflow starting, or a source merge succeeding, is not evidence of a successful APK build or an on-device test.

More documentation, inherited from upstream: [build guide](docs/Build.md), [coding guidelines](docs/Coding.md).

## Credits and license

This fork is maintained by [Nick Walters](https://github.com/NickWalters). It builds on [pavelgajdo's Android port](https://github.com/pavelgajdo/Lichii-Android), [Lichii](https://github.com/zortos293/Lichii) by Zortos, Eglantine, Eden, yuzu and Sudachi, and their contributors, including Eglantine's original developer Camille LaVey. Lichii artwork is by YOB (yobslab). Original copyright notices and third-party licenses are retained.

See [LICENSE.txt](LICENSE.txt) and the source files for the applicable license terms.
