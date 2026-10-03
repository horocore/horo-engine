#include "Horo/Audio/AudioCommandStaging.h"

#include "Horo/Audio/Internal/AudioCallbackSafetyHooks.h"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

namespace Horo::Audio {
    namespace {  // NOSONAR - internal-linkage implementation details must not enter the public audio namespace.
        enum class ScenePhase : std::uint8_t {
            Vacant,
            Open,
            Closing,
            Queued,
            Retired
        };

        /** @brief Ingress-mutex-protected admission and exact barrier identity, never accessed by the callback. */
        struct SceneGate {
            std::uint64_t barrier{};
            std::uint32_t generation{};
            ScenePhase phase{ScenePhase::Vacant};
        };

        /** @brief Bound arithmetic before any allocation or pool admission. */
        bool ValidDimensions(const AudioCommandStagingDescriptor &descriptor) noexcept {
            const std::array requirements{descriptor.ingressIdentity != descriptor.callback.storageIdentity,
                                          descriptor.ingressSlots >= 2,
                                          descriptor.ingressSlots <= MaximumAudioCommandSlots,
                                          std::has_single_bit(descriptor.ingressSlots),
                                          descriptor.criticalSlots > 0,
                                          descriptor.criticalSlots < descriptor.ingressSlots,
                                          descriptor.sceneSlots > 0,
                                          descriptor.sceneSlots <= MaximumAudioHandleSlots,
                                          descriptor.ingressBudgetBytes <= MaximumAudioMemoryBytes};
            return std::ranges::all_of(requirements, std::identity{});
        }

        /** @brief Apply one operation only while its moved-from-capable owner still has state. */
        template <typename ResultType, typename StateType, typename Operation>
        ResultType WithState(StateType *state, ResultType inactive, Operation &&operation) noexcept {
            if (!state) {
                return inactive;
            }
            return std::invoke(std::forward<Operation>(operation), *state);
        }
    }  // namespace

    /** @brief Ingress lock protects records/gates/sequences/closing; output's independent SPSC atomics serve callback access. */
    struct AudioCommandStaging::State {
        AudioCommandStagingDescriptor descriptor;
        AudioCommandBuffer output;
        AudioMemoryPool storage;
        std::span<AudioCommandRecord> records;
        std::vector<SceneGate> scenes;
        std::mutex mutex;
        std::uint32_t head{};
        std::uint32_t count{};
        std::uint64_t nextSequence{};
        std::uint64_t publishedSequence{};
        std::uint64_t resetSequence{};
        bool resetPending{};
        bool closing{};

        /** @brief Allocate gate storage and initialize admitted record lifetimes before any participant starts. */
        State(const AudioCommandStagingDescriptor &description, AudioCommandBuffer buffer, AudioMemoryPool pool,
              const std::span<std::byte> allocation)
            : descriptor(description), output(std::move(buffer)), storage(std::move(pool)),
              records(static_cast<AudioCommandRecord *>(static_cast<void *>(allocation.data())), description.ingressSlots),
              scenes(description.sceneSlots) {
            std::uninitialized_value_construct_n(records.data(), records.size());
        }

        /** @brief End record lifetimes only after the host has joined all participants. */
        ~State() {
            std::destroy_n(records.data(), records.size());
        }

        /** @brief Validate owner and slot before indexing, optionally requiring the registered generation. */
        SceneGate *Find(const AudioSceneContextHandle scene, const bool requireExactGeneration = false) noexcept {
            if (const std::array invalid{!scene.IsValid(), scene.owner != descriptor.callback.owner, scene.slot > descriptor.sceneSlots};
                std::ranges::any_of(invalid, std::identity{})) {
                return nullptr;
            }
            auto *gate = &scenes[scene.slot - 1];
            const std::array unavailableExactGate{gate->generation != scene.generation, gate->phase == ScenePhase::Retired};
            const std::array generationMismatch{requireExactGeneration, std::ranges::any_of(unavailableExactGate, std::identity{})};
            const std::array candidates{gate, static_cast<SceneGate *>(nullptr)};
            return candidates[static_cast<std::size_t>(std::ranges::all_of(generationMismatch, std::identity{}))];
        }

