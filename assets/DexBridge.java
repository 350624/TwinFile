import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import com.android.tools.smali.dexlib2.dexbacked.DexBackedDexFile;
import com.android.tools.smali.dexlib2.iface.ClassDef;
import com.android.tools.smali.dexlib2.writer.builder.DexBuilder;
import com.android.tools.smali.dexlib2.writer.io.FileDataStore;
import com.android.tools.smali.baksmali.BaksmaliOptions;
import com.android.tools.smali.baksmali.Adaptors.ClassDefinition;
import com.android.tools.smali.baksmali.formatter.BaksmaliWriter;
import brut.androlib.smali.SmaliBuilder;

public class DexBridge {
    private static long written = 0;
    private static class LimitedWriter extends FilterWriter {
        private long classSize = 0;
        LimitedWriter(Writer writer) { super(writer); }
        private void account(int count) throws IOException {
            classSize += count;
            written += count;
            if (classSize > 2 * 1024 * 1024 || written > 128 * 1024 * 1024) throw new IOException("Smali workspace size limit exceeded");
        }
        public void write(int character) throws IOException { account(1); out.write(character); }
        public void write(char[] buffer, int offset, int length) throws IOException { account(length); out.write(buffer, offset, length); }
        public void write(String text, int offset, int length) throws IOException { account(length); out.write(text, offset, length); }
    }
    public static void main(String[] arguments) throws Exception {
        if (arguments.length != 4) throw new IOException("Invalid arguments");
        Path source = Paths.get(arguments[1]);
        if (Files.size(source) > 64 * 1024 * 1024) throw new IOException("DEX exceeds 64 MiB");
        DexBackedDexFile dex = new DexBackedDexFile(Files.readAllBytes(source), 0);
        if (dex.classCount > 50000) throw new IOException("Too many classes");
        Path workspace = Paths.get(arguments[2]);
        if (arguments[0].equals("decode")) {
            Files.createDirectories(workspace);
            BaksmaliOptions options = new BaksmaliOptions();
            options.debugInfo = true;
            options.localsDirective = true;
            Set<String> descriptors = new HashSet<>();
            for (int index = 0; index < dex.classCount; index++) {
                ClassDef definition = (ClassDef) dex.classSection.get(index);
                String descriptor = definition.getType();
                if (!descriptors.add(descriptor)) throw new IOException("Duplicate class");
                Path output = workspace.resolve(String.format(Locale.ROOT, "%05d.smali", index));
                try (Writer writer = new LimitedWriter(Files.newBufferedWriter(output, StandardCharsets.UTF_8))) {
                    new ClassDefinition(options, definition).writeTo(new BaksmaliWriter(writer));
                }
            }
            System.out.println(dex.classCount + " classes");
        } else if (arguments[0].equals("build")) {
            DexBuilder builder = new DexBuilder(dex.opcodes);
            SmaliBuilder assembler = new SmaliBuilder(dex.opcodes.api);
            try (var files = Files.list(workspace)) {
                List<Path> entries = files.sorted().limit(50001).toList();
                if (entries.size() > 50000) throw new IOException("Too many classes");
                long size = 0;
                for (Path file : entries) {
                    if (!file.getFileName().toString().matches("[0-9]{5}\\.smali") || Files.isSymbolicLink(file)) throw new IOException("Invalid workspace entry");
                    long length = Files.size(file);
                    size += length;
                    if (length > 8 * 1024 * 1024 || size > 512 * 1024 * 1024) throw new IOException("Smali workspace size limit exceeded");
                    if (!assembler.buildFile(file.toFile(), builder)) throw new IOException("Smali compile failed: " + file.getFileName());
                }
            }
            FileDataStore output = new FileDataStore(new File(arguments[3]));
            try { builder.writeTo(output); } finally { output.raf.close(); }
        } else {
            throw new IOException("Unknown operation");
        }
    }
}
