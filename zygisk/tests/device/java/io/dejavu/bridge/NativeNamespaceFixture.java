package io.dejavu.bridge;

/** Load through java.load_dex; keep that loader handle until the process exits. */
public final class NativeNamespaceFixture {
    private static boolean loaded;

    private NativeNamespaceFixture() {}

    public static synchronized void load(String absolutePath) {
        if (!loaded) {
            // System.load attributes the native library to this class's loader.
            System.load(absolutePath);
            loaded = true;
        }
    }

    public static native long address();
    public static native long call(long value);
    public static native int protect(int protection);
    public static native int permissions();
}
