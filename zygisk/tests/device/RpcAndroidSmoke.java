package dejavu.tests;

import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import cafe.ivory.dejavu.data.DejavuRpc;
import cafe.ivory.dejavu.data.RpcDeadlineKt;
import cafe.ivory.dejavu.data.RootShell;
import cafe.ivory.dejavu.data.ShellResult;
import java.net.SocketTimeoutException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import kotlin.Unit;

/**
 * Run under su with app_process and the built Manager APK on CLASSPATH.
 * run-as may not have permission to invoke su on rooted devices. All data is synthetic.
 */
public final class RpcAndroidSmoke {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void blockedSocketDeadline() throws Exception {
        String name = "dejavu-rpc-deadline-" + android.os.Process.myPid();
        LocalServerSocket server = new LocalServerSocket(name);
        CountDownLatch accepted = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        AtomicReference<Throwable> peerError = new AtomicReference<>();
        Thread peer = new Thread(() -> {
            try (LocalSocket remote = server.accept()) {
                accepted.countDown();
                release.await(3, TimeUnit.SECONDS);
            } catch (Throwable error) {
                peerError.set(error);
            }
        });
        peer.setDaemon(true);
        peer.start();
        LocalSocket client = new LocalSocket();
        try {
            client.getInputStream();
            client.setSoTimeout(2000);
            client.connect(new LocalSocketAddress(name, LocalSocketAddress.Namespace.ABSTRACT));
            require(accepted.await(2, TimeUnit.SECONDS), "peer did not accept");
            long started = System.nanoTime();
            boolean timedOut = false;
            try {
                RpcDeadlineKt.withRpcDeadline(80L, () -> {
                    try { client.shutdownInput(); } catch (Exception ignored) {}
                    try { client.shutdownOutput(); } catch (Exception ignored) {}
                    try { client.close(); } catch (Exception ignored) {}
                    return Unit.INSTANCE;
                }, () -> {
                    try { return client.getInputStream().read(); }
                    catch (Exception error) { throw new RuntimeException(error); }
                });
            } catch (Exception error) {
                timedOut = error instanceof SocketTimeoutException;
                if (!timedOut) throw error;
            }
            require(timedOut, "blocked Android read escaped deadline");
            require(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started) < 1500,
                    "Android shutdown did not promptly interrupt read");
        } finally {
            release.countDown();
            client.close();
            server.close();
            peer.join(2000);
        }
        require(!peer.isAlive(), "socket peer thread leaked");
        require(peerError.get() == null, "socket peer failed: " + peerError.get());
        System.out.println("OK: real Android LocalSocket shutdown interrupts a blocked read");
    }

    public static void main(String[] args) {
        try {
            run(args);
        } catch (Throwable error) {
            error.printStackTrace(System.err);
            System.exit(1);
        }
    }

    static void run(String[] args) throws Exception {
        String process = args.length == 0 ? "com.android.settings" : args[0];
        Process plain = new ProcessBuilder("/system/bin/sh", "-c", "echo SUBPROCESS_OK").start();
        require(plain.waitFor(3, TimeUnit.SECONDS) && plain.exitValue() == 0, "plain subprocess failed");
        ShellResult root = RootShell.INSTANCE.run("echo ROOT_SMOKE", 3L);
        require(root.getExitCode() == 0, "root bridge failed: " + root.getOutput());
        System.out.println("OK: App root-shell bridge");
        blockedSocketDeadline();
        DejavuRpc rpc = DejavuRpc.INSTANCE;
        require("ANDROID_RPC_OK".equals(rpc.execute(process, "return 'ANDROID_RPC_OK'")),
                "Android RPC transport failed");
        StringBuilder controls = new StringBuilder();
        for (int i = 0; i < 32; ++i) controls.append((char) i);
        String value = controls.append((char) 127).append("0123'\"\\中文😀").toString();
        require(value.equals(rpc.execute(process, "return " + rpc.luaString(value))),
                "Android/Lua string roundtrip changed control or UTF-8 bytes");
        String wait = "local h=hook.stream_listen('127.0.0.1:18776',1); "
                + "assert(type(h)=='number' and h>0); "
                + "local ok,res=pcall(hook.stream_accept,h,10000); "
                + "if ok and type(res)=='number' and res>0 then hook.stream_close(res) end; "
                + "hook.stream_close(h); assert(ok,tostring(res)); assert(res==0); return 'WAIT_OK'";
        long started = System.nanoTime();
        require("WAIT_OK".equals(rpc.execute(process, wait)), "Android supported wait failed");
        require(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started) >= 9000,
                "Android wait returned before the intended accept timeout");
        System.out.println("OK: App RPC root handshake, control/Unicode roundtrip, 10-second wait");
    }
}
