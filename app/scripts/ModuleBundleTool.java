import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

/** Build-time preparation of the exact module shipped with the app. JDK only. */
public final class ModuleBundleTool {
    private static final long MAX_ARCHIVE = 64L * 1024 * 1024;
    private static final long MAX_EXPANDED = 128L * 1024 * 1024;
    private static final int MAX_ENTRIES = 4096;
    private static final Set<String> REQUIRED = Set.of(
        "module.prop", "customize.sh", "service.sh", "uninstall.sh", "root-watchdog.sh",
        "router-control.sh", "router-install.sh", "router-watchdog.sh", "router-config.example.json",
        "zygisk/arm64-v8a.so", "lib/arm64-v8a/libdejavu_agent.so", "lib/arm64-v8a/libdejavu.so",
        "lib/arm64-v8a/dejavu-root-helper", "bin/dejavuerouter", "config/init.lua", "config/targets.txt");

    private static Map<String, String> properties(String text) throws IOException {
        Map<String, String> result = new HashMap<>();
        for (String line : text.split("\\r?\\n")) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            int equals = line.indexOf('=');
            if (equals < 1) throw new IOException("Invalid module.prop line");
            String key = line.substring(0, equals).trim();
            String value = line.substring(equals + 1).trim();
            if (result.putIfAbsent(key, value) != null) throw new IOException("Duplicate module.prop key: " + key);
        }
        return result;
    }

    private static String safeName(String name) throws IOException {
        if (name.isEmpty() || name.startsWith("/") || name.contains("\\") || name.contains(":"))
            throw new IOException("Unsafe ZIP entry path");
        for (int i = 0; i < name.length(); ++i)
            if (Character.isISOControl(name.charAt(i))) throw new IOException("Control character in ZIP entry path");
        String normalized = name.endsWith("/") ? name.substring(0, name.length() - 1) : name;
        for (String component : normalized.split("/", -1))
            if (component.isEmpty() || component.equals(".") || component.equals(".."))
                throw new IOException("Unsafe ZIP entry path");
        return normalized;
    }

    private static void validate(Path archive, Map<String, String> expected) throws IOException {
        Map<String, Boolean> names = new HashMap<>();
        List<String> order = new ArrayList<>();
        try (ZipFile zip = new ZipFile(archive.toFile(), StandardCharsets.UTF_8)) {
            var entries = zip.entries();
            long declared = 0;
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                String name = safeName(entry.getName());
                if (names.putIfAbsent(name, entry.isDirectory()) != null)
                    throw new IOException("Duplicate ZIP entry: " + name);
                order.add(entry.getName());
                if (names.size() > MAX_ENTRIES || entry.getSize() < 0 || entry.getSize() > MAX_EXPANDED)
                    throw new IOException("Module ZIP entry limit exceeded");
                declared += entry.getSize();
                if (declared > MAX_EXPANDED) throw new IOException("Expanded module ZIP exceeds limit");
            }
        }
        for (String name : names.keySet()) {
            int slash = name.indexOf('/');
            while (slash >= 0) {
                if (Boolean.FALSE.equals(names.get(name.substring(0, slash))))
                    throw new IOException("ZIP file/directory collision");
                slash = name.indexOf('/', slash + 1);
            }
        }
        for (String required : REQUIRED)
            if (!Boolean.FALSE.equals(names.get(required))) throw new IOException("Missing module payload: " + required);

        // Read every local entry to verify CRCs, bounded decompression and agreement
        // with the central directory that the Android module installers consume.
        long expanded = 0;
        int index = 0;
        byte[] buffer = new byte[16384];
        try (ZipInputStream zip = new ZipInputStream(Files.newInputStream(archive), StandardCharsets.UTF_8)) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                if (index >= order.size() || !entry.getName().equals(order.get(index++)))
                    throw new IOException("ZIP local and central entries disagree");
                ByteArrayOutputStream prop = entry.getName().equals("module.prop") ? new ByteArrayOutputStream() : null;
                for (int count; (count = zip.read(buffer)) != -1;) {
                    expanded += count;
                    if (expanded > MAX_EXPANDED) throw new IOException("Expanded module ZIP exceeds limit");
                    if (prop != null) {
                        if (prop.size() + count > 65536) throw new IOException("module.prop exceeds limit");
                        prop.write(buffer, 0, count);
                    }
                }
                if (prop != null) {
                    Map<String, String> actual = properties(prop.toString(StandardCharsets.UTF_8));
                    for (String key : List.of("id", "version", "versionCode"))
                        if (!expected.get(key).equals(actual.get(key)))
                            throw new IOException("Module ZIP " + key + " does not match zygisk/module/module.prop");
                }
                zip.closeEntry();
            }
        }
        if (index != order.size()) throw new IOException("ZIP local and central entries disagree");
    }

    private static void deleteTree(Path path) throws IOException {
        if (!Files.exists(path, java.nio.file.LinkOption.NOFOLLOW_LINKS)) return;
        try (var paths = Files.walk(path)) {
            for (Path child : paths.sorted(java.util.Comparator.reverseOrder()).toList()) Files.delete(child);
        }
    }

    static void prepare(Path source, Path moduleProperties, Path output) throws Exception {
        if (!Files.isRegularFile(source))
            throw new IOException("Module ZIP missing: " + source + ". Run ./dejavu package first or set -PdejavuModuleZip=/path/to/module.zip");
        Map<String, String> expected = properties(Files.readString(moduleProperties));
        if (!"dejavu_zygisk".equals(expected.get("id")) ||
            !expected.getOrDefault("version", "").matches("v[0-9]+\\.[0-9]+\\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?"))
            throw new IOException("Invalid Dejavu module identity/version");
        int code = Integer.parseInt(expected.getOrDefault("versionCode", "0"));
        if (code <= 0) throw new IOException("Invalid module versionCode");
        Files.createDirectories(output.getParent());
        Path stage = Files.createTempDirectory(output.getParent(), ".dejavu-assets-");
        Path backup = null;
        try {
            Path assets = Files.createDirectory(stage.resolve("dejavu"));
            Path archive = assets.resolve("module.zip");
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            long size = 0;
            try (InputStream input = Files.newInputStream(source); var destination = Files.newOutputStream(archive)) {
                byte[] buffer = new byte[16384];
                for (int count; (count = input.read(buffer)) != -1;) {
                    size += count;
                    if (size > MAX_ARCHIVE) throw new IOException("Module ZIP exceeds 64 MiB");
                    digest.update(buffer, 0, count);
                    destination.write(buffer, 0, count);
                }
            }
            if (size == 0) throw new IOException("Empty module ZIP");
            validate(archive, expected);
            String hash = java.util.HexFormat.of().formatHex(digest.digest());
            String metadata = "{\"id\":\"dejavu_zygisk\",\"version\":\"" + expected.get("version") +
                "\",\"versionCode\":" + code + ",\"sha256\":\"" + hash + "\",\"size\":" + size + "}\n";
            Files.writeString(assets.resolve("module.json"), metadata, StandardCharsets.UTF_8);
            if (Files.exists(output)) {
                backup = Files.createTempDirectory(output.getParent(), ".dejavu-assets-old-");
                Files.delete(backup);
                Files.move(output, backup, StandardCopyOption.ATOMIC_MOVE);
            }
            try {
                Files.move(stage, output, StandardCopyOption.ATOMIC_MOVE);
            } catch (Exception failure) {
                if (backup != null) { Files.move(backup, output, StandardCopyOption.ATOMIC_MOVE); backup = null; }
                throw failure;
            }
            System.out.println("Prepared Dejavu " + expected.get("version") + " module assets (" + size + " bytes)");
        } finally {
            deleteTree(stage);
            if (backup != null) deleteTree(backup);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 3) throw new IllegalArgumentException("Usage: ModuleBundleTool ZIP MODULE_PROP OUTPUT_DIRECTORY");
        prepare(Path.of(args[0]).toAbsolutePath(), Path.of(args[1]).toAbsolutePath(), Path.of(args[2]).toAbsolutePath());
    }
}
