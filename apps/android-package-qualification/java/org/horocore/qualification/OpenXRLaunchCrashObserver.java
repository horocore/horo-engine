package org.horocore.qualification;

import android.util.Log;
import java.util.concurrent.atomic.AtomicBoolean;

/** Records a qualification-only Java crash marker and delegates Android's normal crash handling. */
public final class OpenXRLaunchCrashObserver {
    private OpenXRLaunchCrashObserver() {}

    public static void install(String nonce, String startTicks) {
        Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        if (previous == null) return;
        String marker = "host-launch-crash nonce=" + nonce + " startTicks=" + startTicks;
        AtomicBoolean recorded = new AtomicBoolean();
        try {
            Thread.setDefaultUncaughtExceptionHandler((thread, failure) -> {
                try {
                    if (recorded.compareAndSet(false, true)) {
                        Log.i("HoroPackageQualification", marker);
                    }
                } finally {
                    previous.uncaughtException(thread, failure);
                }
            });
        } catch (SecurityException unavailable) {
            // The qualification observer is optional.
        }
    }
}
