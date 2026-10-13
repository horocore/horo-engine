#include "AnimationGraphInstanceInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Animation {
    namespace {
        /** @brief Selects the registered diagnostic without constructing an owning error on the hot path. */
        const ErrorCodeDescriptor &FailureDescriptor(const GraphInstanceFailure failure) noexcept {
            using enum GraphInstanceFailure;
            switch (failure) {
                case Cancelled:
                    return AnimationErrors::GraphOperationCancelled;
                case AdmissionRejected:
                    return AnimationErrors::GraphAdmissionRejected;
                case TypeMismatch:
                    return AnimationErrors::GraphTypeMismatch;
                case LimitExceeded:
                    return AnimationErrors::GraphLimitExceeded;
                case BindingMismatch:
                    return AnimationErrors::GraphBindingMismatch;
                case HandleMalformed:
                    return AnimationErrors::HandleMalformed;
                case HandleStale:
                    return AnimationErrors::HandleStale;
                case ThreadViolation:
                    return AnimationErrors::PoseThreadViolation;
                case TickStale:
                    return AnimationErrors::PoseFrameStale;
                case StateInvalid:
                    return AnimationErrors::GraphMalformed;
                case None:
                case Malformed:
                case ClipFailure:
                    return AnimationErrors::GraphMalformed;
            }
            return AnimationErrors::GraphMalformed;
        }

        /** @brief Checks immutable track storage against the instance's admitted sampling limits. */
        GraphInstanceFailure AdmitClipTracks(const AnimationClipAsset &clip, const AnimationClipLimits &limits) noexcept {
            using enum GraphInstanceFailure;
            if (clip.Data().tracks.size() > limits.maximumTracks)
                return LimitExceeded;
            std::uint64_t keys = 0;
            for (const auto &track : clip.Data().tracks) {
                if (track.keys.size() > limits.maximumKeysPerTrack)
                    return LimitExceeded;
                keys += track.keys.size();
            }
            return keys > limits.maximumTotalKeys ? LimitExceeded : None;
        }

        /** @brief Proves exactly one exact immutable publication exists for each compiled clip binding. */
        GraphInstanceFailure AdmitClipBindings(const GraphInstanceAssets &assets, const AnimationClipLimits &limits) noexcept {
            using enum GraphInstanceFailure;
            for (const auto &expected : assets.program->ClipBindings()) {
                std::size_t matches = 0;
                for (const auto &clip : assets.clips) {
                    if (!clip)
                        return BindingMismatch;
                    if (clip->Data().descriptor.id != expected.id)
                        continue;
                    if (clip->Data().descriptor != expected)
                        return BindingMismatch;
                    if (const auto failure = AdmitClipTracks(*clip, limits); failure != None)
                        return failure;
                    ++matches;
                }
                if (matches != 1)
                    return BindingMismatch;
            }
            return None;
        }

        /** @brief Charges complete per-tick pose/key-search work after exact clip bindings have been admitted. */
        GraphInstanceFailure AdmitClipWork(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
            std::uint64_t operations = 0;
            for (const auto &curve : assets.curves)
                operations += 1 + std::bit_width(curve->Keys().size());
            if (operations > context.maximumJointOperations)
                return GraphInstanceFailure::LimitExceeded;
            for (const auto &occurrence : assets.program->Occurrences()) {
                const auto &instruction =
                    assets.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex];
                std::uint64_t cost = 0;
                if (std::holds_alternative<GraphBlendNode>(instruction.payload))
                    cost = 3ULL * assets.program->SkeletonBinding().joints;
                if (occurrence.phase == GraphOccurrencePhase::StateBegin)
                    cost = 4ULL * assets.program->SkeletonBinding().joints;
                if (occurrence.phase == GraphOccurrencePhase::StateEnd)
                    cost = 8ULL * assets.program->SkeletonBinding().joints;
                if (occurrence.clipBindingIndex.has_value()) {
                    const auto &descriptor = assets.program->ClipBindings()[*occurrence.clipBindingIndex];
                    const auto clip = std::ranges::find_if(assets.clips, [&descriptor](const auto &value) {
                        return value->Data().descriptor == descriptor;
                    });
                    cost = (descriptor.kind == AnimationClipKind::Additive ? 6ULL : 3ULL) * assets.program->SkeletonBinding().joints;
                    for (const auto &track : (*clip)->Data().tracks)
                        cost += 1 + std::bit_width(track.keys.size());
                }
                if (cost > context.maximumJointOperations || operations > context.maximumJointOperations - cost)
                    return GraphInstanceFailure::LimitExceeded;
                operations += cost;
            }
            return GraphInstanceFailure::None;
        }

        /** @brief Validates sampling policy against fixed engine bounds before inspecting clip contents. */
        bool ValidClipLimits(const AnimationClipLimits &limits) noexcept {
            return limits.maximumTracks > 0 && limits.maximumTracks <= AnimationClipHardLimits::Tracks && limits.maximumKeysPerTrack > 0 &&
                   limits.maximumKeysPerTrack <= AnimationClipHardLimits::KeysPerTrack && limits.maximumTotalKeys > 0 &&
                   limits.maximumTotalKeys <= AnimationClipHardLimits::TotalKeys &&
                   limits.maximumBoundaryCrossings <= AnimationClipHardLimits::BoundaryCrossings;
        }

        /** @brief Admits unique float curve bindings and their bounded binary-search work before allocation. */
        GraphInstanceFailure AdmitCurves(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
            using enum GraphInstanceFailure;
            const auto declarations = assets.program->Parameters();
            if (assets.curves.size() > declarations.size())
                return LimitExceeded;
            std::uint64_t work = 0;
            for (std::size_t index = 0; index < assets.curves.size(); ++index) {
                const auto &curve = assets.curves[index];
                if (!curve)
                    return BindingMismatch;
                // Moving a prepared value can retain its binding while retiring its key storage.
                if (curve->Keys().empty())
                    return Malformed;
                const auto found = std::ranges::lower_bound(declarations, curve->Parameter(), {}, &GraphParameter::id);
                if (found == declarations.end() || found->id != curve->Parameter())
                    return BindingMismatch;
                if (found->type != GraphValueType::Float)
                    return TypeMismatch;
                for (std::size_t earlier = 0; earlier < index; ++earlier)
                    if (assets.curves[earlier]->Parameter() == curve->Parameter())
                        return Malformed;
                work += 1 + std::bit_width(curve->Keys().size());
            }
            return work > context.maximumJointOperations ? LimitExceeded : None;
        }

        /** @brief Charges every retained workspace and both transaction/publication buffers without overflow. */
        GraphInstanceFailure AdmitStorage(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
            const auto &memory = assets.program->Memory();
            std::uint64_t bytes = sizeof(Detail::GraphInstanceState);
            const auto charge = [&bytes, &context](std::uint64_t count, std::uint64_t stride) {
                if (count > (std::numeric_limits<std::uint64_t>::max() - bytes) / stride)
                    return false;
                bytes += count * stride;
                return bytes <= context.maximumStorageBytes;
            };
            if (!charge(memory.poseTransforms + 2ULL * assets.program->SkeletonBinding().joints, sizeof(Math::Transform)) ||
                !charge(memory.poseTransforms, sizeof(std::uint8_t)) ||
                !charge(memory.clipPlayers, 2ULL * sizeof(Detail::GraphPlayerState)) ||
                !charge(memory.parameters, 4ULL * sizeof(GraphParameterValue) + sizeof(std::uint8_t)) ||
                !charge(assets.clips.size(), sizeof(std::shared_ptr<const AnimationClipAsset>)) ||
                !charge(assets.curves.size(), sizeof(std::shared_ptr<const AnimationParameterCurve>) + sizeof(std::size_t)))
                return GraphInstanceFailure::LimitExceeded;
            if (!charge(memory.stateMachines, 2ULL * sizeof(Detail::GraphMachineState) + sizeof(Detail::GraphPreparedMachine)) ||
                !charge(memory.conditionLatches, 3ULL * sizeof(std::uint8_t) + sizeof(Detail::StateMachineCondition)) ||
                !charge(memory.stateEvents, 3ULL * sizeof(GraphStateEvent)))
                return GraphInstanceFailure::LimitExceeded;
            for (const auto &binding : assets.program->StateMachines()) {
                const auto &occurrence = assets.program->Occurrences()[binding.beginOccurrence];
                const auto &machine = std::get<GraphStateMachineNode>(
                    assets.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex].payload);
                if (!charge(machine.states.size(), sizeof(Detail::StateMachineState)) ||
                    !charge(machine.transitions.size(), sizeof(Detail::StateMachineTransition)))
                    return GraphInstanceFailure::LimitExceeded;
            }
            for (std::size_t type = 1; type < memory.workingSlots.size(); ++type)
                if (!charge(memory.workingSlots[type], sizeof(GraphParameterValue)))
                    return GraphInstanceFailure::LimitExceeded;
            return GraphInstanceFailure::None;
        }

        /** @brief Checks identity, immutable compatibility and complete storage before any runtime allocation. */
        GraphInstanceFailure Admit(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
            using enum GraphInstanceFailure;
            if (context.cancellation.IsCancellationRequested())
                return Cancelled;
            if (!context.instance.IsValid())
                return HandleMalformed;
            if (!assets.program || !assets.skeleton || assets.program->Occurrences().empty())
                return BindingMismatch;
            const auto &binding = assets.program->SkeletonBinding();
            if (const auto &skeleton = assets.skeleton->Data();
                binding.id != skeleton.skeleton || binding.generation != assets.skeletonGeneration ||
                binding.contractVersion != skeleton.contractVersion || binding.joints != skeleton.joints.size())
                return BindingMismatch;
            if (context.maximumInstructions == 0 || context.maximumInstructions > AnimationGraphHardLimits::EvaluationInstructions ||
                assets.program->Occurrences().size() > context.maximumInstructions)
                return LimitExceeded;
            if (assets.clips.size() != assets.program->ClipBindings().size())
                return BindingMismatch;
            if (!ValidClipLimits(context.clipLimits))
                return LimitExceeded;
            if (const auto failure = AdmitStorage(assets, context); failure != None)
                return failure;
            if (const auto failure = AdmitClipBindings(assets, context.clipLimits); failure != None)
                return failure;
            if (const auto failure = AdmitCurves(assets, context); failure != None)
                return failure;
            if (context.maximumControlOperations > 64ULL * 1024ULL * 1024ULL)
                return LimitExceeded;
            std::uint64_t control = assets.program->Occurrences().size() * (4ULL * assets.program->Memory().callDepth + 4ULL);
            control += 3ULL * assets.program->Memory().conditionLatches + 3ULL * assets.program->Memory().stateEvents;
            for (const auto &binding : assets.program->StateMachines()) {
                const auto &occurrence = assets.program->Occurrences()[binding.beginOccurrence];
                const auto &machine = std::get<GraphStateMachineNode>(
                    assets.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex].payload);
                control += 2ULL * machine.transitions.size() + assets.program->Parameters().size();
            }
            if (control > context.maximumControlOperations)
                return LimitExceeded;
            return AdmitClipWork(assets, context);
        }

        /** @brief Allocates all scalar, pose, control and publication workspaces before the instance can execute. */
        GraphInstanceFailure PrepareWorkspace(Detail::GraphInstanceState &state, const GraphInstanceAssets &assets) {
            const auto &memory = state.program->Memory();
            state.poses.transforms.resize(static_cast<std::size_t>(memory.poseTransforms));
            state.poses.coverage.resize(static_cast<std::size_t>(memory.poseTransforms));
            std::size_t scalars = 0;
            for (std::size_t type = 1; type < memory.workingSlots.size(); ++type) {
                state.scalars.offsets[type] = scalars;
                scalars += memory.workingSlots[type];
            }
            state.scalars.values.resize(scalars);
            state.players.resize(memory.clipPlayers);
            state.candidate.players.resize(memory.clipPlayers);
            if (const auto failure = Detail::PrepareGraphStateMachines(state); failure != GraphInstanceFailure::None)
                return failure;
            state.parameters.resize(memory.parameters);
            state.candidate.parameters.resize(memory.parameters);
            state.queuedTriggers.resize(memory.parameters);
            for (auto &parameters : state.publishedParameters)
                parameters.resize(memory.parameters);
            state.curves.publications.assign(assets.curves.begin(), assets.curves.end());
            state.curves.parameters.reserve(assets.curves.size());
            for (const auto &curve : assets.curves) {
                const auto declarations = state.program->Parameters();
                const auto found = std::ranges::lower_bound(declarations, curve->Parameter(), {}, &GraphParameter::id);
                state.curves.parameters.push_back(static_cast<std::size_t>(found - declarations.begin()));
            }
            for (auto &pose : state.publishedPoses)
                pose.resize(state.program->SkeletonBinding().joints);
            return GraphInstanceFailure::None;
        }

        /** @brief Owner-thread gate does not inspect mutable runtime state from another thread. */
        GraphInstanceOutcome Owner(const Detail::GraphInstanceState *state) noexcept {
            if (!state)
                return {GraphInstanceFailure::AdmissionRejected};
            if (state->ownerThread != std::this_thread::get_id())
                return {GraphInstanceFailure::ThreadViolation};
            return {};
        }

        /** @brief Fences all copied publications before reading mutable handle metadata or caller outputs. */
        GraphInstanceOutcome ResidentPublication(const Detail::GraphInstanceState *state, const PoseHandle &publication) noexcept {
            if (const auto owner = Owner(state); owner.HasError())
                return owner;
            if (!publication.IsValid())
                return {GraphInstanceFailure::HandleMalformed};
            if (publication.instance != state->identity || publication.slot.index >= state->publishedHandles.size() ||
                publication != state->publishedHandles[publication.slot.index])
                return {GraphInstanceFailure::HandleStale};
            return {};
        }
    }  // namespace

    /** @copydoc DescribeGraphInstanceFailure */
    Error DescribeGraphInstanceFailure(const GraphInstanceOutcome &outcome) {
        if (outcome.failure == GraphInstanceFailure::ClipFailure)
            return DescribeAnimationClipFailure(outcome.clipFailure);
        return MakeError(FailureDescriptor(outcome.failure));
    }

    /** @copydoc AnimationGraphInstance::Create */
    Result<AnimationGraphInstance> AnimationGraphInstance::Create(const GraphInstanceAssets &assets,
                                                                  const GraphInstanceCreateContext &context) {
        if (const auto failure = Admit(assets, context); failure != GraphInstanceFailure::None)
            return Result<AnimationGraphInstance>::Failure(DescribeGraphInstanceFailure({failure}));
        try {
            auto state = std::make_unique<Detail::GraphInstanceState>();
            state->identity = context.instance;
            state->program = assets.program;
            state->skeleton = assets.skeleton;
            state->clipLimits = context.clipLimits;
            state->clips.reserve(assets.clips.size());
            state->ownerCancellation = context.cancellation;
            for (const auto &expected : state->program->ClipBindings()) {
                const auto found = std::ranges::find_if(assets.clips, [&expected](const auto &clip) {
                    return clip->Data().descriptor == expected;
                });
                state->clips.push_back(*found);
            }
            if (const auto failure = PrepareWorkspace(*state, assets); failure != GraphInstanceFailure::None)
                return Result<AnimationGraphInstance>::Failure(DescribeGraphInstanceFailure({failure}));
            if (context.cancellation.IsCancellationRequested())
                return Result<AnimationGraphInstance>::Failure(DescribeGraphInstanceFailure({GraphInstanceFailure::Cancelled}));
            return Result<AnimationGraphInstance>::Success(AnimationGraphInstance{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Result<AnimationGraphInstance>::Failure(DescribeGraphInstanceFailure({GraphInstanceFailure::LimitExceeded}));
        }
    }

    /** @copydoc AnimationGraphInstance::AnimationGraphInstance */
    AnimationGraphInstance::AnimationGraphInstance(std::unique_ptr<Detail::GraphInstanceState> state) noexcept : state_(std::move(state)) {}

    AnimationGraphInstance::AnimationGraphInstance(AnimationGraphInstance &&) noexcept = default;
    AnimationGraphInstance &AnimationGraphInstance::operator=(AnimationGraphInstance &&) noexcept = default;
    AnimationGraphInstance::~AnimationGraphInstance() = default;

    /** @copydoc AnimationGraphInstance::Replace */
    Result<void> AnimationGraphInstance::Replace(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) {
        if (const auto owner = Owner(state_.get()); owner.HasError())
            return Result<void>::Failure(DescribeGraphInstanceFailure(owner));
        const auto &old = state_->identity;
        if (state_->closed || state_->candidate.tick.IsValid())
            return Result<void>::Failure(DescribeGraphInstanceFailure({GraphInstanceFailure::StateInvalid}));
        if (context.instance.owner != old.owner || context.instance.component != old.component ||
            context.instance.slot.index != old.slot.index || old.slot.generation == std::numeric_limits<std::uint32_t>::max() ||
            context.instance.slot.generation != old.slot.generation + 1)
            return Result<void>::Failure(DescribeGraphInstanceFailure({GraphInstanceFailure::HandleStale}));
        if (!assets.program || assets.program->Id() != state_->program->Id())
            return Result<void>::Failure(MakeError(AnimationErrors::GraphReloadMismatch));
        auto candidate = Create(assets, context);
        if (candidate.HasError())
            return Result<void>::Failure(std::move(candidate).ErrorValue());
        state_ = std::move(candidate).Value().state_;
        return Result<void>::Success();
    }

    /** @copydoc AnimationGraphInstance::Evaluate */
    GraphInstanceOutcome AnimationGraphInstance::Evaluate(const GraphEvaluationInput &input) {
        if (const auto owner = Owner(state_.get()); owner.HasError())
            return owner;
        if (state_->closed)
            return {GraphInstanceFailure::AdmissionRejected};
        if (state_->ownerCancellation.IsCancellationRequested() || input.cancellation.IsCancellationRequested())
            return {GraphInstanceFailure::Cancelled};
        if (state_->candidate.tick.IsValid())
            return {GraphInstanceFailure::StateInvalid};
        if (!input.tick.IsValid() || input.tick <= state_->publication.tick)
            return {GraphInstanceFailure::TickStale};
        if (state_->nextPoseGeneration == 0 || state_->nextPoseGeneration > std::numeric_limits<std::uint32_t>::max())
            return {GraphInstanceFailure::LimitExceeded};
        if (const auto executed = Detail::ExecuteGraph(*state_, input); executed.HasError())
            return executed;
        state_->candidate.cancellation = input.cancellation;
        state_->candidate.tick = input.tick;
        return {};
    }

    /** @copydoc AnimationGraphInstance::Commit */
    GraphInstanceOutcome AnimationGraphInstance::Commit(const AnimationTickId tick) {
        if (const auto owner = Owner(state_.get()); owner.HasError())
            return owner;
        if (state_->closed)
            return {GraphInstanceFailure::AdmissionRejected};
        if (!tick.IsValid() || tick != state_->candidate.tick)
            return {GraphInstanceFailure::TickStale};
        if (state_->ownerCancellation.IsCancellationRequested() || state_->candidate.cancellation.IsCancellationRequested()) {
            state_->candidate.tick = {};
            return {GraphInstanceFailure::Cancelled};
        }
        const auto generation = state_->nextPoseGeneration;
        const auto slot = static_cast<std::uint32_t>((generation - 1) % 2);
        const PoseHandle handle{state_->identity,
                                {slot, static_cast<std::uint32_t>(generation)},
                                PoseGeneration::Create(generation).Value()};
        std::ranges::copy(state_->Pose(state_->program->OutputSlot()), state_->publishedPoses[slot].begin());
        std::ranges::copy(state_->candidate.parameters, state_->publishedParameters[slot].begin());
        std::ranges::fill(state_->queuedTriggers, 0);
        state_->publishedHandles[slot] = handle;
        state_->players.swap(state_->candidate.players);
        state_->machines.swap(state_->candidate.machines);
        state_->latches.swap(state_->candidate.latches);
        state_->publishedEventCounts[slot] = state_->candidate.eventCount;
        std::copy_n(state_->candidate.events.begin(), state_->candidate.eventCount, state_->publishedEvents[slot].begin());
        for (std::size_t index = 0; index < state_->machines.size(); ++index) {
            if (!state_->machines[index].visited)
                continue;
            const auto &binding = state_->program->StateMachines()[index];
            std::ranges::copy(state_->Pose(binding.poses[1]), state_->Pose(binding.poses[0]).begin());
            std::ranges::copy(state_->Pose(binding.poses[3]), state_->Pose(binding.poses[2]).begin());
            std::ranges::copy(state_->Coverage(binding.poses[1]), state_->Coverage(binding.poses[0]).begin());
            std::ranges::copy(state_->Coverage(binding.poses[3]), state_->Coverage(binding.poses[2]).begin());
        }
        state_->parameters.swap(state_->candidate.parameters);
        state_->publication = {tick, state_->publication.currentPose, handle};
        state_->nextPoseGeneration = generation == std::numeric_limits<std::uint64_t>::max() ? 0 : generation + 1;
        state_->candidate.tick = {};
        state_->candidate.cancellation = {};
        return {GraphInstanceFailure::None, {}, state_->publication};
    }

    /** @copydoc AnimationGraphInstance::Abort */
    GraphInstanceOutcome AnimationGraphInstance::Abort() {
        if (const auto owner = Owner(state_.get()); owner.HasError())
            return owner;
        state_->candidate.tick = {};
        state_->candidate.cancellation = {};
        state_->candidate.eventCount = 0;
        return {};
    }

    /** @copydoc AnimationGraphInstance::QueueTrigger */
    GraphInstanceOutcome AnimationGraphInstance::QueueTrigger(const GraphParameterId parameter) {
        if (const auto owner = Owner(state_.get()); owner.HasError())
            return owner;
        if (state_->closed)
            return {GraphInstanceFailure::AdmissionRejected};
        if (state_->candidate.tick.IsValid())
            return {GraphInstanceFailure::StateInvalid};
        if (state_->ownerCancellation.IsCancellationRequested())
            return {GraphInstanceFailure::Cancelled};
        const auto declarations = state_->program->Parameters();
        const auto found = std::ranges::lower_bound(declarations, parameter, {}, &GraphParameter::id);
        if (!parameter.IsValid() || found == declarations.end() || found->id != parameter)
            return {GraphInstanceFailure::BindingMismatch};
        if (found->type != GraphValueType::Trigger)
            return {GraphInstanceFailure::TypeMismatch};
        state_->queuedTriggers[static_cast<std::size_t>(found - declarations.begin())] = 1;
        return {};
    }

    /** @copydoc AnimationGraphInstance::CopyParameters */
    GraphInstanceOutcome AnimationGraphInstance::CopyParameters(const PoseHandle &publication,
                                                                const std::span<GraphParameterInput> output) const {
        if (const auto resident = ResidentPublication(state_.get(), publication); resident.HasError())
            return resident;
        const auto declarations = state_->program->Parameters();
        if (output.size() != declarations.size())
            return {GraphInstanceFailure::LimitExceeded};
        for (std::size_t index = 0; index < output.size(); ++index)
            output[index] = {declarations[index].id, state_->publishedParameters[publication.slot.index][index]};
        return {};
    }

    /** @copydoc AnimationGraphInstance::CopyStateEvents */
    GraphInstanceOutcome AnimationGraphInstance::CopyStateEvents(const PoseHandle &publication, const std::span<GraphStateEvent> output,
                                                                 std::uint32_t &written) const {
        if (const auto resident = ResidentPublication(state_.get(), publication); resident.HasError())
            return resident;
        const auto count = state_->publishedEventCounts[publication.slot.index];
        if (output.size() < count)
            return {GraphInstanceFailure::LimitExceeded};
        std::copy_n(state_->publishedEvents[publication.slot.index].begin(), count, output.begin());
        written = count;
        return {};
    }

    /** @copydoc AnimationGraphInstance::CopyPose */
    GraphInstanceOutcome AnimationGraphInstance::CopyPose(const PoseHandle &pose, const std::span<Math::Transform> output) const {
        if (const auto resident = ResidentPublication(state_.get(), pose); resident.HasError())
            return resident;
        if (output.size() != state_->program->SkeletonBinding().joints)
            return {GraphInstanceFailure::LimitExceeded};
        std::ranges::copy(state_->publishedPoses[pose.slot.index], output.begin());
        return {};
    }

    /** @copydoc AnimationGraphInstance::Publication */
    GraphInstancePublication AnimationGraphInstance::Publication() const noexcept {
        if (!state_ || state_->ownerThread != std::this_thread::get_id())
            return {};
        return state_->publication;
    }

    /** @copydoc AnimationGraphInstance::Shutdown */
    GraphInstanceOutcome AnimationGraphInstance::Shutdown() {
        if (const auto owner = Owner(state_.get()); owner.HasError())
            return owner;
        state_->closed = true;
        std::ranges::fill(state_->queuedTriggers, 0);
        state_->candidate.tick = {};
        state_->candidate.cancellation = {};
        state_->candidate.eventCount = 0;
        return {};
    }
}  // namespace Horo::Animation
