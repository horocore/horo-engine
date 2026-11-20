package org.horocore.qualification;

import android.content.Context;
import android.os.PowerManager;
import android.util.Log;
import org.horocore.platform.HoroInputActivity;

/** The Android Vulkan profile observes platform thermal status independently of frame attempts. */
public class HoroVulkanQualificationActivity extends HoroInputActivity {
    private static ThermalObservation thermal;

    @Override public void onResume() {
        super.onResume();
        if (thermal == null) thermal = new ThermalObservation(getApplicationContext());
        thermal.resume(this);
    }

    @Override public void onPause() {
        if (thermal != null) thermal.pause(this);
        super.onPause();
    }

    @Override public void onDestroy() {
        if (thermal != null) thermal.pause(this);
        super.onDestroy();
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
