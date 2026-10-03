#pragma once

/** @file AudioBackend.h
 * @brief Non-installed, equal-peer backend protocol for Audio control and native output adapters.
 */

#include "Horo/Audio/AudioBackendCapabilities.h"
#include "Horo/Audio/AudioCallbackEvents.h"
#include "Horo/Audio/AudioDeviceNegotiation.h"
#include "Horo/Audio/AudioPlanarBlock.h"

namespace Horo::Audio::Backend {
    struct AudioCallbackViolation;
    struct AudioCallbackViolationDrain;

    /** @brief Runtime-local, non-wrapping operation identity; sequence zero is invalid. */
    struct OperationId final {
        AudioRuntimeId owner;
        std::uint64_t sequence{};
        bool operator==(const OperationId &) const noexcept = default;
    };

    /** @brief Requested access mode; unsupported exclusive mode must not silently become shared. */
    enum class AccessMode : std::uint8_t {
        Shared,
        Exclusive
    };
    /** @brief Preserved host policy hint; explicit format/period bounds remain the admission authority. */
    enum class LatencyClass : std::uint8_t {
        Interactive,
        Balanced,
        PowerSaving
    };
    /** @brief Control-published intent for the currently retained render epoch. */
    enum class RenderPhase : std::uint8_t {
        Priming,
        Rendering,
        Quiescing
    };
    /** @brief Render-core response; Ready/Quiesced are facts, not runtime state commits. */
    enum class RenderDisposition : std::uint8_t {
        Rendered,
        Ready,
        Quiesced,
        Fault
    };

    /** @brief Borrowed data for one native-triggered Horo processing call. */
    struct RenderInvocation final {
        AudioDeviceEpoch epoch;
        RenderPhase phase{RenderPhase::Priming};
        AudioMonotonicTimestamp startedAt;
        std::uint64_t sampleFrame{};
        AudioPlanarBlockView output;
    };

    /** @brief Default failure is safe; successful responses use fault None and write the entire valid output. */
    struct RenderResult final {
        RenderDisposition disposition{RenderDisposition::Fault};
        AudioCallbackFaultCode fault{AudioCallbackFaultCode::InvalidEpoch};
    };

    /**
     * @brief Internal Horo render-core port, never an application, scene, editor or arbitrary user callback.
     *
     * The host retains context, code and epoch storage until native detachment and the required
     * quiescence proof. process cannot allocate/free, block, log, query configuration/ECS, or retain
     * invocation views. Priming/Quiescing output is preallocated positive-zero silence until the
     * corresponding core acknowledgement. Backend forwards intent; it never chooses runtime policy.
     * Native adapter owns FP environment, conversion and final safety clamp. Fault output is replaced
     * with preallocated silence and the first fault is latched for control.
     */
    struct RenderPort final {
        void *context{}; /**< Horo-owned retained render state, never an exposed native device handle. */
        RenderResult (*process)(void *, const RenderInvocation &) noexcept {};
    };

    /** @brief Queries native availability without selecting or activating an output device. */
    struct Probe final {};

    /** @brief Requests a bounded owned device catalog with stable Horo identities. */
    struct Enumerate final {};

    /** @brief Bounded owned open policy; planned identity is reserved by control before native acquisition. */
    struct Open final {
        AudioDeviceEpoch plannedEpoch;
        AudioDeviceFormatRequest format;
        AccessMode access{AccessMode::Shared};
        LatencyClass latency{LatencyClass::Balanced};
    };

    /** @brief Installs a retained render port for the validated opened epoch, initially in Priming. */
    struct Start final {
        AudioDeviceEpoch epoch;
        RenderPort render;
    };

    /** @brief Publishes Quiescing intent; completion requires the matching render-core acknowledgement. */
    struct Quiesce final {
        AudioDeviceEpoch epoch;
    };

    /** @brief Stops/unregisters native callbacks and joins relevant native work; success proves no further entry. */
    struct Stop final {
        AudioDeviceEpoch epoch;
    };

    /** @brief Releases already detached native state; never bypasses an unproven stop. */
    struct Close final {};

    /** @brief Closed set of owned asynchronous control requests; copying may allocate only on control. */
    using Request = std::variant<Probe, Enumerate, Open, Start, Quiesce, Stop, Close>;

    /** @brief Resource disposition after cancellation/failure; Retained forbids destruction or library unload. */
    enum class ResourceDisposition : std::uint8_t {
        Unchanged,
        Closed,
        CallbackDetached,
        Retained
    };

