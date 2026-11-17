// SPDX-License-Identifier: Apache-2.0
package dev.victus.player;

import org.libsdl.app.SDLActivity;

/**
 * The game's activity. SDLActivity owns the window, input, audio and lifecycle; it loads the
 * native libraries named here (libSDL3.so, then the engine and game in libmain.so) and runs
 * SDL_main on its own thread.
 */
public class VictusActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
