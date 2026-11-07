import java.awt.*;
import java.awt.geom.*;
import java.awt.image.BufferedImage;
import java.nio.file.*;
import javax.imageio.ImageIO;

/** Renders the repository header from the project's logo and wallpaper assets. */
public class RepoBanner {
    static void text(Graphics2D g, String s, int x, int y, int size, int style, int color) {
        g.setFont(new Font("Segoe UI",style,size));g.setColor(new Color(color));g.drawString(s,x,y);
    }
    static void pill(Graphics2D g,String s,int x,int y,int width) {
        g.setColor(new Color(0x24283f));g.fillRoundRect(x,y,width,42,21,21);
        text(g,s,x+20,y+28,18,Font.PLAIN,0xdadff6);
    }
    public static void main(String[] args) throws Exception {
        Path root=Path.of(args.length>0?args[0]:".");
        BufferedImage image=new BufferedImage(1440,720,BufferedImage.TYPE_INT_RGB);
        Graphics2D g=image.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING,RenderingHints.VALUE_ANTIALIAS_ON);
        g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING,RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
        g.setRenderingHint(RenderingHints.KEY_INTERPOLATION,RenderingHints.VALUE_INTERPOLATION_BICUBIC);
        g.setPaint(new GradientPaint(0,0,new Color(0x141125),1440,720,new Color(0x0b1525)));g.fillRect(0,0,1440,720);
        g.setPaint(new RadialGradientPaint(new Point2D.Float(1120,320),650,new float[]{0,1},new Color[]{new Color(0x6041b5),new Color(0x111725)}));
        g.fillRect(730,0,710,720);
        g.setColor(new Color(0xffffff, true));
        for(int i=0;i<6;i++){g.setColor(new Color(160,149,244,13));g.setStroke(new BasicStroke(2));g.drawLine(740+i*105,-20,300+i*105,740);}
        g.drawImage(VeloxIcon.render(92),70,65,null);
        text(g,"VELOX",185,126,60,Font.BOLD,0xf4f6ff);
        text(g,"ANDROID GAMING FOR WINDOWS",74,201,18,Font.BOLD,0xafa1f6);
        text(g,"Your games.",68,291,65,Font.BOLD,0xffffff);
        text(g,"Your phone.",68,369,65,Font.BOLD,0xffffff);
        text(g,"Your space.",68,447,65,Font.BOLD,0xbcaaff);
        pill(g,"Game library",74,498,154);pill(g,"Custom controls",241,498,185);pill(g,"Multiple phones",439,498,185);
        text(g,"Open source. Made for your play style.",74,600,25,Font.PLAIN,0xc8cde3);
        text(g,"Windows 10 / 11   ·   English & Deutsch",74,645,19,Font.PLAIN,0x8994b5);
        // A stylized phone preview uses the shipped artwork without personal UI data.
        for(int i=24;i>0;i--){g.setColor(new Color(0,0,0,3));g.fillRoundRect(1010-i,53-i,290+i*2,618+i*2,66+i,66+i);}
        g.setColor(new Color(0x45485f));g.fillRoundRect(1006,49,298,626,66,66);
        g.setColor(new Color(0x101219));g.fillRoundRect(1010,53,290,618,62,62);
        Shape clip=new RoundRectangle2D.Double(1020,65,270,590,50,50);g.setClip(clip);
        BufferedImage wall=ImageIO.read(root.resolve("wallpapers/Velox-chan - Windowlight.png").toFile());
        g.drawImage(wall,1020,65,270,600,null);
        g.setPaint(new GradientPaint(0,510,new Color(0,0,0,0),0,655,new Color(9,10,27,210)));g.fillRect(1020,480,270,175);
        g.setClip(null);g.setColor(new Color(0x0b0c12));g.fillOval(1143,76,23,23);
        for(int i=0;i<4;i++){g.setColor(new Color(new int[]{0x8a6aff,0x497fff,0x60c5c4,0xe1ddff}[i]));g.fillRoundRect(1043+i*58,578,42,42,14,14);}
        g.setColor(Color.WHITE);g.fillRoundRect(1124,640,62,4,4,4);
        g.setColor(new Color(0x20263a));g.fillRoundRect(845,214,133,56,18,18);text(g,"144 Hz",866,251,26,Font.BOLD,0xd9d1ff);
        g.setColor(new Color(0x20263a));g.fillRoundRect(836,287,142,56,18,18);text(g,"PLAY",869,324,25,Font.BOLD,0x9be1d8);
        g.dispose();Path output=root.resolve("docs/assets/velox-banner.png");Files.createDirectories(output.getParent());ImageIO.write(image,"png",output.toFile());
    }
}
