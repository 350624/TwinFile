import java.awt.Color;
import java.awt.Dimension;
import java.awt.geom.Rectangle2D;
import java.awt.image.BufferedImage;
import java.io.ByteArrayOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import javax.imageio.ImageIO;
import org.apache.poi.sl.usermodel.PictureData.PictureType;
import org.apache.poi.xslf.usermodel.*;
public final class SlideFixture {
    public static void main(String[] args) throws Exception {
        try (XMLSlideShow deck = new XMLSlideShow()) {
            deck.setPageSize(new Dimension(960, 540));
            for (int index = 0; index < 2; ++index) {
                XSLFSlide slide = deck.createSlide();
                slide.getBackground().setFillColor(index == 0 ? Color.WHITE : new Color(235, 245, 240));
                XSLFTextBox box = slide.createTextBox(); box.setAnchor(new Rectangle2D.Double(50, 40, 850, 160));
                XSLFTextRun run = box.setText(index == 0 ? "陪伴式聊天机器人 · 设计参考" : "第二页 · 中文内容");
                run.setFontFamily("Microsoft YaHei"); run.setFontSize(32.0); run.setFontColor(Color.BLACK);
                BufferedImage image = new BufferedImage(80, 60, BufferedImage.TYPE_INT_RGB);
                var painter = image.createGraphics(); painter.setColor(Color.RED); painter.fillRect(0, 0, 80, 60); painter.dispose();
                ByteArrayOutputStream bytes = new ByteArrayOutputStream(); ImageIO.write(image, "png", bytes);
                XSLFPictureData picture = deck.addPicture(bytes.toByteArray(), PictureType.PNG);
                slide.createPicture(picture).setAnchor(new Rectangle2D.Double(100, 240, 160, 120));
            }
            if (args.length > 1 && args[1].equals("external")) {
                deck.getSlides().get(0).getPackagePart().addExternalRelationship("http://127.0.0.1:9/private.png", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image");
            }
            try (var output = Files.newOutputStream(Path.of(args[0]))) { deck.write(output); }
        }
    }
}
