# Android

VICTUS games run on Android arm64 phones and tablets (Android 9 / API 28 and newer; Vulkan 1.1, or OpenGL ES 3.0 without it).
The Android app is a thin shell: SDL's `SDLActivity` owns the window, input, audio and lifecycle,
and the whole engine plus the player run natively from `libmain.so`. The game itself is the same
`game.vpak` that `victus export` produces for desktop.

## Requirements

| Tool | Version | Notes |
| --- | --- | --- |
| JDK | 17 or newer | Temurin 17 is used in CI |
| Gradle | 9.6 or newer | no wrapper is committed; CI uses 9.9.1 |
| Android Gradle Plugin | 9.5.1 | declared in `platforms/android/build.gradle` |
| Android SDK | platform 36, Build Tools 36.0.0, CMake 3.31.6 | `sdkmanager "platforms;android-36" "build-tools;36.0.0" "cmake;3.31.6"` |
| NDK | r29 (29.0.14206865) | `sdkmanager "ndk;29.0.14206865"` |

## Building an APK

```sh
# 1. Export the game on the desktop (any template or project).
victus export path/to/project --out out/mygame --name mygame

# 2. Build the APK (debug-signed). victus.game accepts the export folder or its game/ subfolder.
gradle -p platforms/android assembleDebug -Pvictus.game=$PWD/out/mygame \
    -Pvictus.applicationId=com.example.mygame -Pvictus.appName="My Game"

# 3. Install on a device with USB debugging enabled.
adb install -r platforms/android/app/build/outputs/apk/debug/app-debug.apk
```

Gradle properties:

| Property | Default | Meaning |
| --- | --- | --- |
| `victus.game` | (required) | exported game folder containing `game.vpak` |
| `victus.applicationId` | `dev.victus.game` | Android package name |
| `victus.appName` | `VICTUS Game` | launcher label |
| `victus.versionCode` / `victus.versionName` | `1` / `0.1.0` | store version |
| `victus.orientation` | `sensorLandscape` | activity `screenOrientation` |
| `victus.sdlSource` | (download) | local SDL 3 source tree to take the Java glue from |

Release builds (`assembleRelease`) need your own signing config; add it to
`platforms/android/app/build.gradle` or an init script. Debug APKs still compile the engine
with optimizations (`RelWithDebInfo`), because an unoptimized renderer is unusable on phones.

## How it fits together

- `platforms/android` is the Gradle project. `app/build.gradle` builds the repository root
  `CMakeLists.txt` with the NDK (targets `main` and `SDL3-shared`, `arm64-v8a`, `c++_shared`).
- SDL's Java glue (`org.libsdl.app`) is not committed. The `victusSdlJava` task reads the SDL URL
  and SHA-256 that `cmake/VictusDependencies.cmake` pins, downloads that release once, verifies
  the hash and extracts `android-project/app/src/main/java`.
- `dev.victus.player.VictusActivity` extends `SDLActivity` and loads `libSDL3.so`, then
  `libmain.so`. SDL is a shared library on Android because its JNI entry points must stay
  exported; elsewhere it is linked statically.
- `apps/android/main.cpp` defines `main`, which `victus/platform/main.hpp` renames to the
  exported `SDL_main`. It installs the bundled pack and calls `victus::player::run`.
- APK assets are not files, but packs are opened by path. On first launch, and whenever the
  bundled pack changes (an app update), `player::install_bundled_pack` streams
  `assets/game.vpak` to the app's internal storage in 1 MiB chunks. It writes to a `.tmp` file
  and renames it into place, so an interrupted copy never leaves a broken pack. The installed
  pack is reused when its size and 40-byte header match: the header holds the hash of the table
  of contents, which holds the hash of every payload. The pack is stored uncompressed in the APK
  (`noCompress 'vpak'`).
- Save games go to the app's internal storage (`Platform::user_data_dir`).

## Runtime behaviour on phones

- **Quality.** The player uses `rendering.mobile_quality` (default `medium`) and
  `rendering.mobile_target_fps` (default 30, dynamic resolution) from `victus.project` instead
  of the desktop `quality` / `target_fps`. `0` turns the frame-rate target off.
- **Lifecycle.** When the app goes to the background (`AppWillEnterBackground`) the player stops
  rendering, mutes the master bus and pauses the simulation. Networked sessions keep simulating
  so the connection survives. On `AppDidEnterForeground` it resumes without a time jump. Low
  memory warnings are logged. SDL also blocks its event loop while the activity is paused.
- **Touch.** Taps arrive as left mouse clicks (SDL's touch-to-mouse events), so mouse-driven UI
  works unchanged. Scripts read every finger with `input.touch_count()` and
  `input.touch(i)`. `touch(i)` returns a vector `(x, y, pressure)` normalized to the window,
  plus the finger id.
- **Graphics.** The player first asks SDL_GPU for a Vulkan 1.1 device. The RHI relaxes optional
  Vulkan features that mobile drivers often lack. If no Vulkan device can be created, it falls
  back to the VICTUS OpenGL ES 3.0 backend (ADR-0144) and creates its window with OpenGL. GLES has
  no compute shaders, so GPU particles, GPU occlusion culling and volumetric fog are off there and
  `quality = "auto"` caps the tier at Medium. The manifest declares Vulkan optional and
  `glEsVersion` 0x30000 as required. If both backends fail, the app shows an error dialog
  instead of crashing. The GLES path is tested on Mesa llvmpipe only, not on a phone.
- **Textures.** ASTC is the mobile texture format (SDL_GPU exposes no ETC2). Cook textures with
  `"compression": "auto"` (BC7 + ASTC 4x4) or `"small"` (BC1 + ASTC 6x6). The runtime uploads the
  first variant the GPU samples and falls back to RGBA8.
- **Logs.** Engine log lines go to logcat (`adb logcat -s VICTUS`).
- **16 KB pages.** Native libraries are linked with `max-page-size=16384` and packaged
  uncompressed, as Android 15+ devices with 16 KB page kernels require.

## Limitations

- Only `arm64-v8a` is built. 32-bit ARM and x86_64 emulator images are not.
- No physical device or emulator run has been done yet. The APK build, its contents, the
  `SDL_main` export and 16 KB alignment are verified in CI, and the install and lifecycle logic is
  unit-tested on desktop.
- There are no on-screen virtual sticks or buttons yet: games read touches directly.
  Touches are not recorded in regression captures.
- No thermal or memory profiling, Android Game Mode or Performance Hint integration yet.
- The pack is copied once to internal storage, so it uses the pack size twice on disk.
