package app.velox.home;

import android.content.ClipData;
import android.os.IBinder;
import java.lang.reflect.Method;

/** Android 15 shell clipboard access; no application or background clipboard listener. */
final class ShellClipboard {
    private final Object service;
    private final Method read, write, clear;
    ShellClipboard() throws Exception {
        int uid = android.os.Process.myUid();
        if (uid != 2000 && uid != 0) throw new SecurityException("Only adb shell may use the clipboard bridge");
        IBinder binder = (IBinder)Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "clipboard");
        Class<?> api = Class.forName("android.content.IClipboard");
        service = Class.forName("android.content.IClipboard$Stub").getMethod("asInterface", IBinder.class).invoke(null, binder);
        if (service == null) throw new IllegalStateException("Clipboard service unavailable");
        read = api.getMethod("getPrimaryClip", String.class, String.class, int.class, int.class);
        write = api.getMethod("setPrimaryClip", ClipData.class, String.class, String.class, int.class, int.class);
        clear = api.getMethod("clearPrimaryClip", String.class, String.class, int.class, int.class);
    }
    ClipData read() throws Exception { return (ClipData)read.invoke(service, "com.android.shell", null, 0, 0); }
    void write(ClipData value) throws Exception {
        if (value == null) clear.invoke(service, "com.android.shell", null, 0, 0);
        else write.invoke(service, value, "com.android.shell", null, 0, 0);
    }
    String text() throws Exception {
        ClipData value = read();
        if (value == null || value.getItemCount() == 0 || value.getItemAt(0).getText() == null) return null;
        return value.getItemAt(0).getText().toString();
    }
    boolean owns(String marker) throws Exception {
        ClipData value = read();
        CharSequence label = value == null || value.getDescription() == null ? null : value.getDescription().getLabel();
        return label != null && marker.contentEquals(label);
    }
}
