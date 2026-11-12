package dejavu.tests;

import android.app.Activity;
import android.app.Application;
import android.app.Instrumentation;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.concurrent.Executors;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicBoolean;

/** Drives the production VM as the real Manager UID without starting an Activity. */
public final class ModuleAndroidInstrumentation extends Instrumentation {
    private static final long TOTAL_MILLIS = 120_000;
    private String socketName;
    private volatile String phase = "setup";
    private volatile LocalServerSocket server;
    private volatile LocalSocket client;
    private long deadline;
    private Object viewModel;
    private Object viewModelStore;
    private Object stateFlow;
    private Method flowValue;
    private Method setVisible;
    private Method refresh;
    private Method install;
    private Method clearStore;

    private interface CheckedAction { void run() throws Exception; }
    private interface StateCondition { boolean matches(Object state) throws Exception; }
    private static final class TestFailure extends Exception {
        private static final long serialVersionUID = 1L;
        TestFailure(String message) { super(message); }
    }

    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        socketName = arguments == null ? null : arguments.getString("control_socket");
        start();
    }

    @Override public void onStart() {
        deadline = SystemClock.elapsedRealtime() + TOTAL_MILLIS;
        ScheduledExecutorService timer = Executors.newSingleThreadScheduledExecutor(action -> {
            Thread thread = new Thread(action, "DejavuModuleTestDeadline");
            thread.setDaemon(true);
            return thread;
        });
        timer.schedule(this::closeSockets, TOTAL_MILLIS, TimeUnit.MILLISECONDS);
        Bundle results = new Bundle();
        Throwable failure = null;
        int uid = android.os.Process.myUid();
        try {
            require(socketName != null && socketName.matches("[A-Za-z0-9_.-]{1,100}"),
                    "Pass a unique abstract control_socket name");
            String context = new String(Files.readAllBytes(Paths.get("/proc/self/attr/current")),
                    StandardCharsets.UTF_8).trim();
            require(uid >= 10000 && context.contains(":untrusted_app"),
                    "Expected the ordinary Manager UID and untrusted_app domain");
            server = new LocalServerSocket(socketName);
            phase = "create_vm";
            createViewModel();
            onMain(() -> {
                setVisible.invoke(viewModel, true);
                refresh.invoke(viewModel);
            });
            client = acceptController();
            client.setSoTimeout(60_000);

            phase = "busy";
            Object busy = awaitState(40_000, state -> !flag(state, "getLoading") &&
                    snapshotFlag(state, "getInstallationRunning"));
            require(!flag(busy, "getCanInstall"), "External root lock must disable installation");
            send("BUSY");
            expect("RELEASED");

            // Do not refresh manually: visibility polling must recover from the
            // external installer's lock using the production VM's own behavior.
            phase = "automatic_ready";
            Object ready = awaitState(40_000, state -> !flag(state, "getLoading") &&
                    !snapshotFlag(state, "getInstallationRunning") && flag(state, "getCanInstall"));
            Object snapshot = property(ready, "getSnapshot");
            require("APATCH".equals(String.valueOf(property(snapshot, "getProvider"))),
                    "Expected the authorized APatch installer");
            send("READY");
            expect("INSTALL");

            phase = "install";
            onMain(() -> install.invoke(viewModel));
            awaitState(90_000, state -> !flag(state, "getInstalling") &&
                    !flag(state, "getLoading") && flag(state, "getInstallSucceeded") &&
                    snapshotFlag(state, "getRebootPending"));
            send("PENDING");
            expect("CANCELLED");

            phase = "cancelled_refresh";
            onMain(() -> refresh.invoke(viewModel));
            awaitState(40_000, state -> !flag(state, "getLoading") && !flag(state, "getInstalling") &&
                    !snapshotFlag(state, "getRebootPending") && !flag(state, "getInstallSucceeded") &&
                    flag(state, "getCanInstall"));
            phase = "cleanup";
        } catch (Throwable error) {
            failure = unwrap(error);
        } finally {
            try {
                cleanupViewModel();
            } catch (Throwable cleanup) {
                if (failure == null) failure = unwrap(cleanup);
            }
            if (failure == null) {
                try {
                    send("PASS");
                    results.putString("result", "PASS");
                    results.putString("module_status", "passed");
                    results.putString("stream", "UID=" + uid +
                            " production ModuleViewModel: external busy, automatic ready, installation pending reboot, cancelled stage refresh\n");
                } catch (Throwable completion) {
                    failure = unwrap(completion);
                }
            }
            if (failure != null) {
                // State flags and our own test reasons are safe diagnostics. Do
                // not expose VM output/error strings, root output or metadata.
                String reason = failure instanceof TestFailure
                        ? failure.getMessage() : failure.getClass().getSimpleName();
                String diagnostic = (phase + ": " + reason + "; " + stateSummary()).replace('\n', ' ')
                        .replace('\r', ' ');
                if (diagnostic.length() > 768) diagnostic = diagnostic.substring(0, 768);
                try { send("ERROR " + diagnostic); } catch (Exception ignored) { }
                results.putString("result", "FAIL");
                results.putString("module_status", "failed");
                results.putString("stream", diagnostic + "\n");
            }
            closeSockets();
            timer.shutdownNow();
            finish(failure == null ? Activity.RESULT_OK : Activity.RESULT_CANCELED, results);
        }
    }

    private void createViewModel() throws Exception {
        ClassLoader loader = getTargetContext().getClassLoader();
        Class<?> vmClass = Class.forName("cafe.ivory.dejavu.data.ModuleViewModel", true, loader);
        Class<?> storeClass = Class.forName("androidx.lifecycle.ViewModelStore", true, loader);
        Class<?> baseClass = Class.forName("androidx.lifecycle.ViewModel", true, loader);
        Method put = storeClass.getMethod("put", String.class, baseClass);
        clearStore = storeClass.getMethod("clear");
        setVisible = vmClass.getMethod("setVisible", boolean.class);
        refresh = vmClass.getMethod("refresh");
        install = vmClass.getMethod("install");
        Method state = vmClass.getMethod("getState");
        flowValue = Class.forName("kotlinx.coroutines.flow.StateFlow", true, loader).getMethod("getValue");
        Application application = (Application) getTargetContext().getApplicationContext();
        onMain(() -> {
            viewModelStore = storeClass.getConstructor().newInstance();
            viewModel = vmClass.getConstructor(Application.class).newInstance(application);
            put.invoke(viewModelStore, "dejavu-module-instrumentation", viewModel);
            stateFlow = state.invoke(viewModel);
        });
    }

    private void onMain(CheckedAction action) throws Exception {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        AtomicBoolean cancelled = new AtomicBoolean();
        CountDownLatch completed = new CountDownLatch(1);
        Handler handler = new Handler(Looper.getMainLooper());
        Runnable operation = () -> {
            try {
                if (!cancelled.get()) action.run();
            } catch (Throwable error) {
                failure.set(error);
            } finally {
                completed.countDown();
            }
        };
        require(handler.post(operation), "The main looper rejected an instrumentation action");
        try {
            require(completed.await(Math.max(1, deadline - SystemClock.elapsedRealtime()), TimeUnit.MILLISECONDS),
                    "The main-thread action deadline exceeded");
        } catch (Exception error) {
            // Never let a queued install run later after the host has abandoned
            // this test and begun restoring its staged module transaction.
            cancelled.set(true);
            handler.removeCallbacks(operation);
            throw error;
        }
        Throwable error = failure.get();
        if (error instanceof Exception) throw (Exception) error;
        if (error != null) throw new AssertionError("Main-thread instrumentation action failed", error);
    }

    private void cleanupViewModel() throws Exception {
        if (viewModel == null && viewModelStore == null) return;
        onMain(() -> {
            try {
                if (viewModel != null && setVisible != null) setVisible.invoke(viewModel, false);
            } finally {
                if (viewModelStore != null && clearStore != null) clearStore.invoke(viewModelStore);
            }
        });
    }

    private LocalSocket acceptController() throws Exception {
        while (SystemClock.elapsedRealtime() < deadline) {
            LocalSocket connection = server.accept();
            int peer = connection.getPeerCredentials().getUid();
            if (peer == 0 || peer == 2000 || peer == android.os.Process.myUid()) return connection;
            connection.close();
        }
        throw new TestFailure("Controller connection deadline exceeded");
    }

    private Object awaitState(long phaseMillis, StateCondition condition) throws Exception {
        long until = Math.min(deadline, SystemClock.elapsedRealtime() + phaseMillis);
        while (SystemClock.elapsedRealtime() < until) {
            Object state = state();
            if (!flag(state, "getLoading") && !flag(state, "getInstalling")) {
                if (property(state, "getError") != null) throw new TestFailure("The production VM reported an error");
                Object exit = property(state, "getFailureExitCode");
                if (exit != null) throw new TestFailure("The production installer failed with exit " + exit);
            }
            if (condition.matches(state)) return state;
            Thread.sleep(80);
        }
        throw new TestFailure("Production VM state deadline exceeded");
    }

    private Object state() throws Exception {
        if (stateFlow == null || flowValue == null) throw new TestFailure("Production state flow is unavailable");
        return flowValue.invoke(stateFlow);
    }

    private static Object property(Object owner, String method) throws Exception {
        if (owner == null) return null;
        return owner.getClass().getMethod(method).invoke(owner);
    }

    private static boolean flag(Object owner, String method) throws Exception {
        return Boolean.TRUE.equals(property(owner, method));
    }

    private static boolean snapshotFlag(Object state, String method) throws Exception {
        return flag(property(state, "getSnapshot"), method);
    }

    private String stateSummary() {
        try {
            Object state = state();
            return "loading=" + flag(state, "getLoading") + ", installing=" + flag(state, "getInstalling") +
                    ", canInstall=" + flag(state, "getCanInstall") + ", success=" + flag(state, "getInstallSucceeded") +
                    ", pending=" + snapshotFlag(state, "getRebootPending") +
                    ", external=" + snapshotFlag(state, "getInstallationRunning") +
                    ", errorPresent=" + (property(state, "getError") != null) +
                    ", exit=" + property(state, "getFailureExitCode");
        } catch (Exception ignored) {
            return "state=unavailable";
        }
    }

    private void send(String line) throws IOException {
        if (client == null) throw new IOException("Controller is not connected");
        OutputStream output = client.getOutputStream();
        output.write((line + "\n").getBytes(StandardCharsets.UTF_8));
        output.flush();
    }

    private void expect(String command) throws Exception {
        long remaining = deadline - SystemClock.elapsedRealtime();
        require(remaining > 0, "Control deadline exceeded");
        client.setSoTimeout((int) Math.min(60_000, remaining));
        InputStream input = client.getInputStream();
        StringBuilder line = new StringBuilder();
        while (line.length() < 128) {
            int value = input.read();
            if (value < 0) throw new TestFailure("Controller disconnected before phase acknowledgement");
            if (value == '\n') {
                String received = line.toString();
                if (received.endsWith("\r")) received = received.substring(0, received.length() - 1);
                require(command.equals(received), "Unexpected controller acknowledgement");
                return;
            }
            require(value == '\r' || value >= 32 && value <= 126, "Invalid controller command encoding");
            line.append((char) value);
        }
        throw new TestFailure("Controller command exceeded its size limit");
    }

    private static void require(boolean condition, String reason) throws TestFailure {
        if (!condition) throw new TestFailure(reason);
    }

    private static Throwable unwrap(Throwable error) {
        while (error instanceof InvocationTargetException && error.getCause() != null) error = error.getCause();
        return error;
    }

    private void closeSockets() {
        LocalSocket connection = client;
        if (connection != null) try { connection.close(); } catch (IOException ignored) { }
        LocalServerSocket listener = server;
        if (listener != null) try { listener.close(); } catch (IOException ignored) { }
    }
}
