#pragma once

/** @file MixerGraphCompiler.h
 * @brief Detached mixer compilation, fixed callback execution and acknowledged generation retirement.
 */

#include "Horo/Audio/AudioCommandStaging.h"
#include "Horo/Audio/AudioDSPNode.h"
#include "Horo/Audio/MixerAssetSchema.h"
#include "Horo/Audio/MixerSnapshot.h"

#include <memory>
#include <optional>

namespace Horo::Audio {
    /** @brief Pinned build identity; revisions and generations are nonzero and never wrap. */
    struct MixerPlanIdentity final {
        AudioRuntimeId owner;
        std::uint64_t epoch{};
        std::uint64_t generation{};
        std::uint64_t assetRevision{};
        std::uint64_t catalogRevision{};
        std::uint64_t profileRevision{};
        auto operator<=>(const MixerPlanIdentity &) const noexcept = default;
    };

    /** @brief Control-selected resource bounds, lowered beneath the compiled schema ceilings. */
    struct MixerCompileProfile final {
        MixerAssetSchemaLimits schema;
        std::uint32_t sampleRate{};
        AudioChannelLayout outputLayout; /**< Admitted Master/device processing layout; no implicit conversion. */
        std::uint32_t maximumFrames{};
        std::uint32_t maximumVoices{256};
        std::uint32_t maximumFanIn{256};
        std::uint32_t maximumFanOut{256};
        std::size_t maximumStorageBytes{MaximumAudioMemoryBytes};
        std::size_t maximumScratchBytes{MaximumAudioDSPScratchBytes};
        std::uint64_t maximumSampleOperations{64U * 1024U * 1024U};
        bool operator==(const MixerCompileProfile &) const = default;
    };

    /** @brief Owned DSP strategy and declared upper bound charged once per selected frame. */
    struct MixerDSPStrategy final {
        std::unique_ptr<IAudioDSPNode> node;
        std::uint64_t maximumOperationsPerFrame{}; /**< Positive bound includes all channels and private DSP work. */
    };

    /** @brief Explicit off-callback construction port; returned nodes own every provider/code lease.
     * The factory translates the complete persisted parameters into node defaults. It cannot retain
     * borrowed asset/format views. Nodes use the admitted first-party IAudioDSPNode contract.
     */
    class IMixerDSPFactory {
    public:
        virtual ~IMixerDSPFactory() = default;
        /** @brief Prepare an owned strategy without activating it. @param effect Persisted insert.
         * @param format Exact bus format. @return Node with owned code/provider lifetime, or typed failure.
         */
        [[nodiscard]] virtual Result<MixerDSPStrategy> Create(const MixerEffectDescriptor &effect, const AudioProcessingFormat &format) = 0;
    };

    /** @brief Immutable bus order and retained tap geometry; offsets are bytes in private plan storage. */
    struct MixerCompiledBus final {
        AudioBusId id;
        MixerBusRole role{MixerBusRole::Bus};
        AudioChannelLayout layout;
        float gain{1.0F};
        bool paused{};
        std::uint32_t firstIncoming{};
        std::uint32_t incomingCount{};
        std::size_t preFaderOffset{};
        std::size_t postFaderOffset{};
    };

    /** @brief Canonical incoming route; matching layouts are required until explicit conversion exists. */
    struct MixerCompiledRoute final {
        AudioRouteId id;
        std::uint32_t sourceBus{};
        MixerSendTap tap{MixerSendTap::PostFader};
        float gain{1.0F};
        bool operator==(const MixerCompiledRoute &) const = default;
    };

    /** @brief Fully owned prepared generation; immutable metadata and callback-exclusive mutable DSP bytes.
     * No asset views survive compilation. The control owner destroys it only after block completion
     * acknowledges replacement, or shutdown quiescence plus native callback detachment.
     */
    class MixerRenderPlan final {
    public:
        /** @brief Destroy owned strategies/storage only on control after acknowledged retirement or detached shutdown. */
        ~MixerRenderPlan();
        MixerRenderPlan(const MixerRenderPlan &) = delete;
        MixerRenderPlan &operator=(const MixerRenderPlan &) = delete;
        /** @brief Return the pinned compilation identity. @return Immutable identity. */
        [[nodiscard]] const MixerPlanIdentity &Identity() const noexcept;
        /** @brief Return canonical topological buses. @return Borrow valid through plan retirement. */
        [[nodiscard]] std::span<const MixerCompiledBus> Buses() const noexcept;
        /** @brief Return destination-grouped, stable-route-ID ordered edges. @return Immutable route table. */
        [[nodiscard]] std::span<const MixerCompiledRoute> Routes() const noexcept;
        /** @brief Resolve stable identity off-callback. @param id Bus identity. @return Compiled index, if present. */
        [[nodiscard]] std::optional<std::uint32_t> ResolveBus(AudioBusId id) const noexcept;
        /** @brief Return charged aligned sample/state/scratch bytes. @return Complete private byte reservation. */
        [[nodiscard]] std::size_t StorageBytes() const noexcept;

