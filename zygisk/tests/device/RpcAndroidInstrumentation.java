package dejavu.tests;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import java.io.ByteArrayOutputStream;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;

/** Runs the transport fixture inside the Manager's ordinary application process. */
public final class RpcAndroidInstrumentation extends Instrumentation {
    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        start();
    }

    @Override public void onStart() {
        Bundle results = new Bundle();
        ByteArrayOutputStream captured = new ByteArrayOutputStream();
        PrintStream previous = System.out;
        PrintStream output = new PrintStream(captured);
        int result = Activity.RESULT_CANCELED;
        try {
            System.setOut(output);
            String context = new String(Files.readAllBytes(Paths.get("/proc/self/attr/current")),
                    StandardCharsets.UTF_8).trim();
            System.out.println("UID=" + android.os.Process.myUid() + " SELinux=" + context);
            if (android.os.Process.myUid() < 10000 || !context.contains(":untrusted_app")) {
                throw new AssertionError("Expected an ordinary application UID and SELinux domain");
            }
            RpcAndroidSmoke.run(new String[] {"com.android.settings"});
            results.putString("result", "PASS");
            result = Activity.RESULT_OK;
        } catch (Throwable error) {
            error.printStackTrace(output);
            results.putString("result", "FAIL");
        } finally {
            output.flush();
            System.setOut(previous);
            results.putString("stream", new String(captured.toByteArray(), StandardCharsets.UTF_8));
            finish(result, results);
        }
    }
}
