import com.reandroid.apk.AndroidFrameworks;
import com.reandroid.arsc.chunk.TableBlock;
import com.reandroid.arsc.chunk.PackageBlock;
import com.reandroid.arsc.chunk.xml.ResXmlDocument;
import com.reandroid.arsc.value.Entry;
import com.reandroid.json.JSONArray;
import com.reandroid.json.JSONObject;
import com.reandroid.xml.XMLFactory;
import java.io.File;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Iterator;
import java.util.ArrayList;
import java.util.List;
import java.util.Comparator;
import java.util.Map;
import java.util.HashMap;
import javax.xml.parsers.DocumentBuilderFactory;
import org.w3c.dom.Element;
import org.w3c.dom.Node;
import org.w3c.dom.NamedNodeMap;
import org.xml.sax.InputSource;
import java.io.StringReader;
import org.xmlpull.v1.XmlSerializer;

public final class AndroidBinaryBridge {
    private static final int LIMIT = 32 * 1024 * 1024;
    private static String read(File file) throws Exception {
        if (!file.isFile() || file.length() > LIMIT) throw new Exception("Input exceeds 32 MiB");
        return Files.readString(file.toPath(), StandardCharsets.UTF_8);
    }
    private static String xml(ResXmlDocument doc) throws Exception {
        StringWriter text = new StringWriter();
        XmlSerializer serializer = XMLFactory.newSerializer(text);
        doc.serialize(serializer, false);
        serializer.flush();
        return text.toString();
    }
    private static JSONArray resources(TableBlock table) throws Exception {
        JSONArray rows = new JSONArray();
        Iterator<Entry> entries = entries(table).iterator();
        while (entries.hasNext()) {
            Entry entry = entries.next();
            if (entry.isNull()) continue;
            if (rows.length() >= 100000) throw new Exception("Resource entry limit exceeded");
            JSONObject row = new JSONObject();
            row.put("id", entry.getResourceId());
            row.put("type", entry.getTypeName());
            row.put("name", entry.getName());
            row.put("config", entry.getResConfig().getQualifiers());
            row.put("value", entry.toJson());
            rows.put(row);
        }
        return rows;
    }
    private static List<Entry> entries(TableBlock table) {
        List<Entry> entries = new ArrayList<>();
        for (PackageBlock pkg : table) {
            Iterator<com.reandroid.arsc.model.ResourceEntry> resources = pkg.getResources();
            while (resources.hasNext()) for (Entry entry : resources.next()) if (!entry.isNull()) entries.add(entry);
        }
        entries.sort(Comparator.comparingLong((Entry e) -> Integer.toUnsignedLong(e.getResourceId())).thenComparing(e -> e.getResConfig().getQualifiers()));
        return entries;
    }
    private static Map<String, String> xmlAttributes(String text) throws Exception {
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        factory.setFeature("http://apache.org/xml/features/disallow-doctype-decl", true);
        factory.setFeature("http://xml.org/sax/features/external-general-entities", false);
        factory.setFeature("http://xml.org/sax/features/external-parameter-entities", false);
        Map<String, String> values = new HashMap<>();
        collectXml(factory.newDocumentBuilder().parse(new InputSource(new StringReader(text))).getDocumentElement(), "", new HashMap<>(), values);
        return values;
    }
    private static void collectXml(Element node, String parent, Map<String, Integer> siblings, Map<String, String> values) {
        String name = node.getLocalName(); int index = siblings.getOrDefault(name, 0); siblings.put(name, index + 1);
        String path = parent + "/" + name + "[" + index + "]";
        NamedNodeMap attributes = node.getAttributes();
        for (int i = 0; i < attributes.getLength(); i++) { Node attr = attributes.item(i); values.put(path + "|" + (attr.getNamespaceURI() == null ? "" : attr.getNamespaceURI()) + "|" + attr.getLocalName(), attr.getNodeValue()); }
        Map<String, Integer> children = new HashMap<>();
        for (Node child = node.getFirstChild(); child != null; child = child.getNextSibling()) if (child instanceof Element) collectXml((Element) child, path, children, values);
    }
    private static void preserveAttributes(JSONObject node, String path, Map<String, JSONObject> original, Map<String, String> before, Map<String, String> after, boolean collect) {
        JSONArray attributes = node.optJSONArray("attributes");
        if (attributes != null) for (int i = 0; i < attributes.length(); i++) {
            JSONObject attr = attributes.getJSONObject(i); String key = path + "|" + attr.optString("uri", "") + "|" + attr.getString("name");
            if (collect) original.put(key, attr);
            else if (original.containsKey(key) && before.containsKey(key) && before.get(key).equals(after.get(key))) {
                JSONObject old = original.get(key); for (String field : new String[]{"id", "value_type", "data"}) if (old.has(field)) attr.put(field, old.get(field));
            }
        }
        JSONArray nodes = node.optJSONArray("nodes"); Map<String, Integer> counts = new HashMap<>();
        if (nodes != null) for (int i = 0; i < nodes.length(); i++) {
            JSONObject child = nodes.getJSONObject(i); String name = child.optString("name", ""); if (name.isEmpty()) continue;
            int index = counts.getOrDefault(name, 0); counts.put(name, index + 1);
            preserveAttributes(child, path + "/" + name + "[" + index + "]", original, before, after, collect);
        }
    }
    public static void main(String[] args) {
        try {
            if (args.length != 5) throw new Exception("Expected mode, kind, source, document, output");
            String mode = args[0], kind = args[1];
            File source = new File(args[2]), document = new File(args[3]), output = new File(args[4]);
            if (!source.isFile() || source.length() < 8 || source.length() > LIMIT) throw new Exception("Invalid binary source size");
            boolean build = mode.equals("build");
            if (!build && !mode.equals("decode")) throw new Exception("Invalid mode");
            if ((build ? output : document).exists()) throw new Exception("Output already exists");
            String text;
            if (kind.equals("axml")) {
                ResXmlDocument original = new ResXmlDocument();
                original.readBytes(source);
                if (original.getDocumentElement() == null) throw new Exception("Missing XML root");
                if (!build) text = xml(original);
                else {
                    text = read(document);
                    if (text.contains("<!DOCTYPE") || text.contains("<!ENTITY")) throw new Exception("DTD and entities are not supported");
                    // Numeric resource references remain numeric; framework names resolve offline.
                    TableBlock context = new TableBlock();
                    context.addFramework(AndroidFrameworks.getLatest().getTableBlock());
                    PackageBlock pkg = context.newPackage(0x7f, "twinfile.editor");
                    ResXmlDocument edited = new ResXmlDocument();
                    edited.setPackageBlock(pkg);
                    edited.parse(XMLFactory.newPullParser(text));
                    if (edited.getDocumentElement() == null) throw new Exception("Missing XML root");
                    // Keep original typed values for untouched attributes, including numeric-looking strings.
                    Map<String, String> before = xmlAttributes(xml(original)), after = xmlAttributes(text);
                    Map<String, JSONObject> attributes = new HashMap<>();
                    preserveAttributes(original.toJson(), "", attributes, before, after, true);
                    JSONObject restored = edited.toJson(); preserveAttributes(restored, "", attributes, before, after, false);
                    edited = new ResXmlDocument(); edited.setPackageBlock(pkg); edited.fromJson(restored);
                    edited.refreshFull();
                    edited.writeBytes(output);
                    ResXmlDocument checked = new ResXmlDocument();
                    checked.readBytes(output);
                    if (!checked.toJson().similar(edited.toJson())) throw new Exception("Binary XML verification failed");
                    return;
                }
            } else if (kind.equals("arsc")) {
                TableBlock table = TableBlock.load(source);
                JSONArray baseline = resources(table);
                if (!build) text = baseline.toString(2);
                else {
                    JSONArray changes = new JSONArray(read(document));
                    if (changes.length() != baseline.length()) throw new Exception("Resource rows cannot be added or removed");
                    Iterator<Entry> entries = entries(table).iterator();
                    int index = 0;
                    while (entries.hasNext()) {
                        Entry entry = entries.next();
                        if (entry.isNull()) continue;
                        JSONObject before = baseline.getJSONObject(index), after = changes.getJSONObject(index++);
                        for (String key : new String[]{"id", "type", "name", "config"}) {
                            if (!before.get(key).equals(after.get(key))) throw new Exception("Resource identity changed: " + key);
                        }
                        JSONObject value = after.getJSONObject("value");
                        if (!before.getJSONObject("value").similar(value)) {
                            entry.fromJson(value);
                            if (entry.getResourceId() != before.getInt("id") || !entry.getName().equals(before.getString("name"))) throw new Exception("Resource ID/name cannot change");
                        }
                    }
                    table.refreshFull();
                    table.writeBytes(output);
                    if (!resources(TableBlock.load(output)).similar(changes)) throw new Exception("Resource round-trip verification failed");
                    return;
                }
            } else throw new Exception("Unknown format");
            byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
            if (bytes.length > LIMIT) throw new Exception("Decoded document exceeds 32 MiB");
            Files.write(document.toPath(), bytes);
        } catch (Throwable error) {
            System.err.println(error.getClass().getSimpleName() + ": " + error.getMessage());
            System.exit(1);
        }
    }
}
