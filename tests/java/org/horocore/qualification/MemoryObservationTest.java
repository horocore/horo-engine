package org.horocore.qualification;

import org.horocore.qualification.MemoryObservation.State;

/** Exercises the production observation policy without Android or a wall clock. */
public final class MemoryObservationTest {
    private static final long INTERVAL_MS = 5_000;
    private static final long HOUR_MS = 60 * 60 * 1_000;

    private static final class Owner {
        State memory = State.NORMAL;
        int samples;
        boolean conserving;
    }

    private static final class VirtualPort implements MemoryObservation.Port<Owner> {
        long now;
        long deadline;
        Runnable pending;
        State reportedState;
        long reportedRevision;
        boolean schedulerAvailable = true;

        @Override public State sample(Owner owner) {
            ++owner.samples;
            return owner.memory;
        }

        @Override public boolean schedule(Runnable action, long delayMs) {
            check(pending == null, "Only one foreground sample may be scheduled");
            check(delayMs == INTERVAL_MS, "Unexpected sample interval");
            if (!schedulerAvailable) return false;
            pending = action;
            deadline = now + delayMs;
            return true;
        }

        @Override public void cancel(Runnable action) {
            check(pending == action, "Cancellation must target the exact pending sample");
            pending = null;
        }

        @Override public void pace(Owner owner, boolean conserve) {
            owner.conserving = conserve;
        }

        @Override public void report(long revision, State state) {
            check(revision > reportedRevision, "Observation revisions must increase");
            reportedRevision = revision;
            reportedState = state;
        }

