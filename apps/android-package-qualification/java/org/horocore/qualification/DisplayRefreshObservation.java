package org.horocore.qualification;

import android.app.Activity;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.Window;

/** Samples only the display attached to the resumed Activity's exact focused window. */
final class DisplayRefreshObservation {
    private enum State {
        INACTIVE("inactive"), UNSUPPORTED("unsupported"), UNAVAILABLE("unavailable"), OBSERVED("observed");

        final String diagnostic;

        State(String diagnostic) {
            this.diagnostic = diagnostic;
        }
    }

    private enum Change { ADDED, CHANGED, REMOVED }

    private Activity owner;
    private Listener retained;
    private View decor;
    private IBinder windowToken;
    private State previous;
    private int previousRate;
    private long revision;

    void resume(Activity activity) {
        if (owner == activity) return;
        owner = activity;
        decor = null;
        windowToken = null;
        focus(activity, activity.hasWindowFocus());
    }

    void pause(Activity activity) {
        if (owner != activity) return;
        owner = null;
        stop(State.INACTIVE);
    }

    void focus(Activity activity, boolean focused) {
        if (owner != activity) return;
        if (!focused) {
            stop(State.INACTIVE);
            return;
        }
        View view = decorOf(activity);
        IBinder token = view == null ? null : view.getWindowToken();
        if (token == null) {
            stop(State.UNAVAILABLE);
            return;
        }
        if (view != decor || token != windowToken) {
            if (!retireListener()) {
                publish(State.UNAVAILABLE, 0);
                return;
            }
            if (!registerListener(activity, view, token)) return;
        }
        sample(Display.INVALID_DISPLAY, Change.CHANGED);
    }

    private static View decorOf(Activity activity) {
        if (activity == null) return null;
        Window window = activity.getWindow();
        return window == null ? null : window.peekDecorView();
    }

    private boolean registerListener(Activity activity, View view, IBinder token) {
        try {
            DisplayManager manager = activity.getApplicationContext().getSystemService(DisplayManager.class);
            if (manager == null) {
                publish(State.UNSUPPORTED, 0);
                return false;
            }
            Listener listener = new Listener(manager);
            manager.registerDisplayListener(listener, new Handler(Looper.getMainLooper()));
            retained = listener;
            decor = view;
            windowToken = token;
            return true;
        } catch (RuntimeException unavailable) {
            publish(State.UNAVAILABLE, 0);
            return false;
        }
    }

    private void stop(State state) {
        retireListener();
        publish(state, 0);
    }

    /** Clears the window borrow before native cleanup, and retains failed cleanup for retry. */
    private boolean retireListener() {
        decor = null;
        windowToken = null;
        if (retained == null) return true;
        try {
            retained.manager.unregisterDisplayListener(retained);
        } catch (RuntimeException unavailable) {
            return false;
        }
        retained = null;
        return true;
    }

    private void sample(int changedId, Change change) {
        if (decor == null || owner == null) return;
        if (!owner.hasWindowFocus()) {
            stop(State.INACTIVE);
            return;
        }
        if (decorOf(owner) != decor || decor.getWindowToken() != windowToken) {
            stop(State.UNAVAILABLE);
            return;
        }
        try {
            if (change == Change.REMOVED && changedId == retained.attachedId) retained.removedId = changedId;
            Display display = decor.getDisplay();
            if (display == null) {
                publish(State.UNAVAILABLE, 0);
                return;
            }
            int id = display.getDisplayId();
            if (changedId != Display.INVALID_DISPLAY && changedId != id) return;
            retained.attachedId = id;
            if (change == Change.ADDED) retained.removedId = Display.INVALID_DISPLAY;
            if (change == Change.REMOVED) retained.removedId = id;
            if (!display.isValid() || id == Display.INVALID_DISPLAY || retained.removedId == id) {
                publish(State.UNAVAILABLE, 0);
                return;
            }
            publishRate(display.getRefreshRate());
        } catch (RuntimeException unavailable) {
            publish(State.UNAVAILABLE, 0);
        }
    }

    private void publishRate(float hertz) {
        double milliHz = hertz * 1000.0;
        if (!Double.isFinite(milliHz) || milliHz < 1 || milliHz > Integer.MAX_VALUE) {
            publish(State.UNAVAILABLE, 0);
            return;
        }
        publish(State.OBSERVED, (int) Math.round(milliHz));
    }

    private void publish(State state, int milliHz) {
        if ((state == previous && milliHz == previousRate) || revision == Long.MAX_VALUE) return;
        previous = state;
        previousRate = milliHz;
        Log.i("HoroPackageQualification",
                "host-refresh revision=" + ++revision + " state=" + state.diagnostic + " milliHz=" + milliHz);
    }

    private final class Listener implements DisplayManager.DisplayListener {
        final DisplayManager manager;
        int attachedId = Display.INVALID_DISPLAY;
        int removedId = Display.INVALID_DISPLAY;

        Listener(DisplayManager manager) {
            this.manager = manager;
        }

        private void notifyChange(int id, Change change) {
            if (retained == this) sample(id, change);
        }

        @Override public void onDisplayAdded(int id) { notifyChange(id, Change.ADDED); }
        @Override public void onDisplayChanged(int id) { notifyChange(id, Change.CHANGED); }
        @Override public void onDisplayRemoved(int id) { notifyChange(id, Change.REMOVED); }
    }
}
