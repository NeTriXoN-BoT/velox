import javax.imageio.ImageIO;
import java.awt.*;
import java.awt.geom.*;
import java.awt.image.BufferedImage;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

/**
 * Renders the Velox icon: a faceted "V" on a glossy violet-to-blue tile (the same shape as
 * art::logo in src/art.cpp), as a multi-size .ico plus a 256 px PNG.
 *
 *     java gaming-phone/tools/VeloxIcon.java <out.ico>
 */
public class VeloxIcon {
    static Polygon poly(double s, double[][] pts) {
        Polygon p = new Polygon();
        for (double[] xy : pts) p.addPoint((int) Math.round(xy[0] * s), (int) Math.round(xy[1] * s));
        return p;
    }

    static BufferedImage render(int size) {
        int ss = size < 64 ? 8 : 4, n = size * ss;
        double k = n / 100.0;
        BufferedImage big = new BufferedImage(n, n, BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = big.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        // Dark tile with a neon rim.
        Shape tile = new RoundRectangle2D.Double(0, 0, n, n, 48 * k, 48 * k);
        g.setPaint(new GradientPaint(0, 0, new Color(0x1E1840), 0, n, new Color(0x0A0C1A)));
        g.fill(tile);
        g.setClip(tile);
        // Glow behind the V.
        g.setPaint(new RadialGradientPaint(new Point2D.Double(52 * k, 50 * k), (float) (46 * k), new float[] {0f, 1f},
                new Color[] {new Color(0x8B, 0x6C, 0xFF, 150), new Color(0x8B, 0x6C, 0xFF, 0)}));
        g.fill(new Rectangle2D.Double(0, 0, n, n));
        // Speed streaks.
        g.setColor(new Color(0x7F, 0xA6, 0xFF, 170));
        g.fill(poly(k, new double[][] {{9, 30}, {14, 30}, {25, 58}, {20, 58}}));
        g.setColor(new Color(0x7F, 0xA6, 0xFF, 110));
        g.fill(poly(k, new double[][] {{6, 44}, {10, 44}, {19, 66}, {15, 66}}));
        g.setClip(null);
        g.setStroke(new BasicStroke((float) (4.5 * k)));
        g.setPaint(new GradientPaint(0, 0, new Color(0xB39BFF), n, n, new Color(0x2F6BFF)));
        g.draw(new RoundRectangle2D.Double(2.25 * k, 2.25 * k, n - 4.5 * k, n - 4.5 * k, 44 * k, 44 * k));
        // The V: violet left facet, bright right facet.
        g.setPaint(new GradientPaint((float) (20 * k), (float) (23 * k), new Color(0xC9B8FF), (float) (50 * k), (float) (78 * k), new Color(0x7A5CFF)));
        g.fill(poly(k, new double[][] {{22, 24}, {38, 24}, {51, 56}, {51, 77}, {44, 77}}));
        g.setPaint(new GradientPaint((float) (80 * k), (float) (23 * k), Color.WHITE, (float) (50 * k), (float) (78 * k), new Color(0xB9CCFF)));
        g.fill(poly(k, new double[][] {{64, 24}, {80, 24}, {58, 77}, {51, 77}, {51, 56}}));
        g.dispose();
        BufferedImage out = new BufferedImage(size, size, BufferedImage.TYPE_INT_ARGB);
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                long a = 0, r = 0, gg = 0, b = 0;
                for (int yy = 0; yy < ss; yy++) {
                    for (int xx = 0; xx < ss; xx++) {
                        int c = big.getRGB(x * ss + xx, y * ss + yy), ca = c >>> 24;
                        a += ca;
                        r += ((c >> 16) & 255) * ca;
                        gg += ((c >> 8) & 255) * ca;
                        b += (c & 255) * ca;
                    }
                }
                int oa = (int) (a / (ss * ss));
                int or = a == 0 ? 0 : (int) (r / a), og = a == 0 ? 0 : (int) (gg / a), ob = a == 0 ? 0 : (int) (b / a);
                out.setRGB(x, y, (oa << 24) | (or << 16) | (og << 8) | ob);
            }
        }
        return out;
    }

    public static void main(String[] args) throws Exception {
        int[] sizes = {16, 20, 24, 32, 40, 48, 64, 128, 256};
        List<byte[]> images = new ArrayList<>();
        for (int s : sizes) {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            ImageIO.write(render(s), "png", bos);
            images.add(bos.toByteArray());
        }
        ByteBuffer head = ByteBuffer.allocate(6 + 16 * sizes.length).order(ByteOrder.LITTLE_ENDIAN);
        head.putShort((short) 0).putShort((short) 1).putShort((short) sizes.length);
        int offset = 6 + 16 * sizes.length;
        for (int i = 0; i < sizes.length; i++) {
            int s = sizes[i];
            head.put((byte) (s >= 256 ? 0 : s)).put((byte) (s >= 256 ? 0 : s)).put((byte) 0).put((byte) 0);
            head.putShort((short) 1).putShort((short) 32).putInt(images.get(i).length).putInt(offset);
            offset += images.get(i).length;
        }
        try (FileOutputStream os = new FileOutputStream(args[0])) {
            os.write(head.array());
            for (byte[] b : images) os.write(b);
        }
        ImageIO.write(render(256), "png", new File(args[0].replace(".ico", "-256.png")));
    }
}
