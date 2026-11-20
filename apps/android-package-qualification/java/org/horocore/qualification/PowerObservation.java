package org.horocore.qualification;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.util.Log;

/** Foreground power and battery observations share the Activity's one main-lane receiver. */
final class PowerObservation {
    private enum State {
        INACTIVE("inactive"), UNSUPPORTED("unsupported"), UNAVAILABLE("unavailable"), ENABLED("enabled"), DISABLED("disabled");

        final String diagnostic;

        State(String diagnostic) {
            this.diagnostic = diagnostic;
        }
    }

    private enum BatteryState {
        INACTIVE("inactive"), UNSUPPORTED("unsupported"), UNAVAILABLE("unavailable"),
        CHARGING("charging"), DISCHARGING("discharging"), NOT_CHARGING("not-charging"), FULL("full");

        final String diagnostic;

        BatteryState(String diagnostic) {
            this.diagnostic = diagnostic;
        }

        static BatteryState fromAndroid(int status) {
            switch (status) {
                case BatteryManager.BATTERY_STATUS_CHARGING: return CHARGING;
                case BatteryManager.BATTERY_STATUS_DISCHARGING: return DISCHARGING;
                case BatteryManager.BATTERY_STATUS_NOT_CHARGING: return NOT_CHARGING;
                case BatteryManager.BATTERY_STATUS_FULL: return FULL;
                default: return UNAVAILABLE;
            }
        }
    }

    private Context owner;
    private Receiver retained;
    private boolean accepting;
    private State previous;
    private long revision;
    private BatteryState previousBattery;
    private int previousPercent;
    private long batteryRevision;

    void resume(Context activity) {
        if (owner == activity) return;
        owner = activity;
        accepting = false;
        if (!retireReceiver()) {
            publish(State.UNAVAILABLE);
            publishBattery(BatteryState.UNAVAILABLE, -1);
            return;
        }
        try {
            Context application = activity.getApplicationContext();
            Receiver candidate = new Receiver(application);
            IntentFilter filter = new IntentFilter(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED);
            filter.addAction(Intent.ACTION_BATTERY_CHANGED);
            Intent battery;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                battery = application.registerReceiver(candidate, filter, Context.RECEIVER_NOT_EXPORTED);
            } else {
                battery = application.registerReceiver(candidate, filter);
            }
            retained = candidate;
            accepting = true;
            samplePower(application);
            sampleBattery(battery);
        } catch (RuntimeException unavailable) {
            publish(State.UNAVAILABLE);
            publishBattery(BatteryState.UNAVAILABLE, -1);
        }
    }

    void pause(Context activity) {
        if (owner != activity) return;
        owner = null;
        accepting = false;
        retireReceiver();
        publish(State.INACTIVE);
        publishBattery(BatteryState.INACTIVE, -1);
    }

    /** Retains failed cleanup so the next Activity cannot register a second receiver. */
    private boolean retireReceiver() {
        if (retained == null) return true;
        try {
            retained.application.unregisterReceiver(retained);
        } catch (IllegalArgumentException alreadyDetached) {
            // The application context has already removed this exact registration.
        } catch (RuntimeException unavailable) {
            return false;
        }
        retained = null;
        return true;
    }

    private void samplePower(Context application) {
        try {
            PowerManager manager = application.getSystemService(PowerManager.class);
            if (manager == null) {
                publish(State.UNSUPPORTED);
                return;
            }
            publish(manager.isPowerSaveMode() ? State.ENABLED : State.DISABLED);
        } catch (RuntimeException unavailable) {
            publish(State.UNAVAILABLE);
        }
    }

    @SuppressWarnings("deprecation")
    private void sampleBattery(Intent intent) {
        try {
            Bundle extras = intent != null && Intent.ACTION_BATTERY_CHANGED.equals(intent.getAction()) ? intent.getExtras() : null;
            Object presence = extras == null ? null : extras.get(BatteryManager.EXTRA_PRESENT);
            if (!(presence instanceof Boolean)) {
                publishBattery(BatteryState.UNAVAILABLE, -1);
                return;
            }
            if (!((Boolean) presence)) {
                publishBattery(BatteryState.UNSUPPORTED, -1);
                return;
            }
            int level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            int scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 0);
            BatteryState state = BatteryState.fromAndroid(intent.getIntExtra(BatteryManager.EXTRA_STATUS, 0));
            if (scale <= 0 || level < 0 || level > scale || state == BatteryState.UNAVAILABLE) {
                publishBattery(BatteryState.UNAVAILABLE, -1);
                return;
            }
            publishBattery(state, (int) ((100L * level + scale / 2) / scale));
        } catch (RuntimeException unavailable) {
            publishBattery(BatteryState.UNAVAILABLE, -1);
        }
    }

    private void publish(State state) {
        if (state == previous || revision == Long.MAX_VALUE) return;
        previous = state;
        Log.i("HoroPackageQualification", "host-power revision=" + ++revision + " state=" + state.diagnostic);
    }

    private void publishBattery(BatteryState state, int percent) {
        if ((state == previousBattery && percent == previousPercent) || batteryRevision == Long.MAX_VALUE) return;
        previousBattery = state;
        previousPercent = percent;
        Log.i("HoroPackageQualification",
                "host-battery revision=" + ++batteryRevision + " state=" + state.diagnostic + " percent=" + percent);
    }

    private final class Receiver extends BroadcastReceiver {
        final Context application;

        Receiver(Context application) {
            this.application = application;
        }

        @Override public void onReceive(Context context, Intent intent) {
            if (!accepting || retained != this || intent == null) return;
            String action = intent.getAction();
            if (PowerManager.ACTION_POWER_SAVE_MODE_CHANGED.equals(action)) samplePower(application);
            if (Intent.ACTION_BATTERY_CHANGED.equals(action)) sampleBattery(intent);
        }
    }
}
