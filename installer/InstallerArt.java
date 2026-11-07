import java.awt.*;
import java.awt.geom.*;
import java.awt.image.*;
import java.io.*;
import javax.imageio.ImageIO;
public class InstallerArt {
    public static void main(String[] args)throws Exception {
        File out=new File(args[0]);out.mkdirs();
        int[][] large={{164,314},{192,386},{246,459},{273,556},{328,628},{355,707},{410,797}};
        for(int[] size:large){int w=size[0],content=size[1],h=content+Math.round(w*.28f);BufferedImage image=new BufferedImage(w,h,BufferedImage.TYPE_INT_RGB);Graphics2D g=image.createGraphics();g.setRenderingHint(RenderingHints.KEY_ANTIALIASING,RenderingHints.VALUE_ANTIALIAS_ON);
            g.setPaint(new GradientPaint(0,0,new Color(0x201B42),w,h,new Color(0x0D1425)));g.fillRect(0,0,w,h);
            for(int n=0;n<7;n++){float x=w*(.1f+n*.17f);g.setColor(new Color(124,100,255,18+n*3));Path2D path=new Path2D.Float();path.moveTo(x,h*.65);path.lineTo(x+w*.08,h*.65);path.lineTo(x-w*.6,h*1.1);path.lineTo(x-w*.68,h*1.1);path.closePath();g.fill(path);}
            int logo=(int)(w*.72);g.drawImage(VeloxIcon.render(logo),(w-logo)/2,(int)(content*.19),null);
            g.setColor(new Color(0xF2F3F7));g.setFont(new Font("SansSerif",Font.BOLD,(int)(w*.17)));String name="VELOX";FontMetrics font=g.getFontMetrics();g.drawString(name,(w-font.stringWidth(name))/2,(int)(content*.68));
            g.setPaint(new GradientPaint(w*.2f,0,new Color(0x8B6CFF),w*.8f,0,new Color(0x3D7BFF)));g.fill(new RoundRectangle2D.Float(w*.2f,content*.75f,w*.6f,Math.max(2,w*.018f),3,3));g.dispose();ImageIO.write(image,"png",new File(out,"wizard-"+w+"x"+h+".png"));}
        int[][] small={{55,55},{64,68},{83,80},{92,97},{110,106},{119,123},{138,140}};
        for(int[] size:small){BufferedImage image=new BufferedImage(size[0],size[1],BufferedImage.TYPE_INT_ARGB);Graphics2D g=image.createGraphics();int n=Math.min(size[0],size[1]);g.drawImage(VeloxIcon.render(n),(size[0]-n)/2,(size[1]-n)/2,null);g.dispose();ImageIO.write(image,"png",new File(out,"small-"+size[0]+"x"+size[1]+".png"));}
    }
}
