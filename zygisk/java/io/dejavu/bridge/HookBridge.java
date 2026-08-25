package io.dejavu.bridge;

public final class HookBridge {
    private final long token;

    public HookBridge(long token) {
        this.token = token;
    }

    public native Object dispatch(Object[] args);
}
