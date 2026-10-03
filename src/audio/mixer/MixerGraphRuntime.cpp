#include "MixerPlanState.h"

#include <atomic>
#include <limits>

namespace Horo::Audio {
    struct MixerGraphRuntime::ConstructionKey final {};

    /** @brief Control owns slots/generations; callback owns active until host-proved detachment.
     * Lock-free sequentially consistent atomics publish cross-thread handoffs; detached cleanup is exclusive.
     */
    struct MixerGraphRuntime::State final {
        MixerRuntimeDescriptor descriptor;
        AudioProcessingFormat outputFormat;
        std::array<std::unique_ptr<MixerRenderPlan>, 2> plans;
        std::array<std::uint64_t, 2> slotGenerations{};
        std::int32_t activeSlot{-1};
        std::int32_t pendingSlot{-1};
        std::uint64_t lastPublishedGeneration{};
        MixerRenderPlan *active{};
        std::uint64_t callbackSequence{};
        std::atomic<MixerRenderPlan *> candidate{};
        std::atomic<std::uint64_t> completedSequence{};
        std::atomic<bool> closed{};
        std::atomic<bool> quiesced{};
    };

    static_assert(std::atomic<MixerRenderPlan *>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

    namespace {
        /** @brief Control-only validation of the complete pinned candidate against the live runtime profile. */
        bool ValidCandidate(const MixerGraphRuntime::State &runtime, const MixerRenderPlan::State &plan,
                            const MixerPlanIdentity &current) noexcept {
            return plan.identity == current && current.owner == runtime.descriptor.scope.owner &&
                   current.epoch == runtime.descriptor.scope.epoch && current.generation > runtime.lastPublishedGeneration &&
                   plan.profile == runtime.descriptor.profile;
        }

        /** @brief Charge active plus pending backing storage with checked subtraction. */
        bool BackingFits(const MixerGraphRuntime::State &runtime, const MixerRenderPlan &plan) noexcept {
            const std::size_t retained = runtime.activeSlot == -1 ? 0 : runtime.plans[runtime.activeSlot]->StorageBytes();
            return retained <= runtime.descriptor.maximumRetainedBytes &&
                   plan.StorageBytes() <= runtime.descriptor.maximumRetainedBytes - retained;
        }

        /** @brief Match only the current pending mailbox; stale commands never acquire a retired slot pointer. */
        bool MatchingCommand(const AudioCommandScope &scope, const AudioCommandRecord &record, const MixerRenderPlan::State *candidate,
                             const std::uint64_t previous) noexcept {
            const auto *payload = std::get_if<AudioSwapGraphCommand>(&record.command.payload);
            return record.command.scope == scope && payload != nullptr && candidate != nullptr && record.sequence > previous &&
                   record.sequence == candidate->sequence && payload->storage == candidate->handle;
        }

        /** @brief Keep signal faults visible when an independently rejected command accompanies the same block. */
        void MarkCommandFailure(MixerRenderResult &result, const bool rejected) noexcept {
            using enum MixerRenderStatus;
            if (rejected && (result.status == Rendered || result.status == Silence))
                result.status = InvalidCommand;
            result.commandRejected = rejected;
        }

        /** @brief Reject zero-length/non-admitted boundaries before graph adoption or output writes. */
        bool ValidRenderBuffer(const MixerGraphRuntime::State &runtime, const AudioPlanarBlockView &output) noexcept {
            return output.validFrames != 0 && output.capacityFrames <= runtime.descriptor.profile.maximumFrames &&
                   ValidateAudioPlanarBlock(output, runtime.outputFormat);
        }
    }  // namespace

    /** @copydoc MixerGraphRuntime::Create */
    Result<std::unique_ptr<MixerGraphRuntime>> MixerGraphRuntime::Create(const MixerRuntimeDescriptor &descriptor) {
        if (!descriptor.scope.owner.IsValid() || descriptor.scope.epoch == 0 || !descriptor.scope.scene.IsValid() ||
            descriptor.scope.scene.owner != descriptor.scope.owner || !descriptor.storageIdentity.IsValid() ||
            descriptor.maximumRetainedBytes == 0 || descriptor.maximumRetainedBytes > MaximumAudioMemoryBytes)
            return Result<std::unique_ptr<MixerGraphRuntime>>::Failure(
                MakeError(AudioErrors::GraphBuildFailed, "Invalid mixer runtime identity."));
        if (const Result<void> checked = MixerDetail::ValidateProfile(descriptor.profile); checked.HasError())
            return Result<std::unique_ptr<MixerGraphRuntime>>::Failure(checked.ErrorValue());
        try {
            auto state = std::make_unique<State>();
            state->descriptor = descriptor;
            state->outputFormat = {descriptor.profile.sampleRate, descriptor.profile.outputLayout};
            return Result<std::unique_ptr<MixerGraphRuntime>>::Success(
                std::make_unique<MixerGraphRuntime>(ConstructionKey{}, std::move(state)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<MixerGraphRuntime>>::Failure(
                MakeError(AudioErrors::GraphBuildFailed, "Mixer runtime allocation failed."));
        }
    }

    /** @copydoc MixerGraphRuntime::MixerGraphRuntime */
    MixerGraphRuntime::MixerGraphRuntime(ConstructionKey, std::unique_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc MixerGraphRuntime::~MixerGraphRuntime */
    MixerGraphRuntime::~MixerGraphRuntime() = default;

    /** @copydoc MixerGraphRuntime::Publish */
    AudioCommandAdmission MixerGraphRuntime::Publish(std::unique_ptr<MixerRenderPlan> &plan, const MixerPlanIdentity &current,
                                                     AudioCommandStaging &staging) noexcept {
        using enum AudioCommandStagingStatus;
        State &s = *state_;
        if (s.closed.load())
            return {Closed};
        if (s.pendingSlot != -1)
            return {Busy};
        if (!plan || !ValidCandidate(s, *plan->state_, current))
            return {InvalidCommand};
        if (!BackingFits(s, *plan))
            return {OrdinaryFull};
        const std::int32_t slot = s.activeSlot == 0 ? 1 : 0;
        if (s.slotGenerations[slot] == std::numeric_limits<std::uint64_t>::max())
            return {SequenceExhausted};
        const AudioMemoryHandle handle{s.descriptor.scope.owner, s.descriptor.storageIdentity, static_cast<std::uint32_t>(slot + 1),
                                       s.slotGenerations[slot] + 1};
        plan->state_->handle = handle;
        s.plans[slot] = std::move(plan);
        const AudioCommand command{s.descriptor.scope, AudioSwapGraphCommand{handle}};
        const AudioCommandAdmission admission = staging.Submit(command);
        if (admission.status != Ok) {
            plan = std::move(s.plans[slot]);
            plan->state_->handle = {};
            return admission;
        }
        s.plans[slot]->state_->sequence = admission.sequence;
        ++s.slotGenerations[slot];
        s.lastPublishedGeneration = current.generation;
        s.pendingSlot = slot;
        // Pump is owned by this same control thread and cannot publish the record before this return.
        s.candidate.store(s.plans[slot].get());
        return admission;
    }

    /** @copydoc MixerGraphRuntime::Render */
    MixerRenderResult MixerGraphRuntime::Render(const AudioCommandScope &scope, const AudioCommandRecord *swap,
                                                const std::span<const MixerVoiceInput> voices,
                                                const AudioPlanarBlockView &output) noexcept {
        using enum MixerRenderStatus;
        State &s = *state_;
        if (!ValidRenderBuffer(s, output))
            return {InvalidBuffer};
        if (scope != s.descriptor.scope) {
            MixerDetail::Silence(output);
            return {InvalidEpoch};
        }
        if (s.closed.load()) {
            s.active = nullptr;
            MixerDetail::Silence(output);
            s.quiesced.store(true);
            return {Quiesced, 0, s.callbackSequence};
        }
        bool invalidCommand{};
        if (swap != nullptr) {
            MixerRenderPlan *candidate = s.candidate.load();
            const MixerRenderPlan::State *prepared = candidate == nullptr ? nullptr : candidate->state_.get();
            if (!MatchingCommand(scope, *swap, prepared, s.callbackSequence)) {
                invalidCommand = true;
            } else {
                s.active = candidate;
                s.callbackSequence = swap->sequence;
            }
        }
        MixerRenderResult result;
        if (s.active != nullptr) {
            result.status = MixerDetail::RenderPlan(*s.active->state_, voices, output);
            result.generation = s.active->Identity().generation;
        } else {
            MixerDetail::Silence(output);
        }
        MarkCommandFailure(result, invalidCommand);
        result.acknowledgedSequence = s.callbackSequence;
        // Publication occurs after every sample/node reference in this block has ended.
        s.completedSequence.store(s.callbackSequence);
        return result;
    }

    /** @copydoc MixerGraphRuntime::Reconcile */
    MixerRenderResult MixerGraphRuntime::Reconcile() noexcept {
        using enum MixerRenderStatus;
        State &s = *state_;
        const std::uint64_t sequence = s.completedSequence.load();
        if (s.quiesced.load()) {
            return {Quiesced, 0, sequence};
        }
        if (s.pendingSlot != -1 && s.plans[s.pendingSlot]->state_->sequence == sequence) {
            // Only the pending mailbox is lookup-visible. Future stale commands cannot acquire an old slot pointer.
            s.candidate.store(nullptr);
            if (s.activeSlot != -1)
                s.plans[s.activeSlot].reset();
            s.activeSlot = s.pendingSlot;
            s.pendingSlot = -1;
        }
        return {s.activeSlot == -1 ? Silence : Active, s.activeSlot == -1 ? 0 : s.plans[s.activeSlot]->Identity().generation, sequence};
    }

    /** @copydoc MixerGraphRuntime::Close */
    void MixerGraphRuntime::Close() noexcept {
        state_->closed.store(true);
    }

    /** @copydoc MixerGraphRuntime::CompleteShutdown */
    MixerRenderStatus MixerGraphRuntime::CompleteShutdown(const AudioCommandScope &scope, const bool backendDetached) noexcept {
        using enum MixerRenderStatus;
        State &s = *state_;
        if (scope != s.descriptor.scope)
            return InvalidEpoch;
        if (!backendDetached || !s.closed.load())
            return InvalidCommand;
        // The owning host validated exact-epoch stop/join: no current reader or future callback entry remains.
        s.active = nullptr;
        s.candidate.store(nullptr);
        s.quiesced.store(true);
        for (auto &plan : s.plans)
            plan.reset();
        s.activeSlot = -1;
        s.pendingSlot = -1;
        return Quiesced;
    }
}  // namespace Horo::Audio