    /** @brief Opened candidate facts, still awaiting host format/capability admission and callback readiness. */
    struct Opened final {
        AudioNegotiatedDeviceFormat format;
        AudioDeviceTimingReport timing;
        AccessMode access{AccessMode::Shared};
    };

    /** @brief Native start succeeded, not Runtime Active. */
    struct Started final {
        AudioDeviceEpoch epoch;
    };

    /** @brief Core acknowledgement, not native detachment. */
    struct Quiesced final {
        AudioDeviceEpoch epoch;
    };

    /** @brief Native callback entry is impossible for this epoch. */
    struct Stopped final {
        AudioDeviceEpoch epoch;
    };

    /** @brief Native state is closed and no callback or worker can access it. */
    struct Closed final {};

    /** @brief Cancelled operation; Start cancellation must unwind/detach acquired callback state before this result. */
    struct Cancelled final {
        ResourceDisposition resources{ResourceDisposition::Unchanged};
    };

    /** @brief Stable Audio error with preserved cause and explicit resource retention requirements. */
    struct Failed final {
        Error error;
        ResourceDisposition resources{ResourceDisposition::Retained};
    };

    /** @brief Exactly one terminal outcome per accepted operation, retained unchanged until acknowledgement. */
    struct Completion final {
        OperationId operation;
        std::variant<AudioBackendProbe, AudioDeviceSnapshot, Opened, Started, Quiesced, Stopped, Closed, Cancelled, Failed> outcome;
    };
    /** @brief Cancellation request admission, not evidence that native cleanup completed. */
    enum class CancelDisposition : std::uint8_t {
        Requested,
        AlreadyTerminal
    };

    /** @brief Invalidates control's device catalog at a non-zero discovery revision. */
    struct DevicesChanged final {
        std::uint64_t discoveryRevision{};
    };

    /** @brief A role default changed independently of the currently selected output. */
    struct DefaultChanged final {
        std::uint64_t discoveryRevision{};
        AudioDefaultDeviceRole role{AudioDefaultDeviceRole::Multimedia};
        std::optional<AudioDeviceId> device; /**< No replacement default may be available. */
    };
    /** @brief Backend-neutral reason the selected device no longer satisfies its opened contract. */
    enum class DeviceLossCause : std::uint8_t {
        Disconnected,
        ServiceRestart,
        FormatChanged,
        PermissionRevoked,
        BackendFailure
    };

    /** @brief Loss fact for the exact device generation, never an instruction to choose a replacement. */
    struct DeviceLost final {
        AudioDeviceId device;
        DeviceLossCause cause{DeviceLossCause::BackendFailure};
    };
    /** @brief Native interruption edge; an end does not authorize automatic resume. */
    enum class InterruptionState : std::uint8_t {
        Began,
        Ended
    };

    /** @brief Interruption fact reconciled against control-owned suspend/recovery policy. */
    struct DeviceInterruption final {
        AudioDeviceId device;
        InterruptionState state{InterruptionState::Began};
    };

    /** @brief Bounded native/backend facts; control alone decides recovery, suspend or explicit device replacement. */
    struct Event final {
        AudioRuntimeId owner;
        std::variant<DevicesChanged, DefaultChanged, DeviceLost, DeviceInterruption, AudioCallbackEvent> fact;
    };

    /**
     * @brief Non-installed asynchronous output adapter contract; all concrete peers implement it directly.
     *
     * Every method except the supplied RenderPort runs on one declared control owner thread.
     * Native apartments, loops, workers, devices and conversion memory stay private. Construction
     * and factory registration are inert: no native discovery, service connection or ambient registration.
     * Identity is fixed for this runtime; no method may select another backend or silently fall back.
     * Backend-specific worker affinity and bounded cancellation guarantees must be documented by each peer.
     *
     * At most one operation may be pending or retain an unacknowledged completion. Native callbacks
     * continue independently; polling or an observer cannot block callback progression. Critical callback
     * facts are retained in preallocated storage, not dropped on saturation. Catalog invalidation may
     * coalesce only to an observable latest revision; device loss/interruption cannot disappear silently.
     *
     * @pre Destruction requires closed native state, joined native work, acknowledged operations and
     * drained lifecycle facts. If stop/cancellation cannot prove safety by its deadline, report Failed
     * with Retained. Host preserves the entire ownership island under ADR-062 fatal retention rather
     * than invoking this destructor, freeing epoch memory or unloading backend code.
     */
    class AudioBackend {
    public:
        virtual ~AudioBackend() = default;

