package app.velox.home;

import android.app.Activity;
import android.content.Context;
import android.graphics.*;
import android.graphics.drawable.*;
import android.view.*;
import android.widget.*;

final class Design {
    static final int BG = 0xff0e101a, CARD = 0xff1b1e30, TEXT = 0xfff2f3fc, DIM = 0xffa5abc5, VIOLET = 0xff9678ff, BLUE = 0xff477eff;
    static int dp(Context c, float v) { return Math.round(v * c.getResources().getDisplayMetrics().density); }
    static GradientDrawable shape(int color, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radius);
        return d;
    }
    static GradientDrawable accent(Context c) {
        GradientDrawable d = new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {0xff8b6cff, BLUE});
        d.setCornerRadius(dp(c, 16));
        return d;
    }
    static TextView text(Context c, String s, int size, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(Strings.text(c,s));
        t.setTextColor(color);
        t.setTextSize(size);
        if (bold)
            t.setTypeface(Typeface.create("sans-serif-medium", 0));
        return t;
    }
    static LinearLayout column(Context c) {
        LinearLayout v = new LinearLayout(c);
        v.setOrientation(LinearLayout.VERTICAL);
        return v;
    }
    static void gap(LinearLayout v, int h) {
        View g = new View(v.getContext());
        v.addView(g, new LinearLayout.LayoutParams(1, dp(v.getContext(), h)));
    }
    static TextView button(Context c, String s, boolean primary, Runnable action) {
        TextView t = text(c, s, 14, TEXT, true);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(c, 14), dp(c, 13), dp(c, 14), dp(c, 13));
        Drawable normal = primary ? accent(c) : shape(CARD, dp(c, 14));
        t.setBackground(new RippleDrawable(android.content.res.ColorStateList.valueOf(0x33ffffff), normal, null));
        t.setFocusable(true);
        t.setOnClickListener(v -> action.run());
        return t;
    }
    static void screen(Activity a, View root, boolean wallpaper) {
        a.getWindow().setDecorFitsSystemWindows(false);
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            boolean changed = view.getPaddingTop() != bars.top || view.getPaddingBottom() != bars.bottom ||
                              view.getPaddingLeft() != bars.left || view.getPaddingRight() != bars.right;
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            if (changed && view instanceof ScrollView)
                view.post(() -> ((ScrollView)view).scrollTo(0, 0));
            return insets;
        });
        if (root instanceof ScrollView) {
            root.setFocusableInTouchMode(true);
            ((ScrollView)root).setDescendantFocusability(android.view.ViewGroup.FOCUS_BEFORE_DESCENDANTS);
        }
        a.getWindow().setStatusBarColor(BG);
        a.getWindow().setNavigationBarColor(BG);
        if (wallpaper) {
            a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_SHOW_WALLPAPER);
            a.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        }
        root.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                                                new int[] {wallpaper ? 0x80101122 : BG, wallpaper ? 0xb20e101e : BG, 0xf20e101a}));
        a.setContentView(root);
    }
    static class Mark extends View {
        Paint p = new Paint(3);
        Mark(Context c) {
            super(c);
            setContentDescription("Velox");
        }
        protected void onDraw(Canvas c) {
            float s = Math.min(getWidth(), getHeight());
            c.save();
            c.scale(s / 108, s / 108);
            p.setShader(new LinearGradient(0, 0, 108, 108, new int[] {VIOLET, BLUE}, null, Shader.TileMode.CLAMP));
            c.drawRoundRect(3, 3, 105, 105, 26, 26, p);
            p.setShader(null);
            p.setColor(TEXT);
            Path v = new Path();
            v.moveTo(24, 28);
            v.lineTo(40, 28);
            v.lineTo(54, 69);
            v.lineTo(68, 28);
            v.lineTo(84, 28);
            v.lineTo(62, 84);
            v.lineTo(46, 84);
            v.close();
            c.drawPath(v, p);
            c.restore();
        }
    }
    static class SettingsButton extends View {
        final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        SettingsButton(Context context, Runnable action) {
            super(context);
            setContentDescription(Strings.text(context, "Settings"));
            setFocusable(true);
            setBackground(new RippleDrawable(android.content.res.ColorStateList.valueOf(0x338b6cff), shape(CARD, dp(context, 16)), null));
            setOnClickListener(view -> action.run());
        }
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            canvas.save(); canvas.translate(getWidth() / 2f, getHeight() / 2f);
            float unit = dp(getContext(), 1);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.8f * unit); paint.setStrokeJoin(Paint.Join.ROUND);
            paint.setColor(TEXT);
            Path gear = new Path();
            for (int point = 0; point < 32; point++) {
                double angle = point * Math.PI / 16;
                float radius = (point % 4 == 1 || point % 4 == 2 ? 11.5f : 9f) * unit;
                float x = (float)Math.cos(angle) * radius, y = (float)Math.sin(angle) * radius;
                if (point == 0) gear.moveTo(x, y); else gear.lineTo(x, y);
            }
            gear.close(); canvas.drawPath(gear, paint); canvas.drawCircle(0, 0, 3.6f * unit, paint);
            canvas.restore();
        }
    }
    static void header(Activity a, LinearLayout root, String title, String sub) {
        LinearLayout row = new LinearLayout(a);
        row.setGravity(Gravity.CENTER_VERTICAL);
        Mark mark = new Mark(a);
        row.addView(mark, new LinearLayout.LayoutParams(dp(a, 44), dp(a, 44)));
        LinearLayout words = column(a);
        words.setPadding(dp(a, 14), 0, 0, 0);
        words.addView(text(a, title, 25, TEXT, true));
        words.addView(text(a, sub, 12, DIM, false));
        row.addView(words, new LinearLayout.LayoutParams(0, -2, 1));
        root.addView(row);
        gap(root, 24);
    }
}
