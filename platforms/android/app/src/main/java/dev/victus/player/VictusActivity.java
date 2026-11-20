// SPDX-License-Identifier: Apache-2.0
package dev.victus.player;

import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

/**
 * The game's activity. SDLActivity owns the window, input, audio and lifecycle; it loads the
 * native libraries named here (libSDL3.so, then the engine and game in libmain.so) and runs
 * SDL_main on its own thread.
 *
 * Orientation (ADR-0150): the manifest's android:screenOrientation (the Gradle property
 * victus.orientation, default sensorLandscape) is authoritative. SDL would otherwise replace it
 * when the window is created: a resizable window without SDL_HINT_ORIENTATIONS requests
 * SCREEN_ORIENTATION_FULL_USER, which follows the phone's rotation lock and so started the game
 * in portrait on phones held or locked upright.
 */
public class VictusActivity extends SDLActivity {
    private int manifestOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        try {
            ActivityInfo info = getPackageManager().getActivityInfo(getComponentName(), 0);
            manifestOrientation = info.screenOrientation;
        } catch (PackageManager.NameNotFoundException e) {
            manifestOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
        }
        super.onCreate(savedInstanceState);
        if (manifestOrientation != ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED)
            setRequestedOrientation(manifestOrientation);
    }

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        if (manifestOrientation != ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
            setRequestedOrientation(manifestOrientation); // keep the manifest's choice
            return;
        }
        super.setOrientationBis(w, h, resizable, hint);
    }
}
