package com.birabittoh.dq9recompiled;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;

import androidx.browser.customtabs.CustomTabsIntent;

import org.libsdl.app.SDLActivity;

/**
 * Thin wrapper around SDL3's SDLActivity that names the native libraries to
 * load: SDL3 itself, then the runner built as a shared library (it holds
 * SDL_main). The self-updater reaches this activity over JNI for the
 * FileProvider and the package installer intent, and the frontend for the
 * second display of dual-screen handhelds (SecondDisplay).
 */
public class DQ9RecompiledActivity extends SDLActivity {

    private SecondDisplay secondDisplay;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        initDiscord(this);
        super.onCreate(savedInstanceState);
    }

    // Registered by the runner (ndsrecomp/runner/src/android_second_display.cpp)
    // before it calls startSecondDisplay(), so never called before that.
    static native void nativeSecondDisplaySurface(Surface surface, int width, int height);
    static native void nativeSecondDisplayTouch(int action, float x, float y,
                                                int width, int height);

    /** From the runner's thread: look for a second panel for the bottom screen. */
    public void startSecondDisplay() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (secondDisplay == null)
                    secondDisplay = new SecondDisplay(DQ9RecompiledActivity.this);
                secondDisplay.start();
            }
        });
    }

    /** From the runner's thread, as it shuts down. */
    public void stopSecondDisplay() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (secondDisplay != null) secondDisplay.stop();
            }
        });
    }

    @Override
    protected void onPause() {
        // Before SDL hears of it: the runner still has to let go of the
        // second display's surface.
        if (secondDisplay != null) secondDisplay.onPause();
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (secondDisplay != null) secondDisplay.onResume();
    }

    @Override
    protected void onDestroy() {
        if (secondDisplay != null) secondDisplay.stop();
        super.onDestroy();
    }

    /**
     * Opens an http(s) page in a Custom Tab (the browser's trimmed-down view
     * over the game, with "open in browser" in its menu). The native Discord
     * sign-in (ndsrecomp/runner/src/net/open_url_android.cpp) calls this over
     * JNI from its worker thread. Returns false when it could not, so the
     * caller can fall back to a plain browser.
     */
    public boolean openCustomTab(final String url) {
        final Uri uri = Uri.parse(url);
        final String scheme = uri.getScheme();
        if (!"https".equals(scheme) && !"http".equals(scheme)) return false;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    new CustomTabsIntent.Builder().build()
                        .launchUrl(DQ9RecompiledActivity.this, uri);
                } catch (Throwable t) {
                    Log.w("DQ9Recompiled", "Custom Tab failed, using the browser", t);
                    try {
                        startActivity(new Intent(Intent.ACTION_VIEW, uri));
                    } catch (Throwable t2) {
                        Log.w("DQ9Recompiled", "no browser to open the URL", t2);
                    }
                }
            }
        });
        return true;
    }

    /**
     * SDL's message box lays its own centred text and button row into the
     * dialog with no padding. A plain AlertDialog instead, with the system's
     * spacing and the game's icon: the Enter button is the positive one, the Escape button the
     * negative one, any other the neutral one. More than three buttons, or
     * two of a kind, are left to SDL.
     */
    @Override
    protected void messageboxCreateAndShow(Bundle args) {
        final int[] flags = args.getIntArray("buttonFlags");
        final int[] ids = args.getIntArray("buttonIds");
        final String[] texts = args.getStringArray("buttonTexts");
        final int[] which = new int[texts.length];
        boolean positive = false, negative = false, neutral = false;
        for (int i = 0; i < texts.length; ++i) {
            if ((flags[i] & 0x1) != 0 && !positive) {  // RETURNKEY_DEFAULT
                which[i] = DialogInterface.BUTTON_POSITIVE;
                positive = true;
            } else if ((flags[i] & 0x2) != 0 && !negative) {  // ESCAPEKEY_DEFAULT
                which[i] = DialogInterface.BUTTON_NEGATIVE;
                negative = true;
            } else if (!neutral) {
                which[i] = DialogInterface.BUTTON_NEUTRAL;
                neutral = true;
            } else {
                super.messageboxCreateAndShow(args);
                return;
            }
        }
        final AlertDialog dialog = new AlertDialog.Builder(this)
            .setTitle(args.getString("title"))
            .setIcon(getApplicationInfo().icon)  // the ROM banner's icon
            .setMessage(args.getString("message"))
            .setCancelable(false)
            .create();
        for (int i = 0; i < texts.length; ++i) {
            final int id = ids[i];
            dialog.setButton(which[i], texts[i], new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface d, int w) {
                    messageboxSelection[0] = id;
                }
            });
        }
        dialog.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface unused) {
                synchronized (messageboxSelection) {
                    messageboxSelection.notify();
                }
            }
        });
        dialog.show();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "nds_runner",
        };
    }

    /**
     * Discord's Social SDK (Rich Presence, ndsrecomp/runner/src/
     * discord_rpc_social_sdk.cpp) reaches the Discord app through this
     * activity, and has to have it before the runner creates its client.
     * The SDK is only in builds given its .aar (app/build.gradle.kts), hence
     * the reflection.
     */
    private static void initDiscord(Activity activity) {
        try {
            Class.forName("com.discord.socialsdk.DiscordSocialSdkInit")
                .getMethod("setEngineActivity", Activity.class)
                .invoke(null, activity);
        } catch (ClassNotFoundException e) {
            // A build without the SDK: no Discord activity.
        } catch (Throwable t) {
            Log.w("DQ9Recompiled", "Discord Social SDK init failed", t);
        }
    }
}
