package org.horocore.qualification;

/** Keeps one foreground owner's memory sample pending on the Activity's main lane. */
final class MemoryObservation<T> {
    private static final long SAMPLE_INTERVAL_MS = 5000;

    enum State { INACTIVE, UNSUPPORTED, UNAVAILABLE, NORMAL, MODERATE, LOW, CRITICAL }

    /** Samples and publishes on the main lane. Scheduling queues a later callback on that same lane. */
    interface Port<T> {
        State sample(T owner);
        boolean schedule(Runnable action, long delayMs);
        void cancel(Runnable action);
        void pace(T owner, boolean conserve);
        void report(long revision, State state);
    }

    private final Port<T> port;
    private final boolean pollWhileNormal;
    private T owner;
    private SampleTask pending;
    private State previous;
    private long revision;
    private boolean conserving;
    private boolean samplingSupported;

    MemoryObservation(Port<T> port, boolean pollWhileNormal) {
        this.port = port;
        this.pollWhileNormal = pollWhileNormal;
    }

    void resume(T activity) {
        if (activity == null || owner == activity) return;
        if (owner != null) pause(owner);
        owner = activity;
        samplingSupported = true;
        sample();
    }

    void pause(T activity) {
        if (owner == null || owner != activity) return;
        SampleTask retired = pending;
        pending = null;
        if (retired != null) port.cancel(retired);
        port.pace(owner, false);
        owner = null;
        conserving = false;
        publish(State.INACTIVE);
    }

    void trim(T activity, State state) {
        if (owner == null || owner != activity) return;
        publish(state);
        scheduleSample();
    }

    private void sample() {
        State measured = port.sample(owner);
        // System MemoryInfo reports only low/normal. Preserve a more severe running trim until recovery.
        publish(measured == State.LOW && previous == State.CRITICAL ? State.CRITICAL : measured);
        samplingSupported = measured != State.UNSUPPORTED;
        scheduleSample();
    }

    private void scheduleSample() {
        if (pending != null || !samplingSupported || (!pollWhileNormal && !conserving)) return;
        pending = new SampleTask();
        if (!port.schedule(pending, SAMPLE_INTERVAL_MS)) {
            pending = null;
            publish(State.UNAVAILABLE);
        }
    }

    private void publish(State state) {
        if (state == State.LOW || state == State.CRITICAL) setConserving(true);
        if (state == State.NORMAL) setConserving(false);
        if (state == previous) return;
        previous = state;
        if (revision != Long.MAX_VALUE) port.report(++revision, state);
    }

    private void setConserving(boolean next) {
        if (conserving == next) return;
        conserving = next;
        port.pace(owner, next);
    }

    private final class SampleTask implements Runnable {
        @Override public void run() {
            if (pending != this) return;
            pending = null;
            sample();
        }
    }
}
