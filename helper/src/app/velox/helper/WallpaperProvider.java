package app.velox.helper;

import android.app.WallpaperManager;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.InputStream;

/**
 * Receives a wallpaper image from the PC and sets it, for the home and lock screen.
 *
 *   adb shell "content write --uri content://app.velox.helper/wallpaper < image.png"
 *   adb shell content call --uri content://app.velox.helper --method set
 *
 * Both calls need android.permission.DUMP, which adb's shell user has and normal apps can't get.
 */
public class WallpaperProvider extends ContentProvider {
    private static final String PERMISSION = "android.permission.DUMP";

    @Override
    public boolean onCreate() {
        return true;
    }

    private void enforce() {
        if (getContext().checkCallingPermission(PERMISSION) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Only adb may use this");
        }
    }

    private File image() {
        return new File(getContext().getCacheDir(), "wallpaper.img");
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        enforce();
        if (!mode.contains("w")) throw new FileNotFoundException("write only");
        return ParcelFileDescriptor.open(image(), ParcelFileDescriptor.MODE_WRITE_ONLY
                | ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_TRUNCATE);
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        enforce();
        Bundle out = new Bundle();
        if (!"set".equals(method)) {
            out.putBoolean("ok", false);
            return out;
        }
        File file = image();
        try (InputStream in = new FileInputStream(file)) {
            WallpaperManager.getInstance(getContext())
                    .setStream(in, null, true, WallpaperManager.FLAG_SYSTEM | WallpaperManager.FLAG_LOCK);
            out.putBoolean("ok", true);
        } catch (Exception e) {
            out.putBoolean("ok", false);
            out.putString("error", e.toString());
        } finally {
            file.delete();
        }
        return out;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        return 0;
    }
}