        /** @brief Enqueue under the ingress mutex, preserving rejected work and coalescing only adjacent parameter snapshots. */
        AudioCommandAdmission Enqueue(const AudioCommand &command) noexcept {
            using enum AudioCommandStagingStatus;
            if (nextSequence == std::numeric_limits<std::uint64_t>::max()) {
                return {.status = SequenceExhausted};
            }
            const auto tail = (head + count) & (descriptor.ingressSlots - 1);
            const auto previous = (tail - 1) & (descriptor.ingressSlots - 1);
            if (const std::array coalescible{count != 0, CanCoalesceAudioCommands(records[previous].command, command)};
                std::ranges::all_of(coalescible, std::identity{})) {
                const auto replaced = records[previous].sequence;
                records[previous] = {.sequence = ++nextSequence, .command = command};
                return {.status = Coalesced, .sequence = nextSequence, .replacedSequence = replaced};
            }
            const bool critical = ClassifyAudioCommand(command) == AudioCommandClass::Critical;
            const std::array limits{descriptor.ingressSlots - descriptor.criticalSlots, descriptor.ingressSlots};
            if (const auto limit = limits[static_cast<std::size_t>(critical)]; count >= limit) {
                const std::array statuses{OrdinaryFull, CriticalRetry};
                return {.status = statuses[static_cast<std::size_t>(critical)]};
            }
            records[tail] = {.sequence = ++nextSequence, .command = command};
            ++count;
            return {.status = Ok, .sequence = nextSequence};
        }

        /** @brief Close SPSC only after every accepted ingress record was published. */
        void FinishClosing() noexcept {
            const std::array ready{closing, count == 0};
            if (std::ranges::all_of(ready, std::identity{})) {
                output.Close();
            }
        }
    };

