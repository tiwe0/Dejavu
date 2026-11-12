package dejavu.tests;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONObject;

/** Authenticates through the production RouterClient under the Manager's ordinary app UID. */
public final class RouterAndroidInstrumentation extends Instrumentation {
    private String metadataPath;
    private String endpointFixture;

    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        metadataPath = arguments == null ? null : arguments.getString("metadata_path");
        endpointFixture = arguments == null ? null : arguments.getString("endpoint_fixture");
        start();
    }

    @Override public void onStart() {
        Bundle results = new Bundle();
        int code = Activity.RESULT_CANCELED;
        try {
            String selectedPath = endpointFixture != null ? endpointFixture : metadataPath;
            if (selectedPath == null || !selectedPath.startsWith("/") || selectedPath.indexOf('\n') >= 0) {
                throw new AssertionError("Pass -e metadata_path <root metadata> or -e endpoint_fixture <private endpoint fixture>");
            }
            String context = new String(Files.readAllBytes(Paths.get("/proc/self/attr/current")), StandardCharsets.UTF_8).trim();
            int uid = android.os.Process.myUid();
            if (uid < 10000 || !context.contains(":untrusted_app")) {
                throw new AssertionError("Expected ordinary Manager UID and untrusted_app SELinux domain");
            }
            ClassLoader loader = getTargetContext().getClassLoader();
            Class<?> shellClass = Class.forName("cafe.ivory.dejavu.data.RootShell", true, loader);
            Object shell = shellClass.getField("INSTANCE").get(null);
            String quoted = (String) shellClass.getMethod("quote", String.class).invoke(shell, selectedPath);
            Class<?> function = Class.forName("kotlin.jvm.functions.Function1", true, loader);
            AtomicInteger reads = new AtomicInteger();
            Object reader = Proxy.newProxyInstance(loader, new Class<?>[] { function }, (proxy, method, args) -> {
                if (!method.getName().equals("invoke")) throw new AssertionError("Unexpected shell callback");
                if (!"cat /data/adb/dejavu/router/control.json".equals(args[0])) {
                    throw new AssertionError("Fixture permits endpoint discovery only");
                }
                reads.incrementAndGet();
                if (endpointFixture != null) {
                    byte[] bytes = Files.readAllBytes(Paths.get(endpointFixture));
                    if (bytes.length > 4096) throw new AssertionError("Endpoint fixture exceeds limit");
                    Class<?> resultClass = Class.forName("cafe.ivory.dejavu.data.ShellResult", true, loader);
                    return resultClass.getConstructor(int.class, String.class).newInstance(0, new String(bytes, StandardCharsets.UTF_8));
                }
                return shellClass.getMethod("run", String.class, long.class).invoke(shell, "cat " + quoted, 10L);
            });
            Class<?> clientClass = Class.forName("cafe.ivory.dejavu.data.RouterClient", true, loader);
            Object client = clientClass.getConstructor(function).newInstance(reader);
            try {
                Object first = clientClass.getMethod("request", String.class, JSONObject.class)
                        .invoke(client, "status", new JSONObject());
                Object second = clientClass.getMethod("request", String.class, JSONObject.class)
                        .invoke(client, "status", new JSONObject());
                int firstPid = ((Number) first.getClass().getMethod("getPid").invoke(first)).intValue();
                int secondPid = ((Number) second.getClass().getMethod("getPid").invoke(second)).intValue();
                if (firstPid <= 0 || firstPid != secondPid || reads.get() != 1) {
                    throw new AssertionError("Expected stable daemon status with one endpoint read");
                }
                results.putString("stream", "UID=" + uid + " SELinux=" + context + "\nRouter status twice; root peer verified; endpoint reads=1; PID=" + firstPid + "\n");
            } finally {
                clientClass.getMethod("cancelRead").invoke(client);
            }
            results.putString("result", "PASS");
            results.putString("router_status", "passed");
            results.putString("router_mode", endpointFixture != null ? "transport-only" : "root-discovery");
            code = Activity.RESULT_OK;
        } catch (Throwable failure) {
            while (failure instanceof InvocationTargetException && failure.getCause() != null) failure = failure.getCause();
            results.putString("result", "FAIL");
            results.putString("router_status", "failed");
            // Never print transport payloads, credentials or endpoint metadata.
            results.putString("stream", failure.getClass().getSimpleName() + ": " + failure.getMessage() + "\n");
        }
        finish(code, results);
    }
}
