package org.horocore.qualification;

import android.app.ActivityManager;
import android.content.ComponentCallbacks2;
import android.content.Context;
import android.os.PowerManager;
import android.util.Log;
import org.horocore.platform.HoroInputActivity;

/** The Android Vulkan profile observes platform conditions independently of frame attempts. */
public class HoroVulkanQualificationActivity extends HoroInputActivity {
    private static ThermalObservation thermal;
    private static final MemoryObservation memory = new MemoryObservation();
    private static final PowerSaveObservation power = new PowerSaveObservation();

    @Override public void onResume() {
        super.onResume();
        if (thermal == null) thermal = new ThermalObservation(getApplicationContext());
        thermal.resume(this);
        memory.resume(this);
        power.resume(this);
    }

    @Override public void onPause() {
        if (thermal != null) thermal.pause(this);
        memory.pause(this);
        power.pause(this);
        super.onPause();
    }

    @Override public void onDestroy() {
        if (thermal != null) thermal.pause(this);
        memory.pause(this);
        power.pause(this);
        super.onDestroy();
    }

    @Override public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        memory.trim(this, level);
    }

    /** Samples only at resume or from a severity-bearing callback on the main Activity lane. */
    private static final class MemoryObservation {
        private enum State {
            INACTIVE("inactive"), UNSUPPORTED("unsupported"), UNAVAILABLE("unavailable"),
            NORMAL("normal"), MODERATE("moderate"), LOW("low"), CRITICAL("critical");

            final String diagnostic;

            State(String diagnostic) {
                this.diagnostic = diagnostic;
            }
        }

        private Object owner;
        private State previous;
        private long revision;

        void resume(HoroVulkanQualificationActivity activity) {
            if (owner == activity) return;
            owner = activity;
            publish(sample(activity));
        }

        void pause(Object activity) {
            if (owner != activity) return;
            owner = null;
            publish(State.INACTIVE);
        }

        @SuppressWarnings("deprecation")
        void trim(Object activity, int level) {
            if (owner != activity || level < ComponentCallbacks2.TRIM_MEMORY_RUNNING_MODERATE ||
                    level >= ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN) return;
            if (level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL) {
                publish(State.CRITICAL);
            } else {
                publish(level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW ? State.LOW : State.MODERATE);
            }
        }

        private static State sample(Context context) {
            try {
                ActivityManager manager = context.getSystemService(ActivityManager.class);
                if (manager == null) return State.UNSUPPORTED;
                ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
                manager.getMemoryInfo(info);
                return info.lowMemory ? State.LOW : State.NORMAL;
            } catch (RuntimeException unavailable) {
                return State.UNAVAILABLE;
            }
        }

        private void publish(State state) {
            if (state == previous || revision == Long.MAX_VALUE) return;
            previous = state;
            Log.i("HoroPackageQualification", "host-memory revision=" + ++revision + " state=" + state.diagnostic);
        }
    }

    /** Lifecycle, listener callbacks and revision publication are serialized by the main executor. */
    private static final class ThermalObservation {
        private enum State {
            PENDING("pending"), INACTIVE("inactive"), UNSUPPORTED("unsupported"), UNAVAILABLE("unavailable"),
            NONE("none"), LIGHT("light"), MODERATE("moderate"), SEVERE("severe"),
            CRITICAL("critical"), EMERGENCY("emergency"), SHUTDOWN("shutdown");

            final String diagnostic;

            State(String diagnostic) {
                this.diagnostic = diagnostic;
            }

            static State fromAndroid(int status) {
                switch (status) {
                    case PowerManager.THERMAL_STATUS_NONE: return NONE;
                    case PowerManager.THERMAL_STATUS_LIGHT: return LIGHT;
                    case PowerManager.THERMAL_STATUS_MODERATE: return MODERATE;
                    case PowerManager.THERMAL_STATUS_SEVERE: return SEVERE;
                    case PowerManager.THERMAL_STATUS_CRITICAL: return CRITICAL;
                    case PowerManager.THERMAL_STATUS_EMERGENCY: return EMERGENCY;
                    case PowerManager.THERMAL_STATUS_SHUTDOWN: return SHUTDOWN;
                    default: return UNAVAILABLE;
                }
            }
        }

        private final Context application;
        private Object owner;
        private PowerManager manager;
        private Listener listener;
        private boolean accepting;
        private State previous;
        private long revision;

        ThermalObservation(Context application) {
            this.application = application;
        }

        void resume(Object activity) {
            if (owner == activity) return;
            owner = activity;
            accepting = false;
            if (!retireListener()) {
                publish(State.UNAVAILABLE);
                return;
            }
            try {
                PowerManager service = application.getSystemService(PowerManager.class);
                if (service == null) {
                    publish(State.UNSUPPORTED);
                    return;
                }
                Listener candidate = new Listener();
                service.addThermalStatusListener(application.getMainExecutor(), candidate);
                manager = service;
                listener = candidate;
                accepting = true;
                publish(State.PENDING);
            } catch (RuntimeException unavailable) {
                publish(State.UNAVAILABLE);
            }
        }

        void pause(Object activity) {
            if (owner != activity) return;
            owner = null;
            accepting = false;
            retireListener();
            publish(State.INACTIVE);
        }

        /** A failed unregister blocks another registration until the exact old listener can retire. */
        private boolean retireListener() {
            if (listener == null) return true;
            try {
                manager.removeThermalStatusListener(listener);
            } catch (RuntimeException unavailable) {
                return false;
            }
            listener = null;
            manager = null;
            return true;
        }

        private void publish(State state) {
            if (state == previous || revision == Long.MAX_VALUE) return;
            previous = state;
            Log.i("HoroPackageQualification", "host-thermal revision=" + ++revision + " state=" + state.diagnostic);
        }

        private final class Listener implements PowerManager.OnThermalStatusChangedListener {
            @Override public void onThermalStatusChanged(int status) {
                if (accepting && listener == this) publish(State.fromAndroid(status));
            }
        }
    }
}