        /** @brief Returns the inert fixed peer identity. @return Selected Horo backend kind. */
        [[nodiscard]] virtual AudioBackendKind Kind() const noexcept = 0;
        /** @brief Returns the fixed runtime generation. @return Non-zero Horo runtime owner. */
        [[nodiscard]] virtual AudioRuntimeId Owner() const noexcept = 0;

        /**
         * @brief Accepts a bounded owned request without waiting for native completion.
         * @param request Valid typed operation; backend owns a complete copy after successful admission.
         * @param deadline Non-zero-domain monotonic deadline; backend maps its native timers to this domain.
         * @return Non-wrapping owner-scoped operation ID, or rejection without new native side effects.
         *
         * Reject unknown enums, malformed/foreign epochs, expired deadlines, unavailable required features
         * and illegal transitions with stable Audio errors. Reject new work while the completion slot is
         * occupied. Open validates discovery identity and reserves resources but installs no callback;
         * exact access mode is required. Opened format device/revision and timing epoch match plannedEpoch;
         * the host admits the reported candidate before Start. Epochs and operation IDs are never reused.
         * Start requires admitted format, render.process != nullptr, and any context required by
         * process retained through quiescence and native detachment.
         * Quiesce forwards control intent until core acknowledgement; Stop success proves native detachment.
         * Close follows Stop (or an open that never started). Repeated Quiesce/Stop/Close return the recorded
         * safe outcome without repeating native destruction. Partial acquisition failures unwind in reverse
         * order; unproven cleanup reports Retained, never a false Closed/Stopped outcome.
         */
        [[nodiscard]] virtual Result<OperationId> Begin(const Request &request, const AudioMonotonicTimestamp &deadline) = 0;

        /**
         * @brief Publishes Rendering intent inside the host's matching ready/activation transaction.
         * @param epoch Exact opened/primed epoch whose Ready fact control already validated.
         * @return Success or stale/invalid-transition error; never activates the parent runtime itself.
         */
        [[nodiscard]] virtual Result<void> CommitRendering(const AudioDeviceEpoch &epoch) = 0;

        /**
         * @brief Requests cancellation of Probe, Enumerate, Open or Start without blocking.
         * @param operation Exact owner-scoped accepted ID.
         * @return Requested or AlreadyTerminal, or unsupported/identity failure. Quiesce/Stop/Close are not cancellable.
         *
         * Cancellation does not release caller-retained callback state. Poll must reach a terminal result;
         * Start cancellation after native entry requires detachment before Cancelled. Failed/Retained
         * denotes unproven cleanup and invokes host fatal-retention policy, not normal destruction.
         */
        [[nodiscard]] virtual Result<CancelDisposition> Cancel(const OperationId &operation) = 0;

        /**
         * @brief Reads operation progress without waiting or consuming its terminal record.
         * @param operation Exact accepted ID.
         * @return Empty optional while pending; stable completion thereafter, or a query-identity error.
         * @post Repeated polling returns the same logical outcome until AcknowledgeCompletion.
         */
        [[nodiscard]] virtual Result<std::optional<Completion>> Poll(const OperationId &operation) = 0;

        /**
         * @brief Releases an observed terminal slot after control reconciles its outcome and resources.
         * @param operation Exact completed ID; pending or foreign IDs fail without freeing the slot.
         * @return Success or stable identity/state error. Acknowledgement is not a native-detachment proof.
         */
        [[nodiscard]] virtual Result<void> AcknowledgeCompletion(const OperationId &operation) = 0;

        /**
         * @brief Copies a bounded prefix of owned events without allocation or callbacks to user code.
         * @param output Caller-owned storage; at most 64 records are written per call.
         * @return Number of records copied, never exceeding output.size() or 64.
         * @post Copied critical facts are acknowledged to the transport; uncopied critical facts remain
         * retained. Old generations are reported faithfully, not relabeled as the current epoch.
         */
        [[nodiscard]] virtual std::size_t DrainEvents(std::span<Event> output) noexcept = 0;

        /**
         * @brief Drain development callback safety facts on the control thread, never on the callback.
         * @param output Caller-owned fixed storage for a bounded prefix.
         * @return Copied count and saturation/rate-limiting totals since the prior drain.
         */
        [[nodiscard]] virtual AudioCallbackViolationDrain DrainSafetyViolations(std::span<AudioCallbackViolation> output) noexcept = 0;
    };
}  // namespace Horo::Audio::Backend
