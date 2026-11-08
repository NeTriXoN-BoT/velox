package app.velox.home;

import android.content.*;
import android.content.pm.*;
import android.graphics.drawable.Drawable;
import java.util.*;

final class Catalog {
    static final Object SETTINGS_LOCK = new Object();
    static volatile long desktopSeen;
    static void desktopSeen() { desktopSeen = android.os.SystemClock.elapsedRealtime(); }
    static boolean desktopConnected() {
        long elapsed = android.os.SystemClock.elapsedRealtime() - desktopSeen;
        return desktopSeen != 0 && elapsed >= 0 && elapsed < 15000;
    }
    static long nextRevision(Context c) {
        long previous = Math.max(prefs(c).getLong("revision", 0), prefs(c).getLong("desktopAck", 0));
        return Math.max(System.currentTimeMillis(), previous < Long.MAX_VALUE ? previous + 1 : previous);
    }
    static class App {
        String component, label, packageName;
        Drawable icon;
        boolean game, favorite;
        long updated, played;
    }
    static android.content.SharedPreferences prefs(Context c) { return c.getSharedPreferences("velox", Context.MODE_PRIVATE); }
    static ArrayList<App> apps(Context c) {
        PackageManager pm = c.getPackageManager();
        ArrayList<App> out = new ArrayList<>();
        HashSet<String> seen = new HashSet<>();
        for (ResolveInfo r : pm.queryIntentActivities(new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER), 0)) {
            if (r.activityInfo == null || !r.activityInfo.enabled)
                continue;
            App a = new App();
            a.packageName = r.activityInfo.packageName;
            a.component = new ComponentName(a.packageName, r.activityInfo.name).flattenToShortString();
            if (a.packageName.equals(c.getPackageName()) || !seen.add(a.component))
                continue;
            a.label = r.loadLabel(pm).toString();
            a.label = a.label.replaceAll("[\\p{Cntrl}]", " ").trim();
            if (a.label.codePointCount(0, a.label.length()) > 128)
                a.label = a.label.substring(0, a.label.offsetByCodePoints(0, 128));
            if (a.label.isEmpty())
                a.label = a.packageName;
            a.icon = r.loadIcon(pm);
            ApplicationInfo info = r.activityInfo.applicationInfo;
            a.game = prefs(c).getBoolean("game:" + a.component, info.category == ApplicationInfo.CATEGORY_GAME ||
                                                                    (info.flags & ApplicationInfo.FLAG_IS_GAME) != 0);
            a.favorite = prefs(c).getBoolean("fav:" + a.component, false);
            a.played = prefs(c).getLong("played:" + a.component, 0);
            try {
                a.updated = pm.getPackageInfo(a.packageName, 0).lastUpdateTime;
            } catch (Exception ignored) {
            }
            out.add(a);
        }
        out.sort((a, b) -> a.label.compareToIgnoreCase(b.label));
        return out;
    }
    static boolean launch(Context c, String component) {
        ComponentName name = ComponentName.unflattenFromString(component);
        if (name == null)
            return false;
        boolean found = false;
        for (App a : apps(c))
            if (a.component.equals(component)) {
                found = true;
                break;
            }
        if (!found)
            return false;
        try {
            c.startActivity(new Intent(Intent.ACTION_MAIN)
                                .addCategory(Intent.CATEGORY_LAUNCHER)
                                .setComponent(name)
                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED));
            prefs(c).edit().putLong("played:" + component, System.currentTimeMillis()).apply();
            return true;
        } catch (Exception e) {
            return false;
        }
    }
}
