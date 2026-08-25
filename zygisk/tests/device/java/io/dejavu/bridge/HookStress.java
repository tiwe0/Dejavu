package io.dejavu.bridge;

import java.lang.reflect.InvocationTargetException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;

public final class HookStress {
    private static int stressTarget(int value) {
        return value + 1;
    }

    private static int protocolTarget(int value) {
        return value + 1;
    }

    private static String stringTarget(String value) {
        return "orig:" + value;
    }

    private static Object objectTarget(Object value) {
        return value;
    }

    private static int callTarget(int value) {
        return value + 3;
    }

    private static int exceptionTarget(int value) {
        if (value < 0) {
            throw new IllegalStateException("dejavu exception test");
        }
        return value + 1;
    }

    public static int runProtocolTest() {
        int failures = 0;
        if (protocolTarget(7) != 12) {
            ++failures;
        }
        if (protocolTarget(9) != 101) {
            ++failures;
        }
        return failures;
    }

    public static int runHelperTest() {
        int failures = 0;
        if (!"after".equals(stringTarget("input"))) {
            ++failures;
        }
        if (!"orig:null".equals(stringTarget(null))) {
            ++failures;
        }
        Object value = new String("object");
        if (objectTarget(value) != value) {
            ++failures;
        }
        if (objectTarget(null) != null) {
            ++failures;
        }
        return failures;
    }

    public static int runExceptionTest() {
        int failures = 0;
        try {
            if (exceptionTarget(7) != 8) {
                ++failures;
            }
        } catch (RuntimeException exception) {
            ++failures;
        }
        try {
            exceptionTarget(-1);
            ++failures;
        } catch (Throwable expected) {
            // Method.invoke wraps the original exception; its cause must survive.
            if (!(expected instanceof InvocationTargetException) ||
                    !(((InvocationTargetException) expected).getCause()
                            instanceof IllegalStateException)) {
                ++failures;
            }
        }
        return failures;
    }

    public static int runStress(int threadCount, int iterations) {
        AtomicInteger failures = new AtomicInteger();
        CountDownLatch ready = new CountDownLatch(threadCount);
        CountDownLatch start = new CountDownLatch(1);
        Thread[] threads = new Thread[threadCount];
        for (int thread = 0; thread < threadCount; ++thread) {
            threads[thread] = new Thread(new Runnable() {
                @Override
                public void run() {
                    ready.countDown();
                    try {
                        start.await();
                        for (int iteration = 0; iteration < iterations; ++iteration) {
                            if (stressTarget(iteration) != iteration + 1) {
                                failures.incrementAndGet();
                            }
                            if ((iteration & 255) == 0) {
                                Thread.yield();
                            }
                        }
                    } catch (InterruptedException exception) {
                        Thread.currentThread().interrupt();
                        failures.incrementAndGet();
                    }
                }
            }, "dejavu-stress-" + thread);
            threads[thread].start();
        }
        try {
            ready.await();
            start.countDown();
            for (Thread thread : threads) {
                thread.join();
            }
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
            failures.incrementAndGet();
        }
        return failures.get();
    }
}
