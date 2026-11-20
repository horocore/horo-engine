package org.horocore.qualification;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.PowerManager;
import android.util.Log;

/** One foreground power-saver observation on the Activity's main lane. */
final class PowerSaveObservation {
    private enum State {
        INACTIVE("inactive"), UNSUPPORTED("unsupported"), UNAVAILABLE("unavailable"), ENABLED("enabled"), DISABLED("disabled");

        final String diagnostic;

        State(String diagnostic) {
            this.diagnostic = diagnostic;
        }
    }

    private Context owner;
    private Receiver retained;
    private boolean accepting;
    private State previous;
    private long revision;

    void resume(Context activity) {
        if (owner == activity) return;
        owner = activity;
        accepting = false;
        if (!retireReceiver()) {
            publish(State.UNAVAILABLE);
            return;
        }
        try {
            Context application = activity.getApplicationContext();
            PowerManager manager = application.getSystemService(PowerManager.class);
            if (manager == null) {
                publish(State.UNSUPPORTED);
                return;
            }
            Receiver candidate = new Receiver(application, manager);
            IntentFilter filter = new IntentFilter(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                application.registerReceiver(candidate, filter, Context.RECEIVER_NOT_EXPORTED);
            } else {
                application.registerReceiver(candidate, filter);
            }
            retained = candidate;
            accepting = true;
            sample(manager);
        } catch (RuntimeException unavailable) {
            publish(State.UNAVAILABLE);
        }
    }

    void pause(Context activity) {
        if (owner != activity) return;
        owner = null;
        accepting = false;
        retireReceiver();
        publish(State.INACTIVE);
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

    private void sample(PowerManager manager) {
        try {
            publish(manager.isPowerSaveMode() ? State.ENABLED : State.DISABLED);
        } catch (RuntimeException unavailable) {
            publish(State.UNAVAILABLE);
        }
    }

    private void publish(State state) {
        if (state == previous || revision == Long.MAX_VALUE) return;
        previous = state;
        Log.i("HoroPackageQualification", "host-power revision=" + ++revision + " state=" + state.diagnostic);
    }

    private final class Receiver extends BroadcastReceiver {
        final Context application;
        final PowerManager manager;

        Receiver(Context application, PowerManager manager) {
            this.application = application;
            this.manager = manager;
        }

        @Override public void onReceive(Context context, Intent intent) {
            if (accepting && retained == this && intent != null &&
                    PowerManager.ACTION_POWER_SAVE_MODE_CHANGED.equals(intent.getAction())) {
                sample(manager);
            }
        }
    }
}
