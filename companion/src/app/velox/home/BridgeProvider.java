package app.velox.home;

import android.content.*;
import android.database.Cursor;
import android.graphics.*;
import android.net.Uri;
import android.os.*;
import android.util.Base64;
import java.io.*;
import java.nio.charset.StandardCharsets;

/** Only the authorized adb shell can read the app catalog or change desktop preferences. */
public class BridgeProvider extends ContentProvider {
    public boolean onCreate() { return true; }
    void enforce() { getContext().enforceCallingPermission("android.permission.DUMP", "Only adb may use the desktop bridge"); }
    public Bundle call(String method, String arg, Bundle extras) {
        enforce();
        Bundle b = new Bundle();
        b.putBoolean("ok", false);
        SharedPreferences p = Catalog.prefs(getContext());
        if ("gamepad".equals(method)) {
            boolean ready = false;
            for (int id : android.view.InputDevice.getDeviceIds()) {
                android.view.InputDevice device = android.view.InputDevice.getDevice(id);
                if (device != null && device.getName().equals("Velox Controller") &&
                    device.supportsSource(android.view.InputDevice.SOURCE_GAMEPAD)) ready = true;
            }
            b.putBoolean("ready", ready);
            b.putBoolean("ok", true);
        }
        if ("hello".equals(method)) {
            b.putString("build", Build.ID);
            b.putBoolean("ok", true);
            p.edit().putLong("desktopSeen", System.currentTimeMillis()).apply();
        }
        if ("sync".equals(method) && arg != null && arg.matches("[012],[01],[0-9]{1,19},[01]")) {
            synchronized (Catalog.SETTINGS_LOCK) {
                String[] f = arg.split(",");
                long revision;
                try { revision = Long.parseLong(f[2]); }
                catch (NumberFormatException invalidRevision) { return b; }
                SharedPreferences.Editor edit =
                    p.edit().putLong("desktopSeen", System.currentTimeMillis()).putLong("desktopAck", revision);
                // Don't overwrite an Android edit made after the desktop read its state.
                if (p.getLong("revision", 0) <= revision)
                    edit.putInt("navigation", Integer.parseInt(f[0])).putBoolean("rotate", f[1].equals("1")).putInt("language", Integer.parseInt(f[3]));
                edit.apply();
                b.putBoolean("ok", true);
            }
        }
        if ("played".equals(method) && arg != null) {
            for (Catalog.App app : Catalog.apps(getContext()))
                if (app.component.equals(arg)) {
                    p.edit().putLong("played:" + arg, System.currentTimeMillis()).apply();
                    b.putBoolean("ok", true);
                    break;
                }
        }
        if (("favorite".equals(method) || "game".equals(method)) && arg != null) {
            int at = arg.lastIndexOf('|');
            if (at > 0) {
                String name = arg.substring(0, at);
                for (Catalog.App a : Catalog.apps(getContext()))
                    if (a.component.equals(name)) {
                        p.edit().putBoolean(("game".equals(method) ? "game:" : "fav:") + name, arg.substring(at + 1).equals("1")).apply();
                        b.putBoolean("ok", true);
                        break;
                    }
            }
        }
        return b;
    }
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        enforce();
        if (!"r".equals(mode))
            throw new FileNotFoundException("Read only");
        return openPipeHelper(uri, "application/octet-stream", null, uri, (out, u, m, options, request) -> {
            try (OutputStream stream = new ParcelFileDescriptor.AutoCloseOutputStream(out)) {
                if ("/apps".equals(request.getPath())) {
                    for (Catalog.App a : Catalog.apps(getContext())) {
                        String label = Base64.encodeToString(a.label.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP);
                        String line = a.component + "\t" + label + "\t" + (a.game ? 1 : 0) + "\t" + (a.favorite ? 1 : 0) + "\t" +
                                      a.updated + "\t" + a.played + "\n";
                        stream.write(line.getBytes(StandardCharsets.UTF_8));
                    }
                } else if ("/state".equals(request.getPath())) {
                    synchronized (Catalog.SETTINGS_LOCK) {
                        SharedPreferences p = Catalog.prefs(getContext());
                        String line = p.getLong("revision", 0) + "\t" + p.getInt("navigation", 0) + "\t" +
                                      (p.getBoolean("rotate", true) ? 1 : 0) + "\t" + p.getInt("language", 0) + "\n";
                        stream.write(line.getBytes(StandardCharsets.US_ASCII));
                    }
                } else if ("/icon".equals(request.getPath())) {
                    String component = request.getQueryParameter("component");
                    for (Catalog.App a : Catalog.apps(getContext()))
                        if (a.component.equals(component)) {
                            Bitmap image = Bitmap.createBitmap(128, 128, Bitmap.Config.ARGB_8888);
                            Canvas canvas = new Canvas(image);
                            a.icon.setBounds(0, 0, 128, 128);
                            a.icon.draw(canvas);
                            image.compress(Bitmap.CompressFormat.PNG, 100, stream);
                            image.recycle();
                            break;
                        }
                } else
                    throw new IOException("Unknown bridge resource");
            } catch (Exception e) {
                android.util.Log.w("Velox", "Bridge read failed", e);
            }
        });
    }
    public Cursor query(Uri u, String[] p, String s, String[] a, String o) { return null; }
    public String getType(Uri u) { return "application/octet-stream"; }
    public Uri insert(Uri u, ContentValues v) { throw new UnsupportedOperationException(); }
    public int delete(Uri u, String s, String[] a) { throw new UnsupportedOperationException(); }
    public int update(Uri u, ContentValues v, String s, String[] a) { throw new UnsupportedOperationException(); }
}