        /** @brief Freeze exact bus/send linear-gain and DSP parameter projections before publication.
         * @param bindings Host descriptors already bound into a sealed automation engine. Bus/send gain uses parameter ID 1,
         * linear amplitude units and range [0,16]; DSP IDs/ranges belong to the compiled node descriptor.
         * @return Success or typed identity/range/work/memory failure without mutation.
         * Initial values must match compiled defaults. Only one successful binding is allowed on an unpublished plan.
         * Runtime/epoch/generation ownership is exact; graph replacement requires a new plan and new automation owner.
         */
        [[nodiscard]] Result<void> BindAutomation(std::span<const AudioAutomationParameter> bindings);

        struct ConstructionKey; /**< Factory-only construction authority, not an activation capability. */
        struct State;
        /** @brief Construct only through CompileMixerGraph with complete detached state. */
        MixerRenderPlan(ConstructionKey, std::unique_ptr<State> state);

    private:
        friend class MixerGraphRuntime;
        std::unique_ptr<State> state_;
    };

    /** @brief Compile a current MixerAsset into complete fixed storage outside the callback.
     * @param asset Authoritative immutable snapshot, copied before return.
     * @param identity Pinned runtime/epoch/generation and source/catalog/profile revisions.
     * @param profile Explicit format and bounded resource limits.
     * @param factory Control-only DSP construction port; null admits only effect-free graphs.
     * @return Owned prepared plan or typed failure; no invalid/partial plan escapes.
     * Matching layouts, single-main-input/output inserts and zero-latency nodes are supported.
     * Extra ports/control dependencies and layout conversion fail explicitly. Insert tails are bounded
     * by the DSP declaration and cancelled on graph replacement; no implicit migration/overlap occurs.
     * Disabled routes remain structurally validated but are omitted from execution.
     */
    [[nodiscard]] Result<std::unique_ptr<MixerRenderPlan>> CompileMixerGraph(const MixerAssetSchema &asset,
                                                                             const MixerPlanIdentity &identity,
                                                                             const MixerCompileProfile &profile,
                                                                             IMixerDSPFactory *factory = nullptr);

    /** @brief One already admitted voice output in stable slot order; bus index belongs to graphGeneration.
     * The host validates voice liveness and controls paused voice clocks before entry. Views never escape.
     */
    struct MixerVoiceInput final {
        AudioVoiceHandle voice;
        std::uint64_t graphGeneration{};
        std::uint32_t busIndex{};
        AudioPlanarBlockView samples;
    };

    /** @brief Fixed callback outcomes; faults silence the complete admitted output and retain active ownership. */
    enum class MixerRenderStatus : std::uint8_t {
        Rendered,
        Active,
        Silence,
        InvalidEpoch,
        InvalidCommand,
        InvalidBuffer,
        DSPFault,
        Quiesced
    };

    /** @brief Generation facts from one completed block, never an allocating Error. */
    struct MixerRenderResult final {
        MixerRenderStatus status{MixerRenderStatus::Silence};
        std::uint64_t generation{};
        std::uint64_t acknowledgedSequence{};
        bool commandRejected{}; /**< Independent command failure does not hide a simultaneous render fault. */
        MixerSnapshotStatus snapshotStatus{MixerSnapshotStatus::Ok}; /**< Optional snapshot admission, independent of rendered signal. */
    };

    /** @brief Exact epoch and retained command producer composed by the Audio control owner. */
    struct MixerRuntimeDescriptor final {
        AudioCommandScope scope;
        AudioMemoryPoolId storageIdentity;
        MixerCompileProfile profile;
        std::size_t maximumRetainedBytes{MaximumAudioMemoryBytes}; /**< Combined backing bytes for active and pending plans. */
    };

    /** @brief Borrowed same-callback automation and at most one retained snapshot dispatched inside this block.
     * Clock names the first rendered sample; snapshot target must fall inside this block in that exact epoch.
     * The host retains all owners/sidecars through completed-block acknowledgement. No callbacks or registry lookup occur.
     */
    struct MixerAutomationRenderContext final {
        AudioParameterAutomation &automation;
        AudioSampleClock clock;
        MixerSnapshotTransitions *transitions{};
        const PreparedMixerSnapshot *snapshot{};
    };

