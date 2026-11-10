package org.bergamota;

import android.Manifest;
import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.provider.Settings;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

/** A port's app: SDL's activity with libmain.so (SDL linked into it), and
 *  what the launcher asks of Android (launcher/android.cpp). */
public class PortActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    /** Android's screen where the player lets the app read all files: the
     *  game is read by a path, here and by the programs the launcher starts,
     *  which shared storage refuses otherwise. */
    public static void askAllFiles() {
        Activity activity = (Activity) SDL.getContext();
        if (Build.VERSION.SDK_INT < 30) {
            activity.requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE}, 1);
            return;
        }
        try {
            activity.startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                              Uri.parse("package:" + activity.getPackageName())));
        } catch (Exception e) {
            activity.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    /** A document the player picked (content://...) opened for reading, the
     *  permission to it kept for the next runs; its descriptor, -1 when it
     *  cannot be opened. */
    public static int openDocument(String uri) {
        try {
            Uri u = Uri.parse(uri);
            ContentResolver resolver = SDL.getContext().getContentResolver();
            try {
                resolver.takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException e) {
                // not one that can be kept: open for this run only
            }
            ParcelFileDescriptor file = resolver.openFileDescriptor(u, "r");
            return file == null ? -1 : file.detachFd();
        } catch (Exception e) {
            android.util.Log.e("Bergamota", "openDocument " + uri, e);
            return -1;
        }
    }
}
