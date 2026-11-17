# SPDX-License-Identifier: Apache-2.0
# The Android game library: libmain.so, loaded by the app's SDLActivity after libSDL3.so. It runs the
# exported game shipped in the APK as assets/game.vpak (platforms/android, docs/ANDROID.md).
add_library(main SHARED main.cpp)
target_link_libraries(main PRIVATE VICTUS::Player VICTUS::Platform VICTUS::Core SDL3::SDL3)
victus_configure_target(main)
# 16 KB pages: required for apps targeting Android 15+ devices with 16 KB page kernels. NDK r28+
# already defaults to it; stated here so older NDKs produce compatible libraries too.
target_link_options(main PRIVATE "-Wl,-z,max-page-size=16384")
