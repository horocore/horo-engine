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
            if (clip.Data().tracks.size() > limits.maximumTracks)
                return GraphInstanceFailure::LimitExceeded;
            std::uint64_t keys = 0;
            for (const auto &track : clip.Data().tracks) {
                if (track.keys.size() > limits.maximumKeysPerTrack)
                    return GraphInstanceFailure::LimitExceeded;
                keys += track.keys.size();
            }
            return keys > limits.maximumTotalKeys ? GraphInstanceFailure::LimitExceeded : GraphInstanceFailure::None;
        }

        /** @brief Proves exactly one admitted absolute publication exists for each compiled clip binding. */
        GraphInstanceFailure AdmitClipBindings(const GraphInstanceAssets &assets, const AnimationClipLimits &limits) noexcept {
            for (const auto &expected : assets.program->ClipBindings()) {
                std::size_t matches = 0;
                for (const auto &clip : assets.clips) {
                    if (!clip)
                        return GraphInstanceFailure::BindingMismatch;
                    if (clip->Data().descriptor.id != expected.id)
                        continue;
                    if (clip->Data().descriptor != expected || expected.kind != AnimationClipKind::Absolute)
                        return GraphInstanceFailure::BindingMismatch;
                    if (const auto failure = AdmitClipTracks(*clip, limits); failure != GraphInstanceFailure::None)
                        return failure;
                    ++matches;
                }
                if (matches != 1)
                    return GraphInstanceFailure::BindingMismatch;
            }
            return GraphInstanceFailure::None;
        }

        /** @brief Charges complete per-tick pose/key-search work after exact clip bindings have been admitted. */
        GraphInstanceFailure AdmitClipWork(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
            std::uint64_t operations = 0;
            for (const auto &occurrence : assets.program->Occurrences()) {
                const auto &instruction =
                    assets.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex];
                std::uint64_t cost = 0;
                if (std::holds_alternative<GraphBlendNode>(instruction.payload))
                    cost = assets.program->SkeletonBinding().joints;
                if (occurrence.clipBindingIndex) {
                    const auto &descriptor = assets.program->ClipBindings()[*occurrence.clipBindingIndex];
                    const auto clip = std::ranges::find_if(assets.clips, [&descriptor](const auto &value) {
                        return value->Data().descriptor == descriptor;
                    });
                    cost = assets.program->SkeletonBinding().joints;
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
                !charge(memory.clipPlayers, 2ULL * sizeof(Detail::GraphPlayerState)) ||
                !charge(memory.parameters, 2ULL * sizeof(GraphParameterValue)) ||
                !charge(assets.clips.size(), sizeof(std::shared_ptr<const AnimationClipAsset>)))
                return GraphInstanceFailure::LimitExceeded;
            for (std::size_t type = 1; type < memory.workingSlots.size(); ++type)
                if (!charge(memory.workingSlots[type], sizeof(GraphParameterValue)))
                    return GraphInstanceFailure::LimitExceeded;
            return GraphInstanceFailure::None;
        }

        /** @brief Checks identity, immutable compatibility and complete storage before any runtime allocation. */
        GraphInstanceFailure Admit(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
            if (context.cancellation.IsCancellationRequested())
                return GraphInstanceFailure::Cancelled;
            if (!context.instance.IsValid())
                return GraphInstanceFailure::HandleMalformed;
            if (!assets.program || !assets.skeleton || assets.program->Occurrences().empty())
                return GraphInstanceFailure::BindingMismatch;
            const auto &binding = assets.program->SkeletonBinding();
            const auto &skeleton = assets.skeleton->Data();
            if (binding.id != skeleton.skeleton || binding.generation != assets.skeletonGeneration ||
                binding.contractVersion != skeleton.contractVersion || binding.joints != skeleton.joints.size())
                return GraphInstanceFailure::BindingMismatch;
            if (context.maximumInstructions == 0 || context.maximumInstructions > AnimationGraphHardLimits::EvaluationInstructions ||
                assets.program->Occurrences().size() > context.maximumInstructions)
                return GraphInstanceFailure::LimitExceeded;
            if (assets.clips.size() != assets.program->ClipBindings().size())
                return GraphInstanceFailure::BindingMismatch;
            if (!ValidClipLimits(context.clipLimits))
                return GraphInstanceFailure::LimitExceeded;
            if (const auto failure = AdmitStorage(assets, context); failure != GraphInstanceFailure::None)
                return failure;
            if (const auto failure = AdmitClipBindings(assets, context.clipLimits); failure != GraphInstanceFailure::None)
                return failure;
            return AdmitClipWork(assets, context);
        }

        /** @brief Owner-thread gate does not inspect mutable runtime state from another thread. */
        GraphInstanceOutcome Owner(const Detail::GraphInstanceState *state) noexcept {
            if (!state)
                return {GraphInstanceFailure::AdmissionRejected};
            if (state->ownerThread != std::this_thread::get_id())
                return {GraphInstanceFailure::ThreadViolation};
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
            const auto &memory = state->program->Memory();
            state->workingPoses.resize(static_cast<std::size_t>(memory.poseTransforms));
            std::size_t scalars = 0;
            for (std::size_t type = 1; type < memory.workingSlots.size(); ++type) {
                state->scalarOffsets[type] = scalars;
                scalars += memory.workingSlots[type];
            }
            state->workingScalars.resize(scalars);
            state->players.resize(memory.clipPlayers);
            state->candidatePlayers.resize(memory.clipPlayers);
            state->parameters.resize(memory.parameters);
            state->candidateParameters.resize(memory.parameters);
            for (auto &pose : state->publishedPoses)
                pose.resize(state->program->SkeletonBinding().joints);
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
        if (state_->closed || state_->pendingTick.IsValid())
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
        if (state_->pendingTick.IsValid())
            return {GraphInstanceFailure::StateInvalid};
        if (!input.tick.IsValid() || input.tick <= state_->publication.tick)
            return {GraphInstanceFailure::TickStale};
        if (state_->nextPoseGeneration == 0 || state_->nextPoseGeneration > std::numeric_limits<std::uint32_t>::max())
            return {GraphInstanceFailure::LimitExceeded};
        const auto executed = Detail::ExecuteGraph(*state_, input);
        if (executed.HasError())
            return executed;
        state_->pendingCancellation = input.cancellation;
        state_->pendingTick = input.tick;
        return {};
    }

    /** @copydoc AnimationGraphInstance::Commit */
    GraphInstanceOutcome AnimationGraphInstance::Commit(const AnimationTickId tick) {
        if (const auto owner = Owner(state_.get()); owner.HasError())
            return owner;
        if (state_->closed)
            return {GraphInstanceFailure::AdmissionRejected};
        if (!tick.IsValid() || tick != state_->pendingTick)
            return {GraphInstanceFailure::TickStale};
        if (state_->ownerCancellation.IsCancellationRequested() || state_->pendingCancellation.IsCancellationRequested()) {
            state_->pendingTick = {};
            return {GraphInstanceFailure::Cancelled};
        }
        const auto generation = state_->nextPoseGeneration;
        const auto slot = static_cast<std::uint32_t>((generation - 1) % 2);
        const PoseHandle handle{state_->identity,
                                {slot, static_cast<std::uint32_t>(generation)},
                                PoseGeneration::Create(generation).Value()};
        std::ranges::copy(state_->Pose(state_->program->OutputSlot()), state_->publishedPoses[slot].begin());
        state_->publishedHandles[slot] = handle;
        state_->players.swap(state_->candidatePlayers);
        state_->parameters.swap(state_->candidateParameters);
        state_->publication = {tick, state_->publication.currentPose, handle};
        state_->nextPoseGeneration = generation == std::numeric_limits<std::uint64_t>::max() ? 0 : generation + 1;
        state_->pendingTick = {};
        state_->pendingCancellation = {};
        return {GraphInstanceFailure::None, {}, state_->publication};
    }

    /** @copydoc AnimationGraphInstance::Abort */
    GraphInstanceOutcome AnimationGraphInstance::Abort() {
        if (const auto owner = Owner(state_.get()); owner.HasError())
            return owner;
        state_->pendingTick = {};
        state_->pendingCancellation = {};
        return {};
    }

    /** @copydoc AnimationGraphInstance::CopyPose */
    GraphInstanceOutcome AnimationGraphInstance::CopyPose(const PoseHandle &pose, const std::span<Math::Transform> output) const {
        if (const auto owner = Owner(state_.get()); owner.HasError())
            return owner;
        if (!pose.IsValid())
            return {GraphInstanceFailure::HandleMalformed};
        if (pose.instance != state_->identity || pose.slot.index >= state_->publishedHandles.size() ||
            pose != state_->publishedHandles[pose.slot.index])
            return {GraphInstanceFailure::HandleStale};
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
        state_->pendingTick = {};
        state_->pendingCancellation = {};
        return {};
    }
}  // namespace Horo::Animation
