package io.dejavu.bridge;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class HookBridge {
    private final long token;

    public HookBridge(long token) {
        this.token = token;
    }

    public native Object dispatch(Object[] args);

    private static Object invokeOriginal(Method backup, Object receiver, Object[] args)
            throws Throwable {
        try {
            return backup.invoke(receiver, args);
        } catch (InvocationTargetException wrapper) {
            // Strip only the wrapper added by this reflective invocation.
            Throwable original = wrapper.getCause();
            throw original != null ? original : wrapper;
        }
    }
}
