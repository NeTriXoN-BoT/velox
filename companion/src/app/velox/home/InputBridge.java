package app.velox.home;

import android.graphics.Point;
import android.app.ActivityManager;
import android.content.ComponentName;
import android.content.ClipData;
import android.util.Base64;
import android.os.*;
import android.view.*;
import java.io.*;
import java.lang.reflect.*;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.*;

/**
 * Runs as adb shell through app_process, never as the unprivileged Home app.
 * A private stdin pipe carries normalized multi-touch frames; EOF cancels all contacts.
 * No server, network permission, accessibility service or root is required.
 */
public final class InputBridge {
    final LinkedHashMap<Integer, float[]> contacts = new LinkedHashMap<>();
    final LinkedHashMap<Integer, Long> keys = new LinkedHashMap<>();
    final HashMap<Integer, Integer> repeats = new HashMap<>();
    Object manager, displayManager;
    Method inject, getDisplay;
    long down;
    int width = 1080, height = 2400;
    volatile boolean observing;
    Thread observer;
    ByteArrayOutputStream textBytes;
    String textId;
    int textLength;
    boolean preserveClipboard;
    ShellClipboard clipboard;
    InputBridge() throws Exception {
        Class<?> im = Class.forName("android.hardware.input.InputManagerGlobal");
        manager = im.getMethod("getInstance").invoke(null);
        inject = im.getMethod("injectInputEvent", InputEvent.class, int.class);
        Class<?> dm = Class.forName("android.hardware.display.DisplayManagerGlobal");
        displayManager = dm.getMethod("getInstance").invoke(null);
        getDisplay = dm.getMethod("getRealDisplay", int.class);
    }
    void event(int action) throws Exception {
        int n = contacts.size();
        if (n == 0)
            return;
        Display display = (Display)getDisplay.invoke(displayManager, 0);
        Point size = new Point();
        display.getRealSize(size);
        width = size.x;
        height = size.y;
        MotionEvent.PointerProperties[] pp = new MotionEvent.PointerProperties[n];
        MotionEvent.PointerCoords[] pc = new MotionEvent.PointerCoords[n];
        int i = 0;
        for (Map.Entry<Integer, float[]> e : contacts.entrySet()) {
            pp[i] = new MotionEvent.PointerProperties();
            pp[i].id = e.getKey();
            pp[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            pc[i] = new MotionEvent.PointerCoords();
            pc[i].x = Math.max(0, Math.min(width - 1, e.getValue()[0] * (width - 1) / 10000f));
            pc[i].y = Math.max(0, Math.min(height - 1, e.getValue()[1] * (height - 1) / 10000f));
            pc[i].pressure = 1;
            pc[i].size = 1;
            i++;
        }
        MotionEvent ev =
            MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, n, pp, pc, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        try {
            if (!Boolean.TRUE.equals(inject.invoke(manager, ev, 0)))
                throw new IOException("Input injection was refused");
        } finally {
            ev.recycle();
        }
    }
    void cancel() throws Exception {
        if (!contacts.isEmpty())
            event(MotionEvent.ACTION_CANCEL);
        contacts.clear();
        for (int code : new ArrayList<>(keys.keySet()))
            key(code, KeyEvent.ACTION_UP, 0, true);
    }
    void key(int code, int action, int modifiers) throws Exception {
        key(code, action, modifiers, false);
    }
    void key(int code, int action, int modifiers, boolean cancelled) throws Exception {
        if (action == KeyEvent.ACTION_UP && !keys.containsKey(code))
            return;
        long time = SystemClock.uptimeMillis();
        long began = keys.containsKey(code) ? keys.get(code) : time;
        int repeat = action == KeyEvent.ACTION_DOWN && keys.containsKey(code) ? repeats.getOrDefault(code, 0) + 1 : 0;
        if (action == KeyEvent.ACTION_DOWN)
            keys.put(code, began);
        repeats.put(code, repeat);
        KeyEvent event = new KeyEvent(began, time, action, code, repeat, modifiers, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
                                      cancelled ? KeyEvent.FLAG_CANCELED : 0,
                                      InputDevice.SOURCE_KEYBOARD);
        // Keyboard barriers must include application dispatch, so Copy cannot
        // read the clipboard before the preceding key reaches the focused app.
        // This runs on the pipe worker; touch motion remains asynchronous.
        if (!Boolean.TRUE.equals(inject.invoke(manager, event, 2)))
            throw new IOException("Key injection was refused");
        if (action == KeyEvent.ACTION_UP) {
            keys.remove(code);
            repeats.remove(code);
        }
    }
    void observeDisplay() {
        observing = true;
        observer = new Thread(() -> {
            String previous = "";
            Object tasks = null;
            Method focused = null;
            try {
                tasks = Class.forName("android.app.ActivityTaskManager").getMethod("getService").invoke(null);
                focused = Class.forName("android.app.IActivityTaskManager").getMethod("getTasks", int.class, boolean.class, boolean.class, int.class);
            } catch (Exception ignored) {
            }
            while (observing) {
                try {
                    Display display = (Display)getDisplay.invoke(displayManager, 0);
                    Point size = new Point();
                    display.getRealSize(size);
                    String app = "-";
                    if (focused != null) {
                        try {
                            List<?> list = (List<?>)focused.invoke(tasks, 1, false, false, 0);
                            ActivityManager.RunningTaskInfo info = list == null || list.isEmpty() ? null : (ActivityManager.RunningTaskInfo)list.get(0);
                            ComponentName component = info == null ? null : info.topActivity;
                            if (component != null)
                                app = component.getPackageName();
                        } catch (Exception ignored) {
                        }
                    }
                    String value = "STATE " + size.x + " " + size.y + " " + display.getRotation() + " " + app;
                    if (!value.equals(previous)) {
                        System.out.println(value);
                        System.out.flush();
                        previous = value;
                    }
                    Thread.sleep(100);
                } catch (Exception ignored) {
                    break;
                }
            }
        }, "Velox display observer");
        observer.setDaemon(true);
        observer.start();
    }
    void command(String line) throws Exception {
        String[] f = line.trim().split(" +");
        if (f.length == 0)
            return;
        if (f[0].equals("C")) {
            if (textId != null) System.out.println("TEXT " + textId + " CANCELLED");
            textBytes = null; textId = null;
            System.out.flush();
            cancel();
            return;
        }
        if (f[0].equals("B")) {
            if (f.length != 4 || !f[1].matches("[a-f0-9]{16}") || !f[2].matches("[01]") || !f[3].matches("[0-9]{1,7}"))
                throw new IOException("Invalid text request");
            int length = Integer.parseInt(f[3]);
            if (length > 1048576 || textId != null) throw new IOException("Text request too large or overlapping");
            textId = f[1]; preserveClipboard = f[2].equals("1"); textLength = length;
            textBytes = new ByteArrayOutputStream(Math.min(length, 8192));
            return;
        }
        if (f[0].equals("T")) {
            if (f.length != 2 || textId == null || f[1].length() > 3072 || f[1].length() % 4 != 0 ||
                !f[1].matches("[A-Za-z0-9+/]+={0,2}")) throw new IOException("Invalid text chunk");
            byte[] block = Base64.decode(f[1], Base64.NO_WRAP);
            if (textBytes.size() + block.length > textLength) throw new IOException("Text request exceeded its size");
            textBytes.write(block);
            return;
        }
        if (f[0].equals("E")) {
            if (f.length != 2 || textId == null || !textId.equals(f[1]) || textBytes.size() != textLength)
                throw new IOException("Incomplete text request");
            String id = textId;
            try {
                String text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(textBytes.toByteArray())).toString();
                if (text.indexOf(0) >= 0) throw new IOException("NUL text is unsupported");
                if (clipboard == null) clipboard = new ShellClipboard();
                ClipData previous = preserveClipboard ? clipboard.read() : null;
                String marker = "velox-" + UUID.randomUUID();
                try {
                    clipboard.write(ClipData.newPlainText(marker, text));
                    try {
                        key(KeyEvent.KEYCODE_PASTE, KeyEvent.ACTION_DOWN, 0);
                        key(KeyEvent.KEYCODE_PASTE, KeyEvent.ACTION_UP, 0);
                    } finally {
                        if (keys.containsKey(KeyEvent.KEYCODE_PASTE)) key(KeyEvent.KEYCODE_PASTE, KeyEvent.ACTION_UP, 0, true);
                    }
                } finally {
                    if (preserveClipboard && clipboard.owns(marker)) clipboard.write(previous);
                }
                System.out.println("TEXT " + id + " OK");
            } catch (Exception failed) {
                System.out.println("TEXT " + id + " ERROR");
            } finally {
                textBytes = null; textId = null; System.out.flush();
            }
            return;
        }
        if (f[0].equals("G")) {
            if (f.length != 2 || !f[1].matches("[a-f0-9]{16}")) throw new IOException("Invalid clipboard request");
            try {
                if (clipboard == null) clipboard = new ShellClipboard();
                String text = clipboard.text();
                if (text == null) System.out.println("CLIP " + f[1] + " NONE");
                else {
                    byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
                    if (bytes.length > 1048576) throw new IOException("Clipboard too large");
                    System.out.println("CLIP " + f[1] + " BEGIN " + bytes.length);
                    for (int offset = 0; offset < bytes.length; offset += 2304)
                        System.out.println("CLIP " + f[1] + " DATA " + Base64.encodeToString(bytes, offset, Math.min(2304, bytes.length - offset), Base64.NO_WRAP));
                    System.out.println("CLIP " + f[1] + " END");
                }
            } catch (Exception failed) { System.out.println("CLIP " + f[1] + " ERROR"); }
            System.out.flush();
            return;
        }
        if (f[0].equals("P")) {
            System.out.println("PONG");
            System.out.flush();
            return;
        }
        if (f[0].equals("K")) {
            if (f.length != 4)
                throw new IOException("Invalid key event");
            int code = Integer.parseInt(f[1]), action = Integer.parseInt(f[2]), modifiers = Integer.parseInt(f[3]);
            if (code < 0 || code > 288 || action < 0 || action > 1 || (modifiers & ~0x111003) != 0)
                throw new IOException("Invalid key event");
            key(code, action, modifiers);
            return;
        }
        if (f.length < 2)
            return;
        int id = Integer.parseInt(f[1]);
        if (id < 0 || id > 15)
            throw new IOException("Invalid contact");
        if (f[0].equals("U")) {
            if (!contacts.containsKey(id))
                return;
            int index = new ArrayList<>(contacts.keySet()).indexOf(id);
            event(contacts.size() == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP | (index << 8));
            contacts.remove(id);
            return;
        }
        if (f.length != 4)
            return;
        float x = Float.parseFloat(f[2]), y = Float.parseFloat(f[3]);
        if (!Float.isFinite(x) || !Float.isFinite(y))
            throw new IOException("Invalid coordinate");
        if (f[0].equals("D")) {
            if (contacts.containsKey(id))
                return;
            if (contacts.isEmpty())
                down = SystemClock.uptimeMillis();
            contacts.put(id, new float[] {x, y});
            event(contacts.size() == 1 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_POINTER_DOWN | ((contacts.size() - 1) << 8));
        } else if (f[0].equals("M") && contacts.containsKey(id)) {
            contacts.put(id, new float[] {x, y});
            event(MotionEvent.ACTION_MOVE);
        }
    }
    public static void main(String[] args) {
        InputBridge bridge = null;
        try {
            int uid = android.os.Process.myUid();
            if (uid != 2000 && uid != 0) throw new SecurityException("Only adb shell may use game input");
            Looper.prepare();
            bridge = new InputBridge();
            System.out.println("VELOX_INPUT_READY");
            System.out.flush();
            if (args.length == 1 && args[0].equals("--display"))
                bridge.observeDisplay();
            BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
            for (String line; (line = readCommand(in)) != null;) {
                if (line.equals("Q"))
                    break;
                bridge.command(line);
            }
        } catch (Throwable e) {
            System.out.println("VELOX_INPUT_ERROR " + e);
            System.out.flush();
        } finally {
            if (bridge != null)
                try {
                    bridge.observing = false;
                    bridge.cancel();
                } catch (Exception ignored) {
                }
        }
    }
    static String readCommand(Reader source) throws IOException {
        StringBuilder line = new StringBuilder(128);
        for (int value; (value = source.read()) != -1;) {
            if (value == '\n') return line.toString();
            if (line.length() >= 4096) throw new IOException("Input command too long");
            line.append((char)value);
        }
        return line.length() == 0 ? null : line.toString();
    }
}
