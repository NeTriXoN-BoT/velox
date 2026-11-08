package app.velox.home;

import android.app.*;
import android.content.*;
import android.content.res.Configuration;
import android.os.*;
import android.provider.Settings;
import android.widget.*;

public class SettingsActivity extends Activity {
    LinearLayout root;
    android.content.SharedPreferences prefs;
    final android.os.Handler languageHandler=new android.os.Handler(android.os.Looper.getMainLooper());
    final Runnable languageRefresh=()->build();
    final android.content.SharedPreferences.OnSharedPreferenceChangeListener changes=(p,key)->{
        if("language".equals(key)) { languageHandler.removeCallbacks(languageRefresh); languageHandler.post(languageRefresh); }
    };
    protected void onResume(){super.onResume();prefs.registerOnSharedPreferenceChangeListener(changes);build();}
    protected void onPause(){super.onPause();prefs.unregisterOnSharedPreferenceChangeListener(changes);languageHandler.removeCallbacks(languageRefresh);}
    public void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = Catalog.prefs(this);
        build();
    }
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        build();
    }
    void section(String name, String description) {
        Design.gap(root, 20);
        root.addView(Design.text(this, name, 18, Design.TEXT, true));
        Design.gap(root, 5);
        root.addView(Design.text(this, description, 13, Design.DIM, false));
        Design.gap(root, 12);
    }
    void toggle(String title, String key, boolean def, boolean desktop) {
        Switch sw = new Switch(this);
        sw.setText(Strings.text(this,title));
        sw.setTextSize(15);
        sw.setTextColor(Design.TEXT);
        sw.setChecked(prefs.getBoolean(key, def));
        sw.setPadding(Design.dp(this, 16), Design.dp(this, 17), Design.dp(this, 16), Design.dp(this, 17));
        sw.setBackground(Design.shape(Design.CARD, Design.dp(this, 14)));
        sw.setThumbTintList(android.content.res.ColorStateList.valueOf(Design.TEXT));
        sw.setTrackTintList(new android.content.res.ColorStateList(new int[][] {{android.R.attr.state_checked}, {}},
                                                                   new int[] {Design.VIOLET, 0xff42465d}));
        sw.setOnCheckedChangeListener((v, on) -> {
            synchronized(Catalog.SETTINGS_LOCK){
            SharedPreferences.Editor e = prefs.edit().putBoolean(key, on);
            if (desktop)
                e.putLong("revision", Catalog.nextRevision(this));
            e.apply();
    }
});
root.addView(sw, new LinearLayout.LayoutParams(-1, -2));
Design.gap(root, 8);
}
void action(String title, Runnable run) {
    root.addView(Design.button(this, title, false, run), new LinearLayout.LayoutParams(-1, -2));
    Design.gap(root, 8);
}
void androidPage(String action) {
    try {
        startActivity(new Intent(action));
    } catch (Exception e) {
        Toast.makeText(this, Strings.text(this,"This setting isn't available on this phone."), Toast.LENGTH_SHORT).show();
    }
}
void build() {
    ScrollView scroll = new ScrollView(this);
    root = Design.column(this);
    int p = Design.dp(this, 24);
    root.setPadding(p, p, p, p);
    scroll.addView(root);
    Design.screen(this, scroll, false);
    Design.header(this, root, "Velox Settings", "MAKE THIS PHONE YOURS");
    action("Back to Velox Home", () -> startActivity(new Intent(this, HomeActivity.class).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP)));
    section("Velox language", "Choose English or German for Velox. Android's system language stays as it is.");
    String[] languages={"English","Deutsch"};
    for(int n=0;n<languages.length;n++) {
        final int selected=n;
        root.addView(Design.button(this,languages[n],prefs.getInt("language",0)==n,()->{
            synchronized(Catalog.SETTINGS_LOCK){prefs.edit().putInt("language",selected).putLong("revision",Catalog.nextRevision(this)).apply();}
        }),new LinearLayout.LayoutParams(-1,-2));
        Design.gap(root,7);
    }
    section("Your play space", "Appearance and layout for Velox Home.");
    toggle("Show clock and date", "clock", true, false);
    toggle("Larger app tiles", "largeTiles", false, false);
    action("Choose wallpaper", () -> {
        try {
            startActivity(Intent.createChooser(new Intent(Intent.ACTION_SET_WALLPAPER), Strings.text(this,"Choose wallpaper")));
        } catch (Exception e) {
            Toast.makeText(this, Strings.text(this,"Choose a wallpaper from Look in desktop Velox."), Toast.LENGTH_LONG).show();
        }
    });
    section("Game orientation", "These controls sync with desktop Velox while it is connected.");
    toggle("Automatically follow game orientation", "rotate", true, true);
    section("Android navigation", "Reversed buttons need Root and an initial phone restart. Your apps keep their normal direction.");
    String[] labels = {"Back · Home · Recents", "Recents · Home · Back", "Gesture navigation"};
    int nav = prefs.getInt("navigation", 0);
    for (int i = 0; i < labels.length; i++) {
        final int value = i;
        root.addView(Design.button(this, labels[i], i == nav, () -> {
            synchronized (Catalog.SETTINGS_LOCK) {
                prefs.edit().putInt("navigation", value).putLong("revision", Catalog.nextRevision(this)).apply();
            }
            build();
        }), new LinearLayout.LayoutParams(-1, -2));
        Design.gap(root, 7);
    }
    section("Default home", "Keep Velox Home as your everyday launcher, or choose another home app.");
    action("Choose default Home app", () -> androidPage(Settings.ACTION_HOME_SETTINGS));
    section("Android settings", "Open the phone's original settings for system and account options.");
    action("Display and text size", () -> androidPage(Settings.ACTION_DISPLAY_SETTINGS));
    action("Sound and volume", () -> androidPage(Settings.ACTION_SOUND_SETTINGS));
    action("Network and internet", () -> androidPage(Settings.ACTION_WIRELESS_SETTINGS));
    action("Apps and permissions", () -> androidPage(Settings.ACTION_MANAGE_APPLICATIONS_SETTINGS));
    action("Accounts", () -> androidPage(Settings.ACTION_SYNC_SETTINGS));
    action("All Android settings", () -> androidPage(Settings.ACTION_SETTINGS));
    section("Connected to your desktop",
            Catalog.desktopConnected()
                ? "Desktop Velox is connected. Navigation and orientation changes sync automatically."
                : "Open desktop Velox to apply navigation and orientation changes. Home appearance works independently.");
    root.addView(Design.text(this, "VELOX  /  YOUR GAME. YOUR SPACE.", 11, Design.VIOLET, true));
    Design.gap(root, 20);
}
}
