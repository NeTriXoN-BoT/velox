package app.velox.home;

import android.app.*;
import android.app.role.RoleManager;
import android.content.*;
import android.content.res.Configuration;
import android.graphics.Typeface;
import android.os.*;
import android.text.*;
import android.view.*;
import android.widget.*;
import java.text.SimpleDateFormat;
import java.util.*;

public class HomeActivity extends Activity {
    LinearLayout root, tiles;
    EditText search;
    String query = "";
    int tab = 0;
    TextView clock, date;
    Handler handler = new Handler();
    ArrayList<Catalog.App> cachedApps = new ArrayList<>();
    Runnable appRefresh = () -> reloadApps();
    SharedPreferences.OnSharedPreferenceChangeListener appChanges = (prefs, key) -> {
        if ("language".equals(key)) { handler.post(() -> build()); return; }
        if (key != null && (key.startsWith("fav:") || key.startsWith("game:") || key.startsWith("played:"))) {
            handler.removeCallbacks(appRefresh);
            handler.postDelayed(appRefresh, 80);
        }
    };
    Runnable time = new Runnable() {
        public void run() {
            if (clock != null) {
                clock.setText(android.text.format.DateFormat.getTimeFormat(HomeActivity.this).format(new Date()));
                date.setText(new SimpleDateFormat("EEEE, d MMMM", Strings.locale(HomeActivity.this)).format(new Date()));
            }
            handler.postDelayed(this, 60000 - System.currentTimeMillis() % 60000);
        }
    };
    public void onCreate(Bundle b) { super.onCreate(b); }
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        build();
    }
    protected void onResume() {
        super.onResume();
        Catalog.prefs(this).registerOnSharedPreferenceChangeListener(appChanges);
        build();
    }
    protected void onPause() {
        super.onPause();
        handler.removeCallbacks(time);
        handler.removeCallbacks(appRefresh);
        Catalog.prefs(this).unregisterOnSharedPreferenceChangeListener(appChanges);
    }
    public void onBackPressed() {
        if (!query.isEmpty()) {
            search.setText("");
        } else if (tab != 0) {
            tab = 0;
            build();
        }
    }
    void build() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        root = Design.column(this);
        int pad = Design.dp(this, 24);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);
        Design.screen(this, scroll, true);
        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        Design.Mark mark = new Design.Mark(this);
        bar.addView(mark, new LinearLayout.LayoutParams(Design.dp(this, 38), Design.dp(this, 38)));
        TextView brand = Design.text(this, "  VELOX", 20, Design.TEXT, true);
        brand.setLetterSpacing(.12f);
        bar.addView(brand, new LinearLayout.LayoutParams(0, -2, 1));
        bar.addView(new Design.SettingsButton(this, () -> startActivity(new Intent(this, SettingsActivity.class))),
                    new LinearLayout.LayoutParams(Design.dp(this, 48), Design.dp(this, 48)));
        root.addView(bar);
        Design.gap(root, 22);
        if (Catalog.prefs(this).getBoolean("clock", true) &&
            getResources().getConfiguration().orientation != Configuration.ORIENTATION_LANDSCAPE) {
            clock = Design.text(this, "", 54, Design.TEXT, false);
            clock.setTypeface(Typeface.create("sans-serif-light", 0));
            root.addView(clock);
            date = Design.text(this, "", 14, Design.DIM, false);
            root.addView(date);
            Design.gap(root, 24);
        } else {
            clock = null;
            date = null;
        }
        search = new EditText(this);
        search.setSingleLine(true);
        search.setTextColor(Design.TEXT);
        search.setHintTextColor(Design.DIM);
        search.setTextSize(15);
        search.setHint(Strings.text(this, getResources().getConfiguration().fontScale > 1.2f ? "Search apps" : "Search your games and apps"));
        search.setBackground(Design.shape(0xd91b1e30, Design.dp(this, 18)));
        search.setPadding(pad, Design.dp(this, 14), pad, Design.dp(this, 14));
        search.setText(query);
        search.setMinHeight(Design.dp(this, 52));
        root.addView(search, new LinearLayout.LayoutParams(-1, -2));
        search.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int st, int count, int after) {
            }
            public void onTextChanged(CharSequence s, int st, int before, int count) {
                query = s.toString();
                renderApps();
            }
            public void afterTextChanged(Editable e) {
            }
        });
        Design.gap(root, 16);
        LinearLayout tabs = Design.column(this);
        String[] names = {"All apps", "Games", "Favorites", "Recent"};
        // Keep filter names intact on a narrow phone. Larger accessibility
        // text gets two rows instead of being squeezed into four columns.
        Configuration config = getResources().getConfiguration();
        int filterColumns = config.fontScale > 1.2f || config.screenWidthDp < 340 ? 2 : 4;
        LinearLayout filterRow = null;
        for (int i = 0; i < names.length; i++) {
            if (i % filterColumns == 0) {
                filterRow = new LinearLayout(this);
                LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(-1, -2);
                if (i > 0) rowParams.topMargin = Design.dp(this, 6);
                tabs.addView(filterRow, rowParams);
            }
            final int id = i;
            TextView t = Design.button(this, names[i], tab == i, () -> {
                tab = id;
                build();
            });
            t.setSingleLine(true);
            t.setEllipsize(TextUtils.TruncateAt.END);
            t.setTextSize(13);
            t.setPadding(Design.dp(this, 8), Design.dp(this, 13), Design.dp(this, 8), Design.dp(this, 13));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -2, 1);
            if (i % filterColumns < filterColumns - 1) lp.setMargins(0, 0, Design.dp(this, 5), 0);
            filterRow.addView(t, lp);
        }
        root.addView(tabs);
        Design.gap(root, 20);
        tiles = Design.column(this);
        root.addView(tiles, new LinearLayout.LayoutParams(-1, -2));
        reloadApps();
        RoleManager roles = getSystemService(RoleManager.class);
        if (roles != null && !roles.isRoleHeld(RoleManager.ROLE_HOME)) {
            Design.gap(root, 20);
            root.addView(Design.button(this, "Make Velox my Home", true,
                                       () -> startActivityForResult(roles.createRequestRoleIntent(RoleManager.ROLE_HOME), 1)));
        }
        scroll.requestFocus();
        scroll.post(() -> scroll.scrollTo(0, 0));
        handler.removeCallbacks(time);
        handler.post(time);
    }
    void reloadApps() {
        cachedApps = Catalog.apps(this);
        renderApps();
    }
    void renderApps() {
        if (tiles == null)
            return;
        tiles.removeAllViews();
        // Search reuses app metadata and icons instead of querying PackageManager per keystroke.
        ArrayList<Catalog.App> list = new ArrayList<>(cachedApps);
        String normalizedQuery = query.toLowerCase(Locale.ROOT);
        if (tab == 3)
            list.sort((a, b) -> Long.compare(b.played, a.played));
        int columns = getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE ? 7 : 4;
        if (getResources().getConfiguration().fontScale > 1.2f) columns = columns == 7 ? 5 : 3;
        if (Catalog.prefs(this).getBoolean("largeTiles", false))
            columns = Math.max(3, columns - 1);
        LinearLayout row = null;
        int n = 0;
        for (Catalog.App a : list) {
            if (!a.label.toLowerCase(Locale.ROOT).contains(normalizedQuery) && !a.packageName.toLowerCase(Locale.ROOT).contains(normalizedQuery))
                continue;
            if (tab == 1 && !a.game || tab == 2 && !a.favorite || tab == 3 && a.played == 0)
                continue;
            if (n % columns == 0) {
                row = new LinearLayout(this);
                row.setGravity(Gravity.TOP);
                tiles.addView(row, new LinearLayout.LayoutParams(-1, -2));
            }
            LinearLayout cell = Design.column(this);
            cell.setGravity(Gravity.CENTER);
            cell.setPadding(Design.dp(this, 5), Design.dp(this, 13), Design.dp(this, 5), Design.dp(this, 13));
            cell.setBackground(new android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(0x338b6cff), null,
                                                                            Design.shape(0xffffffff, Design.dp(this, 18))));
            cell.setFocusable(true);
            cell.setContentDescription(a.label + (a.favorite ? (Catalog.prefs(this).getInt("language",0)==1 ? ", Favorit" : ", favorite") : ""));
            ImageView icon = new ImageView(this);
            icon.setImageDrawable(a.icon);
            cell.addView(icon, new LinearLayout.LayoutParams(Design.dp(this, 53), Design.dp(this, 53)));
            Design.gap(cell, 8);
            TextView label = Design.text(this, "", 12, Design.TEXT, true);
            label.setText(a.label);
            label.setGravity(Gravity.CENTER);
            label.setMaxLines(2);
            label.setEllipsize(TextUtils.TruncateAt.END);
            cell.addView(label, new LinearLayout.LayoutParams(-1, (int)Math.ceil(label.getPaint().getFontSpacing() * 2) + Design.dp(this, 5)));
            cell.setOnClickListener(v -> {
                if (!Catalog.launch(this, a.component))
                    Toast.makeText(this, Strings.text(this,"This app couldn't be opened."), Toast.LENGTH_SHORT).show();
            });
            cell.setOnLongClickListener(v -> {
                new AlertDialog.Builder(this)
                    .setTitle(a.label)
                    .setItems(Strings.texts(this,new String[] {a.favorite ? "Remove from favorites" : "Add to favorites",
                                            a.game ? "Remove from Games" : "Mark as game", "App information"}),
                              (d, which) -> {
                                  if (which == 0) {
                                      Catalog.prefs(this).edit().putBoolean("fav:" + a.component, !a.favorite).apply();
                                      reloadApps();
                                  } else if (which == 1) {
                                      Catalog.prefs(this).edit().putBoolean("game:" + a.component, !a.game).apply();
                                      reloadApps();
                                  } else
                                      startActivity(new Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                                               android.net.Uri.parse("package:" + a.packageName)));
                              })
                    .show();
                return true;
            });
            row.addView(cell, new LinearLayout.LayoutParams(0, -2, 1));
            n++;
        }
        if (row != null)
            while (n++ % columns != 0)
                row.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1));
        if (n == 0) {
            TextView empty =
                Design.text(this,
                            tab == 1   ? "Games will appear here when Android identifies them as games. All apps always shows everything."
                            : tab == 2 ? "Hold an app to add it to your favorites."
                            : tab == 3 ? "Your recently opened apps will appear here."
                                       : "No matching apps.",
                            15, Design.DIM, false);
            empty.setPadding(0, Design.dp(this, 24), 0, Design.dp(this, 24));
            tiles.addView(empty);
        }
    }
}