        void advanceBy(long elapsedMs) {
            long target = now + elapsedMs;
            while (pending != null && deadline <= target) {
                now = deadline;
                Runnable due = pending;
                pending = null;
                due.run();
            }
            now = target;
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    /** An initially healthy foreground owner can enter and recover from pressure without trim callbacks. */
    private static void observeLongForegroundSession() {
        VirtualPort port = new VirtualPort();
        MemoryObservation<Owner> observation = new MemoryObservation<>(port, true);
        Owner owner = new Owner();
        observation.resume(owner);
        check(owner.samples == 1 && port.reportedState == State.NORMAL, "Resume must sample immediately");
        long initialRevision = port.reportedRevision;
        port.advanceBy(2 * HOUR_MS);
        check(owner.samples == 1 + 2 * HOUR_MS / INTERVAL_MS, "Healthy sessions must keep sampling at a bounded cadence");
        check(!owner.conserving && port.reportedRevision == initialRevision,
                "Unchanged normal samples must not emit transitions");

        owner.memory = State.LOW;
        port.advanceBy(INTERVAL_MS);
        check(owner.conserving && port.reportedState == State.LOW,
                "Foreground sampling must detect low memory without trim");
        observation.trim(owner, State.CRITICAL);
        port.advanceBy(HOUR_MS);
        check(owner.conserving && port.reportedState == State.CRITICAL,
                "A low sample must not downgrade the critical trim state");

        owner.memory = State.UNAVAILABLE;
        port.advanceBy(INTERVAL_MS);
        check(owner.conserving && port.reportedState == State.UNAVAILABLE,
                "An unavailable sample must retain conservation and report its state");
        owner.memory = State.NORMAL;
        port.advanceBy(INTERVAL_MS - 1);
        check(owner.conserving, "Recovery must wait for the next scheduled observation");
        port.advanceBy(1);
        check(!owner.conserving && port.reportedState == State.NORMAL, "An observed recovery must clear conservation");
    }

    /** A callback from a retired schedule cannot observe or rearm the current owner. */
    private static void fenceRetiredSchedules() {
        VirtualPort port = new VirtualPort();
        MemoryObservation<Owner> observation = new MemoryObservation<>(port, true);
        Owner first = new Owner();
        Owner replacement = new Owner();
        first.memory = State.LOW;
        observation.resume(first);
        Runnable pausedSample = port.pending;
        check(pausedSample != null && first.conserving, "Foreground pressure must start conservation and schedule recovery");
        observation.pause(first);
        check(!first.conserving && port.pending == null && port.reportedState == State.INACTIVE,
                "Pause must retire pacing and sampling");
        port.advanceBy(HOUR_MS);
        check(first.samples == 1, "Paused owners must not be sampled");

        observation.resume(first);
        Runnable resumedSample = port.pending;
        pausedSample.run();
        check(first.samples == 2 && port.pending == resumedSample, "A reused owner must reject its previous schedule");

        observation.resume(replacement);
        Runnable replacementSample = port.pending;
        check(!first.conserving && replacement.samples == 1,
                "Replacement must retire the old owner and sample the new owner");
        resumedSample.run();
        observation.pause(first);
        observation.trim(first, State.CRITICAL);
        check(first.samples == 2 && replacement.samples == 1 && port.pending == replacementSample,
                "Old callbacks must leave the replacement untouched");
        port.advanceBy(INTERVAL_MS);
        check(replacement.samples == 2 && port.reportedState == State.NORMAL, "Replacement must continue its own sampling");
        observation.pause(replacement);
    }

    /** A missing capability stops polling for this admission, while transient failure can recover. */
    private static void distinguishUnsupportedFromUnavailable() {
        VirtualPort port = new VirtualPort();
        MemoryObservation<Owner> observation = new MemoryObservation<>(port, true);
        Owner owner = new Owner();
        owner.memory = State.UNAVAILABLE;
        observation.resume(owner);
        check(port.reportedState == State.UNAVAILABLE && port.pending != null, "Unavailable service must be retried");

        owner.memory = State.UNSUPPORTED;
        port.advanceBy(INTERVAL_MS);
        check(port.reportedState == State.UNSUPPORTED && port.pending == null,
                "Unsupported service must stop foreground sampling");
        observation.trim(owner, State.CRITICAL);
        check(owner.conserving && port.reportedState == State.CRITICAL && port.pending == null,
                "A legacy trim may pace conservatively without restarting an unsupported sampler");
        port.advanceBy(HOUR_MS);
        check(owner.samples == 2, "Unsupported owners must not be sampled again until readmitted");

        observation.pause(owner);
        owner.memory = State.NORMAL;
        observation.resume(owner);
        check(owner.samples == 3 && port.reportedState == State.NORMAL && port.pending != null,
                "Resume must recheck capability");
        observation.pause(owner);
    }

    private static void reportSchedulerRejection() {
        VirtualPort port = new VirtualPort();
        port.schedulerAvailable = false;
        MemoryObservation<Owner> observation = new MemoryObservation<>(port, true);
        Owner owner = new Owner();
        observation.resume(owner);
        check(owner.samples == 1 && port.reportedState == State.UNAVAILABLE && port.pending == null,
                "A rejected schedule must report unavailable without retaining a timer");
        observation.pause(owner);
    }

    /** Legacy running trim callbacks start recovery checks without polling healthy devices. */
    private static void useLegacyCallbacksWhileNormal() {
        VirtualPort port = new VirtualPort();
        MemoryObservation<Owner> observation = new MemoryObservation<>(port, false);
        Owner owner = new Owner();
        observation.resume(owner);
        port.advanceBy(HOUR_MS);
        check(owner.samples == 1 && port.pending == null, "A healthy legacy owner should rely on trim callbacks");

        owner.memory = State.LOW;
        observation.trim(owner, State.LOW);
        check(owner.conserving && port.pending != null, "A legacy trim must request conservation and schedule recovery");
        port.advanceBy(INTERVAL_MS);
        check(owner.samples == 2 && owner.conserving, "A low sample must retain legacy conservation");
        owner.memory = State.NORMAL;
        port.advanceBy(INTERVAL_MS);
        check(owner.samples == 3 && !owner.conserving && port.pending == null, "Legacy recovery must stop polling");
        observation.pause(owner);
    }

    public static void main(String[] args) {
        observeLongForegroundSession();
        useLegacyCallbacksWhileNormal();
        fenceRetiredSchedules();
        distinguishUnsupportedFromUnavailable();
        reportSchedulerRejection();
    }

    private MemoryObservationTest() {}
}
