package org.bergamota;

import android.content.ContentResolver;
import android.content.Intent;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

/** A port's app: SDL's activity with libmain.so (SDL linked into it), and
 *  what the launcher asks of Android (launcher/android.cpp). */
public class PortActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
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
            return -1;
        }
    }
}
