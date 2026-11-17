// SPDX-License-Identifier: Apache-2.0
// Portable program entry point. Include this header once, in the translation unit that defines `main`.
//
// On desktop it does nothing. On Android the Java activity (org.libsdl.app.SDLActivity) owns the process: it loads
// the game's shared library and calls the exported symbol `SDL_main` on its own thread, so `main` is renamed to that
// symbol and given default visibility. The game is built as a shared library (`add_library(game SHARED ...)`) named
// by the activity's getLibraries() (docs/ANDROID.md).
#pragma once

#if defined(__ANDROID__)
extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char* argv[]);
#define main SDL_main
#endif