    /** @brief Two-slot generation owner with existing command transport and complete-block acknowledgement.
     * Publish/Reconcile/Close belong to one control thread; Render belongs to one non-reentrant callback.
     * Only one swap may await acknowledgement. Lock-free sequentially consistent atomics publish fully
     * prepared plans and completion after the callback's last use. At most three atomic operations
     * occur per admitted callback boundary; none occur per sample. Reconcile then destroys old nodes
     * and buffers on control. No shared_ptr final release or allocation/free occurs on callback.
     * Native callbacks must be detached/joined before destruction; Close alone is not that proof.
     */
    class MixerGraphRuntime final {
    public:
        /** @brief Allocate the fixed owner off-callback. @param descriptor Exact admitted epoch/profile.
         * @return Stable-address owner or typed invalid/allocation failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<MixerGraphRuntime>> Create(const MixerRuntimeDescriptor &descriptor);
        /** @brief Destroy only on control after callback stop/join and accepted transport work reconciliation. */
        ~MixerGraphRuntime();
        MixerGraphRuntime(const MixerGraphRuntime &) = delete;
        MixerGraphRuntime &operator=(const MixerGraphRuntime &) = delete;
        /** @brief Submit the next complete generation through normal scene-gated staging.
         * @param plan Caller-owned candidate, consumed only on successful admission.
         * @param current Current pinned source/catalog/profile identity; stale candidates are rejected.
         * @param staging Existing retained epoch transport; host Pump publishes accepted work in FIFO order.
         * @return Admission/retry status. Rejection preserves plan and active generation; no sequence is invented.
         */
        [[nodiscard]] AudioCommandAdmission Publish(std::unique_ptr<MixerRenderPlan> &plan, const MixerPlanIdentity &current,
                                                    AudioCommandStaging &staging) noexcept;
        /** @brief Render one complete buffer boundary and at most one host-dispatched graph command.
         * @param scope Exact admitted runtime/epoch/context; foreign entry cannot mutate active state.
         * @param swap Optional record consumed from existing staging by the host dispatcher; only SwapGraph is admitted.
         * @param voices Pre-admitted stable-slot ordered direct outputs, bounded by the profile.
         * @param output Exclusive prevalidated output storage for the admitted Master format.
         * @return Fixed status and generation. Rejected commands preserve last good plan and may render it.
         * Hosts dispatch other command kinds to their owners; this method never consumes or discards them.
         */
        [[nodiscard]] MixerRenderResult Render(const AudioCommandScope &scope, const AudioCommandRecord *swap,
                                               std::span<const MixerVoiceInput> voices, const AudioPlanarBlockView &output) noexcept;
        /** @brief Render actual prepared bus/send/DSP projections at each sample, with atomic scheduled snapshot admission.
         * @param scope Exact runtime/scene epoch. @param swap Optional normal graph publication record.
         * @param voices Prepared voice outputs. @param output Admitted complete output block.
         * @param automation Same-callback sealed owner and first-sample clock, optionally one retained snapshot.
         * @return Rendered signal or full silence on invalid binding/clock/DSP fault; snapshot rejection is reported independently
         * and preserves the prior accepted trajectory. Public plan descriptors remain immutable.
         * Per-sample DSP calls use aligned prepared taps at sample zero and descriptor-admitted one-frame blocks;
         * voice source offsets and output destinations advance through the caller's block. Ordinary Render remains unchanged.
         */
        [[nodiscard]] MixerRenderResult RenderAutomated(const AudioCommandScope &scope, const AudioCommandRecord *swap,
                                                        std::span<const MixerVoiceInput> voices, const AudioPlanarBlockView &output,
                                                        const MixerAutomationRenderContext &automation) noexcept;
        /** @brief Acquire completed-block facts and reclaim an acknowledged replaced generation on control.
         * @return Latest complete-block acknowledgement. Queue consumption alone never authorizes reclamation.
         */
        [[nodiscard]] MixerRenderResult Reconcile() noexcept;
        /** @brief Close publication and request callback silence/quiescence; idempotent, no native join. */
        void Close() noexcept;
        /** @brief Reconcile cancelled queued swaps and release plans only after native detachment.
         * @param scope Exact retiring epoch/context. @param backendDetached Owning host's verified native stop/join proof.
         * @return Quiesced on success; false/stale/unclosed calls preserve all ownership.
         * @pre True is supplied only after the host validates its matching native Stopped outcome: all in-flight
         * callback borrows/pins ended and future entry is impossible. Close, queue consumption, silence and
         * timeout are not that proof. A final graph callback is unnecessary after that proved stop/join.
         * Parent lifecycle quiescence/epoch reconciliation remains the host's authority. The host drains or
         * reconciles accepted transport records before destroying the transport/runtime.
         * Repeated successful calls are harmless; destruction still belongs to the detached control owner.
         */
        [[nodiscard]] MixerRenderStatus CompleteShutdown(const AudioCommandScope &scope, bool backendDetached) noexcept;

        struct ConstructionKey;
        struct State;
        /** @brief Factory-only construction of stable-address prepared state. */
        MixerGraphRuntime(ConstructionKey, std::unique_ptr<State> state);

    private:
        /** @brief Share generation adoption and complete-block acknowledgement across admitted render paths. */
        [[nodiscard]] MixerRenderResult RenderCore(const AudioCommandScope &scope, const AudioCommandRecord *swap,
                                                   std::span<const MixerVoiceInput> voices, const AudioPlanarBlockView &output,
                                                   const MixerAutomationRenderContext *automation) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio
