package app.velox.home;

import android.os.IBinder;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;
import java.io.*;
import java.lang.reflect.*;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import org.json.JSONObject;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.XmlResourceParser;
import org.xmlpull.v1.XmlPullParser;

/** Native Android game pacing, called only by the verified desktop shell bridge. */
public final class FramePolicy {
    final Object service;
    final Class<?> api, infoClass, configClass, builderClass;
    final int user;
    final File folder, journal;

    static final class State {
        int mode, fps, activeFps;
        float scale = 1, activeScale = 1;
        boolean supported;
    }

    FramePolicy() throws Exception {
        if (Os.getuid() != 2000 && Os.getuid() != 0)
            throw new SecurityException("Shell required");
        IBinder binder = (IBinder)Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "game");
        if (binder == null)
            throw new IOException("Game service unavailable");
        api = Class.forName("android.app.IGameManagerService");
        service = Class.forName("android.app.IGameManagerService$Stub").getMethod("asInterface", IBinder.class).invoke(null, binder);
        infoClass = Class.forName("android.app.GameModeInfo");
        configClass = Class.forName("android.app.GameModeConfiguration");
        builderClass = Class.forName("android.app.GameModeConfiguration$Builder");
        user = (Integer)Class.forName("android.app.ActivityManager").getMethod("getCurrentUser").invoke(null);
        if (user < 0 || user > 65535)
            throw new IOException("Invalid Android user");
        folder = new File("/data/local/tmp/velox-frame-policy");
        try { Os.mkdir(folder.getPath(), 0700); } catch (android.system.ErrnoException error) {
            if (error.errno != OsConstants.EEXIST)
                throw error;
        }
        StructStat stat = Os.lstat(folder.getPath());
        if (!OsConstants.S_ISDIR(stat.st_mode) || stat.st_uid != Os.getuid())
            throw new IOException("Private policy folder required");
        Os.chmod(folder.getPath(), 0700);
        journal = new File(folder, "state-" + user + ".json");
    }

    static boolean validPackage(String value) {
        return value.length() <= 199 && value.matches("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z0-9_]+)+");
    }

    static float scale(Object configuration, Class<?> type) throws Exception {
        if (configuration == null)
            return 1;
        float value = (Float)type.getMethod("getScalingFactor").invoke(configuration);
        return Float.isFinite(value) && value >= .1f && value <= 1 ? value : 1;
    }

    State read(String name) throws Exception {
        State state = new State();
        state.mode = (Integer)api.getMethod("getGameMode", String.class, int.class).invoke(service, name, user);
        Object info = api.getMethod("getGameModeInfo", String.class, int.class).invoke(service, name, user);
        if (info == null || state.mode < 1 || state.mode > 4)
            return state;
        state.supported = (Boolean)infoClass.getMethod("isFpsOverrideAllowed").invoke(info);
        if (!state.supported)
            state.supported = permitsFrameOverride(name);
        Object custom = infoClass.getMethod("getGameModeConfiguration", int.class).invoke(info, 4);
        Object active = infoClass.getMethod("getGameModeConfiguration", int.class).invoke(info, state.mode);
        state.fps = custom == null ? 0 : (Integer)configClass.getMethod("getFpsOverride").invoke(custom);
        state.activeFps = active == null ? 0 : (Integer)configClass.getMethod("getFpsOverride").invoke(active);
        state.scale = scale(custom, configClass);
        state.activeScale = scale(active, configClass);
        return state;
    }

    boolean permitsFrameOverride(String name) throws Exception {
        // With no OEM configuration GameModeInfo leaves this flag false, even
        // though Android's default permits interventions. Respect the game's
        // explicit opt-out in its own manifest rather than overriding it.
        if (android.os.Looper.myLooper() == null)
            android.os.Looper.prepare();
        Class<?> threadClass = Class.forName("android.app.ActivityThread");
        Object thread = threadClass.getMethod("systemMain").invoke(null);
        Context context = (Context)threadClass.getMethod("getSystemContext").invoke(thread);
        PackageManager packages = context.getPackageManager();
        ApplicationInfo application = packages.getApplicationInfo(name, PackageManager.GET_META_DATA);
        try (XmlResourceParser parser = application.loadXmlMetaData(packages, "android.game_mode_config")) {
            if (parser == null)
                return true;
            int event;
            while ((event = parser.next()) != XmlPullParser.END_DOCUMENT) {
                if (event == XmlPullParser.START_TAG)
                    return parser.getName().equals("game-mode-config") &&
                           parser.getAttributeBooleanValue("http://schemas.android.com/apk/res/android", "allowGameFpsOverride", true);
            }
        }
        return false;
    }

    void write(String name, int mode, int fps, float scaling) throws Exception {
        Object builder = builderClass.getConstructor().newInstance();
        builderClass.getMethod("setFpsOverride", int.class).invoke(builder, fps);
        builderClass.getMethod("setScalingFactor", float.class).invoke(builder, scaling);
        Object configuration = builderClass.getMethod("build").invoke(builder);
        api.getMethod("updateCustomGameModeConfiguration", String.class, configClass, int.class).invoke(service, name, configuration, user);
        api.getMethod("setGameMode", String.class, int.class, int.class).invoke(service, name, mode, user);
    }

    JSONObject load() throws Exception {
        if (!journal.exists())
            return null;
        try (FileInputStream input = new FileInputStream(Os.open(journal.getPath(), OsConstants.O_RDONLY | OsConstants.O_NOFOLLOW | OsConstants.O_NONBLOCK, 0))) {
            StructStat stat = Os.fstat(input.getFD());
            if (!OsConstants.S_ISREG(stat.st_mode) || stat.st_uid != Os.getuid() || stat.st_nlink != 1 || stat.st_size > 2048)
                throw new IOException("Invalid policy journal");
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] block = new byte[2049];
            int count;
            while ((count = input.read(block)) != -1) {
                bytes.write(block, 0, count);
                if (bytes.size() > 2048)
                    throw new IOException("Policy journal too large");
            }
            JSONObject value = new JSONObject(bytes.toString("UTF-8"));
            if (value.getInt("version") != 1 || !validPackage(value.getString("package")) ||
                value.getInt("mode") < 1 || value.getInt("mode") > 4 ||
                value.getInt("fps") < 0 || value.getInt("fps") > 1000 ||
                value.getInt("activeFps") < 0 || value.getInt("activeFps") > 1000 ||
                (value.has("previousApplied") && (value.getInt("previousApplied") < 0 || value.getInt("previousApplied") > 1000)) ||
                value.getInt("applied") < 1 || value.getInt("applied") > 144)
                throw new IOException("Invalid policy values");
            for (String key : new String[] {"scale", "activeScale"}) {
                double number = value.getDouble(key);
                if (!Double.isFinite(number) || number < .1 || number > 1)
                    throw new IOException("Invalid policy scaling");
            }
            return value;
        }
    }

    void save(JSONObject value) throws Exception {
        File temporary = new File(folder, ".tmp-" + UUID.randomUUID());
        try {
            try (FileOutputStream output = new FileOutputStream(Os.open(temporary.getPath(),
                    OsConstants.O_WRONLY | OsConstants.O_CREAT | OsConstants.O_EXCL | OsConstants.O_NOFOLLOW, 0600))) {
                output.write(value.toString().getBytes(StandardCharsets.UTF_8));
                output.flush(); output.getFD().sync();
            }
            Os.rename(temporary.getPath(), journal.getPath());
            java.io.FileDescriptor directory = Os.open(folder.getPath(), OsConstants.O_RDONLY | OsConstants.O_NOFOLLOW, 0);
            try {
                if (!OsConstants.S_ISDIR(Os.fstat(directory).st_mode))
                    throw new IOException("Policy folder changed");
                Os.fsync(directory);
            } finally { Os.close(directory); }
        } finally { temporary.delete(); }
    }

    boolean matches(State state, JSONObject saved) throws Exception {
        return (state.mode == 4 || state.mode == saved.getInt("mode")) &&
               (state.fps == saved.getInt("applied") || state.fps == saved.optInt("previousApplied", -1)) &&
               Math.abs(state.scale - saved.getDouble("activeScale")) < .0001;
    }

    void restore(JSONObject saved) throws Exception {
        if (saved == null)
            return;
        String name = saved.getString("package");
        State state = read(name);
        if (matches(state, saved))
            write(name, saved.getInt("mode"), saved.getInt("fps"), (float)saved.getDouble("scale"));
        // Explicit Android changes win over a stale desktop policy.
        if (!journal.delete() && journal.exists())
            throw new IOException("Policy journal could not be cleared");
    }

    void apply(String name, int rate) throws Exception {
        JSONObject saved = load();
        if (saved != null && (rate == 0 || !saved.getString("package").equals(name))) {
            restore(saved); saved = null;
        }
        if (rate == 0) {
            System.out.println("VELOX_FRAME_RESTORED"); return;
        }
        State state = read(name);
        if (!state.supported) {
            System.out.println("VELOX_FRAME_UNSUPPORTED"); return;
        }
        if (saved != null && !matches(state, saved)) {
            restore(saved);
            System.out.println("VELOX_FRAME_EXTERNAL"); return;
        }
        if (saved == null) {
            saved = new JSONObject().put("version", 1).put("package", name).put("mode", state.mode)
                .put("fps", state.fps).put("scale", state.scale).put("activeFps", state.activeFps).put("activeScale", state.activeScale);
        }
        int existing = saved.getInt("activeFps");
        int wanted = existing > 0 ? Math.min(rate, existing) : rate;
        if (state.mode == 4 && state.fps == wanted && Math.abs(state.scale - saved.getDouble("activeScale")) < .0001) {
            System.out.println("VELOX_FRAME_APPLIED " + wanted); return;
        }
        saved.put("previousApplied", state.fps).put("applied", wanted);
        save(saved);
        write(name, 4, wanted, (float)saved.getDouble("activeScale"));
        System.out.println("VELOX_FRAME_APPLIED " + wanted);
    }

    public static void main(String[] arguments) {
        try {
            FramePolicy policy = new FramePolicy();
            File lockFile = new File(policy.folder, "lock-" + policy.user);
            try (FileOutputStream output = new FileOutputStream(Os.open(lockFile.getPath(),
                    OsConstants.O_WRONLY | OsConstants.O_CREAT | OsConstants.O_NOFOLLOW | OsConstants.O_NONBLOCK, 0600));
                    FileLock lock = output.getChannel().tryLock()) {
                StructStat stat = Os.fstat(output.getFD());
                if (lock == null || !OsConstants.S_ISREG(stat.st_mode) || stat.st_uid != Os.getuid() || stat.st_nlink != 1)
                    throw new IOException("Private policy lock required");
                if (arguments.length == 1 && arguments[0].equals("restore")) {
                    policy.restore(policy.load()); System.out.println("VELOX_FRAME_RESTORED");
                } else if (arguments.length == 2 && arguments[0].equals("read") && validPackage(arguments[1])) {
                    State state = policy.read(arguments[1]);
                    System.out.println("VELOX_FRAME_STATE " + state.mode + " " + state.fps + " " + state.scale + " " + state.supported);
                } else if (arguments.length == 3 && arguments[0].equals("apply") && validPackage(arguments[1])) {
                    int rate = Integer.parseInt(arguments[2]);
                    if (rate != 0 && rate != 15 && rate != 30 && rate != 60 && rate != 90 && rate != 120 && rate != 144)
                        throw new IOException("Invalid frame rate");
                    policy.apply(arguments[1], rate);
                } else {
                    throw new IOException("Invalid policy command");
                }
            }
        } catch (Throwable error) {
            System.out.println("VELOX_FRAME_ERROR " + error.getClass().getSimpleName());
            System.exit(1);
        }
    }
}
