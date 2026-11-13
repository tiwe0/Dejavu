package dejavu.tests;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;

/** Regular test UID; no root, injection, target changes or business data. */
public final class JniThreadScopeInstrumentation extends Instrumentation {
    static { System.loadLibrary("dejavu_jni_scope_test"); }
    private static native String runScopeChecks();

    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        start();
    }

    @Override public void onStart() {
        Bundle result = new Bundle();
        int code = Activity.RESULT_CANCELED;
        try {
            result.putString("stream", runScopeChecks() + "\n");
            result.putString("result", "PASS");
            code = Activity.RESULT_OK;
        } catch (Throwable failure) {
            result.putString("result", "FAIL");
            result.putString("stream", failure.getClass().getSimpleName() + "\n");
        }
        finish(code, result);
    }
}