    /** @copydoc AudioCommandStaging::Create */
    Result<AudioCommandStaging> AudioCommandStaging::Create(const AudioCommandStagingDescriptor &descriptor) {
        using enum AudioMemoryStatus;
        if (!ValidDimensions(descriptor)) {
            return Result<AudioCommandStaging>::Failure(MakeError(AudioErrors::CommandBufferInvalid));
        }
        const auto gateBytes = sizeof(SceneGate) * descriptor.sceneSlots;
        if (descriptor.ingressBudgetBytes <= gateBytes) {
            return Result<AudioCommandStaging>::Failure(MakeError(AudioErrors::MemoryBudgetExceeded));
        }
        auto output = AudioCommandBuffer::Create(descriptor.callback);
        if (!output.HasValue()) {
            return Result<AudioCommandStaging>::Failure(output.ErrorValue());
        }
        auto pool = AudioMemoryPool::Create({.owner = descriptor.callback.owner,
                                             .identity = descriptor.ingressIdentity,
                                             .purpose = AudioMemoryPurpose::CommandStorage,
                                             .slots = 1,
                                             .blockBytes = sizeof(AudioCommandRecord) * descriptor.ingressSlots,
                                             .budgetBytes = descriptor.ingressBudgetBytes - gateBytes});
        if (!pool.HasValue()) {
            return Result<AudioCommandStaging>::Failure(pool.ErrorValue());
        }
        auto storage = std::move(pool).Value();
        const auto allocation = storage.Acquire();
        if (const std::array invalidAllocation{allocation.status != Ok,
                                               allocation.bytes.size() < sizeof(AudioCommandRecord) * descriptor.ingressSlots};
            std::ranges::any_of(invalidAllocation, std::identity{})) {
            return Result<AudioCommandStaging>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
        try {
            return Result<AudioCommandStaging>::Success(
                AudioCommandStaging{std::make_unique<State>(descriptor, std::move(output).Value(), std::move(storage), allocation.bytes)});
        } catch (const std::bad_alloc &) {
            return Result<AudioCommandStaging>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioCommandStaging::AudioCommandStaging */
    AudioCommandStaging::AudioCommandStaging(std::unique_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc AudioCommandStaging::~AudioCommandStaging */
    AudioCommandStaging::~AudioCommandStaging() = default;
    /** @copydoc AudioCommandStaging::AudioCommandStaging */
    AudioCommandStaging::AudioCommandStaging(AudioCommandStaging &&) noexcept = default;
    /** @copydoc AudioCommandStaging::operator= */
    AudioCommandStaging &AudioCommandStaging::operator=(AudioCommandStaging &&) noexcept = default;

    /** @copydoc AudioCommandStaging::RegisterScene */
    AudioCommandStagingStatus AudioCommandStaging::RegisterScene(const AudioSceneContextHandle scene) noexcept {  // NOSONAR
        using enum AudioCommandStagingStatus;
        return WithState(state_.get(), Inactive, [scene](State &state) {  // NOSONAR - one cohesive locked state transition.
            Safety::OnAudioLockAttempt();
            const std::unique_lock lock(state.mutex, std::try_to_lock);  // NOSONAR - lock covers the complete transition.
            if (!lock.owns_lock()) {  // NOSONAR - moving the lock into this if would end protection too early.
                return Busy;
            }
            if (const std::array closed{state.closing, state.resetPending}; std::ranges::any_of(closed, std::identity{})) {
                return Closed;
            }
            auto *gate = state.Find(scene);
            if (!gate) {
                return InvalidScene;
            }
            const std::array reusablePhases{gate->phase == ScenePhase::Vacant, gate->phase == ScenePhase::Retired};
            if (const std::array invalid{scene.generation <= gate->generation, !std::ranges::any_of(reusablePhases, std::identity{})};
                std::ranges::any_of(invalid, std::identity{})) {
                return InvalidScene;
            }
            *gate = {.generation = scene.generation, .phase = ScenePhase::Open};
            return Ok;
        });
    }

    /** @copydoc AudioCommandStaging::Submit */
    AudioCommandAdmission AudioCommandStaging::Submit(const AudioCommand &command) noexcept {  // NOSONAR
        using enum AudioCommandStagingStatus;
        return WithState(state_.get(), AudioCommandAdmission{}, [&command](State &state) {  // NOSONAR - cohesive admission transition.
            AudioCommand normalized;
            const std::array barriers{std::holds_alternative<AudioSceneUnloadCommand>(command.payload),
                                      std::holds_alternative<AudioResetCommand>(command.payload)};
            if (const std::array valid{command.scope.owner == state.descriptor.callback.owner,
                                       command.scope.epoch == state.descriptor.callback.epoch,
                                       !std::ranges::any_of(barriers, std::identity{}),
                                       NormalizeAudioCommand(command, normalized) == AudioCommandStatus::Ok};
                !std::ranges::all_of(valid, std::identity{})) {
                return AudioCommandAdmission{.status = InvalidCommand};
            }
            Safety::OnAudioLockAttempt();
            const std::unique_lock lock(state.mutex, std::try_to_lock);  // NOSONAR - lock covers the complete transition.
            if (!lock.owns_lock()) {  // NOSONAR - moving the lock into this if would end protection too early.
                return AudioCommandAdmission{.status = Busy};
            }
            const bool ordinary = ClassifyAudioCommand(normalized) == AudioCommandClass::Ordinary;
            const std::array resetBlocksOrdinary{state.resetPending, ordinary};
            if (const std::array closed{state.closing, std::ranges::all_of(resetBlocksOrdinary, std::identity{})};
                std::ranges::any_of(closed, std::identity{})) {
                return AudioCommandAdmission{.status = Closed};
            }
            const auto *gate = state.Find(normalized.scope.scene, true);
            if (!gate) {
                return AudioCommandAdmission{.status = InvalidScene};
            }
            const std::array closingCritical{gate->phase == ScenePhase::Closing, !ordinary};
            if (const std::array admitted{gate->phase == ScenePhase::Open, std::ranges::all_of(closingCritical, std::identity{})};
                !std::ranges::any_of(admitted, std::identity{})) {
                return AudioCommandAdmission{.status = InvalidScene};
            }
            return state.Enqueue(normalized);
        });
    }

    /** @copydoc AudioCommandStaging::StageSceneUnload */
    AudioCommandAdmission AudioCommandStaging::StageSceneUnload(const AudioSceneContextHandle scene) noexcept {  // NOSONAR
        using enum AudioCommandStagingStatus;
        return WithState(state_.get(), AudioCommandAdmission{}, [scene](State &state) {  // NOSONAR - cohesive barrier transition.
            Safety::OnAudioLockAttempt();
            const std::unique_lock lock(state.mutex, std::try_to_lock);  // NOSONAR - lock covers the complete transition.
            if (!lock.owns_lock()) {  // NOSONAR - moving the lock into this if would end protection too early.
                return AudioCommandAdmission{.status = Busy};
            }
            if (state.closing) {
                return AudioCommandAdmission{.status = Closed};
            }
            auto *gate = state.Find(scene, true);
            if (!gate) {
                return AudioCommandAdmission{.status = InvalidScene};
            }
            if (gate->phase == ScenePhase::Queued) {
                return AudioCommandAdmission{.status = AlreadyStaged, .sequence = gate->barrier};
            }
            gate->phase = ScenePhase::Closing;
            const auto result = state.Enqueue(
                {.scope = {.owner = state.descriptor.callback.owner, .epoch = state.descriptor.callback.epoch, .scene = scene},
                 .payload = AudioSceneUnloadCommand{}});
            if (result.status == Ok) {
                gate->phase = ScenePhase::Queued;
                gate->barrier = result.sequence;
            }
            return result;
        });
    }

    /** @copydoc AudioCommandStaging::AcknowledgeScene */
    AudioCommandStagingStatus AudioCommandStaging::AcknowledgeScene(  // NOSONAR - mutates owned state through the pimpl.
        const AudioSceneContextHandle scene,
        const std::uint64_t sequence) noexcept {  // NOSONAR
        using enum AudioCommandStagingStatus;
        return WithState(state_.get(), Inactive, [scene, sequence](State &state) {
            Safety::OnAudioLockAttempt();
            const std::unique_lock lock(state.mutex, std::try_to_lock);  // NOSONAR - lock covers the complete transition.
            if (!lock.owns_lock()) {  // NOSONAR - moving the lock into this if would end protection too early.
                return Busy;
            }
            auto *gate = state.Find(scene, true);
            if (!gate) {
                return InvalidScene;
            }
            if (const std::array invalid{gate->phase != ScenePhase::Queued, gate->barrier != sequence, sequence > state.publishedSequence};
                std::ranges::any_of(invalid, std::identity{})) {
                return InvalidScene;
            }
            gate->phase = ScenePhase::Retired;
            return Ok;
        });
    }

    /** @copydoc AudioCommandStaging::StageReset */
    AudioCommandAdmission AudioCommandStaging::StageReset() noexcept {  // NOSONAR - mutates owned state through the pimpl.
        using enum AudioCommandStagingStatus;
        return WithState(state_.get(), AudioCommandAdmission{}, [](State &state) {  // NOSONAR - cohesive reset transition.
            Safety::OnAudioLockAttempt();
            const std::unique_lock lock(state.mutex, std::try_to_lock);  // NOSONAR - lock covers the complete transition.
            if (!lock.owns_lock()) {  // NOSONAR - moving the lock into this if would end protection too early.
                return AudioCommandAdmission{.status = Busy};
            }
            if (state.resetSequence != 0) {
                return AudioCommandAdmission{.status = AlreadyStaged, .sequence = state.resetSequence};
            }
            if (state.closing) {
                return AudioCommandAdmission{.status = Closed};
            }
            state.resetPending = true;
            const auto result =
                state.Enqueue({.scope = {.owner = state.descriptor.callback.owner, .epoch = state.descriptor.callback.epoch, .scene = {}},
                               .payload = AudioResetCommand{}});
            if (result.status == Ok) {
                state.resetSequence = result.sequence;
                state.closing = true;
            }
            return result;
        });
    }

    /** @copydoc AudioCommandStaging::Pump */
    AudioCommandPumpResult AudioCommandStaging::Pump(const std::uint32_t maximumRecords) noexcept {  // NOSONAR
        using enum AudioCommandPublishStatus;
        return WithState(state_.get(), AudioCommandPumpResult{}, [maximumRecords](State &state) {  // NOSONAR - cohesive pump step.
            Safety::OnAudioLockAttempt();
            const std::unique_lock lock(state.mutex, std::try_to_lock);  // NOSONAR - lock covers the complete pump step.
            if (!lock.owns_lock()) {  // NOSONAR - moving the lock into this if would end protection too early.
                return AudioCommandPumpResult{.status = AudioCommandStagingStatus::Busy};
            }
            AudioCommandPumpResult result{.status = AudioCommandStagingStatus::Ok};
            const auto limit = std::min(maximumRecords, state.count);
            while (result.published < limit) {
                const auto &record = state.records[state.head];
                if (const auto published = state.output.TryPublish(record);  // NOSONAR - publish/staging enums share names.
                    published != Published) {
                    const std::array retryable{published == OrdinaryFull, published == CriticalRetry};
                    const std::array statuses{AudioCommandStagingStatus::ProtocolError, AudioCommandStagingStatus::OutputFull};
                    result.status = statuses[static_cast<std::size_t>(std::ranges::any_of(retryable, std::identity{}))];
                    return result;
                }
                state.publishedSequence = record.sequence;
                state.head = (state.head + 1) & (state.descriptor.ingressSlots - 1);
                --state.count;
                ++result.published;
            }
            state.FinishClosing();
            return result;
        });
    }

    /** @copydoc AudioCommandStaging::Close */
    AudioCommandStagingStatus AudioCommandStaging::Close() noexcept {  // NOSONAR - mutates owned state through the pimpl.
        using enum AudioCommandStagingStatus;
        return WithState(state_.get(), Inactive, [](State &state) {
            Safety::OnAudioLockAttempt();
            const std::unique_lock lock(state.mutex, std::try_to_lock);  // NOSONAR - lock covers the complete transition.
            if (!lock.owns_lock()) {  // NOSONAR - moving the lock into this if would end protection too early.
                return Busy;
            }
            state.closing = true;
            state.FinishClosing();
            return Ok;
        });
    }

    /** @copydoc AudioCommandStaging::TryConsume */
    bool AudioCommandStaging::TryConsume(AudioCommandRecord &record) noexcept {  // NOSONAR - consumes through owned pimpl state.
        return WithState(state_.get(), false, [&record](State &state) {
            return state.output.TryConsume(record);
        });
    }

    /** @copydoc AudioCommandStaging::IsDrained */
    bool AudioCommandStaging::IsDrained() const noexcept {
        return WithState(state_.get(), true, [](const State &state) {
            return state.output.IsDrained();
        });
    }

    /** @copydoc AudioCommandStaging::Stats */
    std::optional<AudioCommandStagingStats> AudioCommandStaging::Stats() const noexcept {
        if (!state_)
            return std::nullopt;
        if (const std::unique_lock lock(state_->mutex, std::try_to_lock); lock.owns_lock())
            return AudioCommandStagingStats{.ingressDepth = state_->count, .callbackDepth = state_->output.Depth()};
        return std::nullopt;
    }
}  // namespace Horo::Audio
