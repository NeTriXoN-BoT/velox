package app.velox.home;

import android.graphics.Point;
import android.os.*;
import android.view.*;
import java.io.*;
import java.lang.reflect.*;
import java.util.*;

/**
 * Runs as adb shell through app_process, never as the unprivileged Home app.
 * A private stdin pipe carries normalized multi-touch frames; EOF cancels all contacts.
 * No server, network permission, accessibility service or root is required.
 */
public final class InputBridge {
    final LinkedHashMap<Integer, float[]> contacts = new LinkedHashMap<>();
    Object manager, displayManager;
    Method inject, getDisplay;
    long down;
    int width = 1080, height = 2400;
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
    }
    void command(String line) throws Exception {
        String[] f = line.trim().split(" +");
        if (f.length == 0)
            return;
        if (f[0].equals("C")) {
            cancel();
            return;
        }
        if (f[0].equals("P")) {
            System.out.println("PONG");
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
            Looper.prepare();
            bridge = new InputBridge();
            System.out.println("VELOX_INPUT_READY");
            System.out.flush();
            BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
            for (String line; (line = in.readLine()) != null;) {
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
                    bridge.cancel();
                } catch (Exception ignored) {
                }
        }
    }
}
