import java.awt.Color;
import java.awt.Dimension;
import java.awt.Font;
import java.awt.GraphicsEnvironment;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.image.BufferedImage;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Base64;
import javax.imageio.ImageIO;
import org.apache.poi.openxml4j.opc.*;
import org.apache.poi.openxml4j.util.ZipSecureFile;
import org.apache.poi.util.IOUtils;
import org.apache.poi.xslf.usermodel.*;
import java.util.zip.ZipFile;
import java.util.zip.ZipEntry;
import java.util.Enumeration;
import java.util.Collections;
import org.apache.poi.sl.draw.Drawable;

public final class SlideBridge {
    private static String fallbackFont() {
        String sample = "中文演示文稿陪伴式聊天机器人设计参考";
        String[] available = GraphicsEnvironment.getLocalGraphicsEnvironment().getAvailableFontFamilyNames(java.util.Locale.ROOT);
        for (String preferred : new String[]{"Microsoft YaHei", "Microsoft YaHei UI", "SimSun", "Noto Sans CJK SC", "DengXian"}) {
            for (String family : available) if (preferred.equalsIgnoreCase(family) && new Font(family, Font.PLAIN, 12).canDisplayUpTo(sample) == -1) return family;
        }
        for (String family : available) if (new Font(family, Font.PLAIN, 12).canDisplayUpTo(sample) == -1) return family;
        return Font.DIALOG;
    }
    private static void prepareFonts(Iterable<XSLFShape> shapes, String fallback) {
        for (XSLFShape shape : shapes) {
            if (shape instanceof XSLFGroupShape) prepareFonts(((XSLFGroupShape)shape).getShapes(), fallback);
            if (shape instanceof XSLFTextShape) prepareTextFonts((XSLFTextShape)shape, fallback);
            if (shape instanceof XSLFTable) for (XSLFTableRow row : ((XSLFTable)shape).getRows()) for (XSLFTableCell cell : row.getCells()) prepareTextFonts(cell, fallback);
        }
    }
    private static void prepareTextFonts(XSLFTextShape shape, String fallback) {
        for (XSLFTextParagraph paragraph : shape.getTextParagraphs()) for (XSLFTextRun run : paragraph.getTextRuns()) {
            String text = run.getRawText();
            if (text == null || text.isEmpty()) continue;
            String family = run.getFontFamily();
            if (family == null) family = paragraph.getDefaultFontFamily();
            Font font = new Font(family == null ? Font.DIALOG : family, Font.PLAIN, 12);
            // POI 5.5.1's explicit TextAttribute.FONT overrides its per-glyph FAMILY fallback.
            // Adjust only the in-memory run when its effective font cannot render the text.
            if (family == null || font.canDisplayUpTo(text) != -1 || Font.DIALOG.equals(font.getFamily()) && !Font.DIALOG.equalsIgnoreCase(family)) {
                run.setFontFamily(fallback);
                run.setFontFamily(fallback, org.apache.poi.common.usermodel.fonts.FontGroup.EAST_ASIAN);
            }
        }
    }
    private static void fail(String message) {
        System.out.println("ERROR\t" + Base64.getEncoder().encodeToString(message.getBytes(StandardCharsets.UTF_8)));
        System.out.flush();
    }
    private static void checkArchive(Path path) throws Exception {
        if (Files.size(path) > 128L * 1024 * 1024) throw new Exception("PPTX exceeds 128 MiB");
        try (ZipFile zip = new ZipFile(path.toFile())) {
            Enumeration<? extends ZipEntry> entries = zip.entries();
            long total = 0; int count = 0;
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                if (++count > 20000 || entry.getSize() < 0 || entry.getSize() > 64L * 1024 * 1024) throw new Exception("PPTX entry limit exceeded");
                total += entry.getSize();
                if (total > 256L * 1024 * 1024) throw new Exception("PPTX expanded size exceeds 256 MiB");
            }
            if (zip.getEntry("ppt/presentation.xml") == null) throw new Exception("Not a PPTX presentation");
        }
    }
    private static void text(Iterable<XSLFShape> shapes, StringBuilder output) {
        for (XSLFShape shape : shapes) {
            if (output.length() > 256 * 1024) return;
            if (shape instanceof XSLFTextShape) output.append(((XSLFTextShape)shape).getText()).append('\n');
            else if (shape instanceof XSLFGroupShape) text(((XSLFGroupShape)shape).getShapes(), output);
            else if (shape instanceof XSLFTable) {
                for (XSLFTableRow row : ((XSLFTable)shape).getRows()) {
                    for (XSLFTableCell cell : row.getCells()) output.append(cell.getText()).append('\t');
                    output.append('\n');
                }
            }
        }
    }
    public static void main(String[] args) {
        try {
            if (args.length != 2) throw new Exception("Invalid viewer arguments");
            Path input = Path.of(args[0]), workspace = Path.of(args[1]);
            checkArchive(input);
            ZipSecureFile.setMinInflateRatio(0.01);
            ZipSecureFile.setMaxEntrySize(64L * 1024 * 1024);
            IOUtils.setByteArrayMaxOverride(64 * 1024 * 1024);
            try (OPCPackage pkg = OPCPackage.open(input.toFile(), PackageAccess.READ)) {
                // Reject external relationships before the renderer can resolve resources.
                for (PackageRelationship rel : pkg.getRelationships()) if (rel.getTargetMode() == TargetMode.EXTERNAL && !rel.getRelationshipType().endsWith("/hyperlink")) throw new Exception("External relationships are not supported");
                for (PackagePart part : pkg.getParts()) {
                    if (part.isRelationshipPart()) continue;
                    for (PackageRelationship rel : part.getRelationships()) if (rel.getTargetMode() == TargetMode.EXTERNAL && !rel.getRelationshipType().endsWith("/hyperlink")) throw new Exception("External relationships are not supported");
                }
                try (XMLSlideShow deck = new XMLSlideShow(pkg); BufferedReader commands = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8))) {
                    Dimension size = deck.getPageSize(); int count = deck.getSlides().size();
                    if (count < 1 || count > 500 || size.width < 16 || size.height < 16 || size.width > 20000 || size.height > 20000) throw new Exception("Invalid slide count or dimensions (maximum 500 slides)");
                    double scale = Math.min(1600.0 / size.width, 1200.0 / size.height);
                    String fallback = fallbackFont();
                    for (XSLFSlide slide : deck.getSlides()) {
                        prepareFonts(slide.getShapes(), fallback);
                        if (slide.getSlideLayout() != null) prepareFonts(slide.getSlideLayout().getShapes(), fallback);
                        if (slide.getSlideMaster() != null) prepareFonts(slide.getSlideMaster().getShapes(), fallback);
                    }
                    int width = Math.max(1, (int)Math.round(size.width * scale)), height = Math.max(1, (int)Math.round(size.height * scale));
                    System.out.println("READY\t" + count); System.out.flush();
                    String command;
                    while ((command = commands.readLine()) != null) {
                        if (command.equals("QUIT")) break;
                        if (!command.matches("RENDER\\t[0-9]{1,3}")) throw new Exception("Invalid render command");
                        int page = Integer.parseInt(command.substring(7));
                        if (page < 0 || page >= count) throw new Exception("Invalid page");
                        XSLFSlide slide = deck.getSlides().get(page);
                        BufferedImage image = new BufferedImage(width, height, BufferedImage.TYPE_INT_RGB);
                        Graphics2D graphics = image.createGraphics();
                        try {
                            graphics.setColor(Color.WHITE); graphics.fillRect(0, 0, width, height);
                            graphics.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
                            graphics.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
                            graphics.setRenderingHint(RenderingHints.KEY_RENDERING, RenderingHints.VALUE_RENDER_QUALITY);
                            // POI otherwise reuses the unsupported run font as its own fallback.
                            graphics.setRenderingHint(Drawable.FONT_FALLBACK, Collections.singletonMap("*", fallback));
                            graphics.scale(scale, scale); slide.draw(graphics);
                        } finally { graphics.dispose(); }
                        if (!ImageIO.write(image, "png", workspace.resolve("page.png").toFile())) throw new Exception("PNG encoder unavailable");
                        StringBuilder content = new StringBuilder(); text(slide.getShapes(), content);
                        Files.writeString(workspace.resolve("page.txt"), content.substring(0, Math.min(content.length(), 256 * 1024)), StandardCharsets.UTF_8);
                        System.out.println("PAGE\t" + page); System.out.flush();
                    }
                }
            }
        } catch (Throwable error) {
            String message = error.getMessage();
            fail(message == null ? error.getClass().getSimpleName() : message.substring(0, Math.min(message.length(), 1200)));
            System.exit(1);
        }
    }
}
