package org.horocore.platform;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/** Private GameActivity/JNI producer. The synchronized peer gate drains callbacks before native destruction. */
public class HoroInputActivity extends com.google.androidgamesdk.GameActivity implements InputManager.InputDeviceListener {
    private static final int RECOGNITION_REQUEST = 4107;
    private long nativePeer;
    private InputManager inputManager;
    private boolean registered;
    private boolean requesting;
    private boolean wasGranted;
    private native void horoInputEvent(long peer, int kind, int value);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
    }
    /** Called on the native owner thread after JNI registration; never stores a VM/native object in engine APIs. */
    public final synchronized void horoBindInput(long peer) {
        if (nativePeer != 0) throw new IllegalStateException("Android input already bound");
        nativePeer = peer;
        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(this, new Handler(Looper.getMainLooper()));
            registered = true;
        }
        publishPermission();
    }
    /** Zero the peer before unregistering; even an unregister failure cannot leave a stale native pointer. */
    public final synchronized void horoDetachInput(long peer) {
        if (peer != nativePeer) return;
        nativePeer = 0;
        if (registered) {
            registered = false;
            inputManager.unregisterInputDeviceListener(this);
        }
    }
    /** Explicit host/user request only; ordinary sampling does not prompt. */
    public final void horoRequestActivityRecognition() {
        runOnUiThread(() -> {
            synchronized (HoroInputActivity.this) {
                if (nativePeer == 0 || requesting) return;
                if (checkSelfPermission(Manifest.permission.ACTIVITY_RECOGNITION) == PackageManager.PERMISSION_GRANTED) {
                    publishPermission(); return;
                }
                getPreferences(MODE_PRIVATE).edit().putBoolean("horo.activityRecognitionAsked", true).apply();
                requesting = true;
                publishPermission();
                requestPermissions(new String[]{Manifest.permission.ACTIVITY_RECOGNITION}, RECOGNITION_REQUEST);
            }
        });
    }
    private void publishPermission() {
        if (nativePeer == 0) return;
        final boolean granted = checkSelfPermission(Manifest.permission.ACTIVITY_RECOGNITION) == PackageManager.PERMISSION_GRANTED;
        final boolean asked = getPreferences(MODE_PRIVATE).getBoolean("horo.activityRecognitionAsked", false);
        final int state = permissionState(granted, asked);
        wasGranted = granted;
        horoInputEvent(nativePeer, 1, state);
    }
    /** Preserve request/grant/revocation precedence when Android returns from settings or a permission dialog. */
    private int permissionState(boolean granted, boolean asked) {
        if (requesting) return 1;
        if (granted) return 2;
        if (wasGranted) return 5;
        if (!asked) return 0;
        if (shouldShowRequestPermissionRationale(Manifest.permission.ACTIVITY_RECOGNITION)) return 3;
        return 4;
    }
    @Override public void onResume() {
        super.onResume();
        synchronized (this) { publishPermission(); }
    }
    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] grants) {
        super.onRequestPermissionsResult(request, permissions, grants);
        if (request == RECOGNITION_REQUEST) {
            synchronized (this) { requesting = false; publishPermission(); }
        }
    }
    @Override public synchronized void onInputDeviceAdded(int device) {
        // Canonical admission occurs lazily on the first physical event, after owner-thread focus checks.
    }
    @Override public synchronized void onInputDeviceChanged(int device) {
        if (nativePeer != 0) horoInputEvent(nativePeer, 2, device);
    }
    @Override public synchronized void onInputDeviceRemoved(int device) {
        if (nativePeer != 0) horoInputEvent(nativePeer, 2, device);
    }
    @Override public void onDestroy() {
        synchronized (this) { horoDetachInput(nativePeer); }
        super.onDestroy();
    }
}
