package io.dejavu.bridge;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;

public final class HookStress {
    private static int stressTarget(int value) {
        return value + 1;
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
