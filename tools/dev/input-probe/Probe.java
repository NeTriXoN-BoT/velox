package app.velox.inputprobe;
import android.app.*;
import android.os.*;
import android.view.*;
import android.graphics.*;
import java.io.*;
public class Probe extends Activity {
    public void onCreate(Bundle b) {
        super.onCreate(b);
        setRequestedOrientation(getIntent().getIntExtra("orientation", 1));
        setContentView(new View(this) {
            Paint paint = new Paint(3);
            protected void onDraw(Canvas c) {
                c.drawColor(0xff111729);
                paint.setColor(0xffaa88ff);
                paint.setTextSize(60);
                c.drawText("Velox Input Probe", 70, 180, paint);
            }
            public boolean onTouchEvent(MotionEvent e) {
                try (FileOutputStream f = openFileOutput("events.tsv", MODE_APPEND)) {
                    String s =
                        SystemClock.uptimeMillis() + "\t" + e.getActionMasked() + "\t" + e.getActionIndex() + "\t" + e.getPointerCount();
                    for (int i = 0; i < e.getPointerCount(); i++)
                        s += "\t" + e.getPointerId(i) + ":" + Math.round(e.getX(i)) + ":" + Math.round(e.getY(i));
                    f.write((s + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
                } catch (Exception ex) {
                    throw new RuntimeException(ex);
                }
                return true;
            }
        });
    }
}
