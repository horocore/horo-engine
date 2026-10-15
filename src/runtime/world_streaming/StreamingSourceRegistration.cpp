#include "Horo/WorldStreaming/StreamingSourcePlanner.h"
#include "StreamingSourcePlannerInternal.h"

#include <algorithm>
#include <bit>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace Horo::WorldStreaming {
    using SourcePlanning::Failure;

    StreamingSourceOwnerToken StreamingSourcePlannerState::Token(const std::size_t slot) const noexcept {
        const auto &owner = partition.Binding().owner;
        return {owner.partition, owner.epoch, static_cast<std::uint32_t>(slot), sources[slot].generation};
    }

    StreamingSourceDescriptor StreamingSourcePlannerState::Descriptor(const std::size_t slot) const noexcept {
        const auto &request = *sources[slot].request;
        return {request.id, Token(slot), request.intent, request.priority, request.revision};
    }

    Result<void> StreamingSourcePlannerState::ValidateOwner(const StreamingSourceOwnerToken &owner) const {
        if (!OnLane() || closed)
            return Failure<void>(WorldStreamingErrors::SourceLifecycleUnavailable);
        if (!owner.IsValid() || owner.slot >= sources.size() || !sources[owner.slot].request || Token(owner.slot) != owner)
            return Failure<void>(WorldStreamingErrors::SourceOwnerStale);
        return Result<void>::Success();
    }

    Result<void> StreamingSourcePlannerState::ValidateEnvironment(const StreamingSourceEnvironment &value,
                                                                  const WorldPartitionRegistrySnapshot &snapshot) const {
        if (!snapshot.IsValid() || value.revision == 0 || value.profile.value == 0 || !std::has_single_bit(value.platformBit) ||
            value.enabledLayers.size() > snapshot.Layers().size())
            return Failure<void>(WorldStreamingErrors::SourceDescriptorInvalid);
        std::optional<StreamingLayerId> previous;
        for (const auto layer : value.enabledLayers) {
            if (!layer.IsValid() || (previous && layer <= *previous) ||
                !std::ranges::binary_search(snapshot.Layers(), layer, {}, &WorldLayerDescriptor::id))
                return Failure<void>(WorldStreamingErrors::SourceDescriptorInvalid);
            previous = layer;
        }
        return Result<void>::Success();
    }

    Result<void> StreamingSourcePlannerState::ValidateRequest(const StreamingSourceRequest &request, const StreamingSourceOwnerToken &owner,
                                                              const std::optional<StreamingSourceRevision> previous) const {
        const StreamingSourceDescriptor descriptor{request.id, owner, request.intent, request.priority, request.revision};
        if (const auto desired = StreamingSourceDesiredState::Create(descriptor, request.residency, request.retention); desired.HasError())
            return Result<void>::Failure(desired.ErrorValue());
        const StreamingSourceAdmissionContext admission{owner, previous, 0, config.limits.sources, StreamingSourceOwnerState::Active};
        if (const auto valid = ValidateStreamingSourceAdmission(descriptor, admission); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateStreamingSourceShape(request.shape); valid.HasError())
            return valid;
        const auto &selection = request.selection;
        if ((selection.grid && !selection.grid->IsValid()) || (selection.profile && selection.profile->value == 0) ||
            selection.platformMask == 0 || (selection.requiredGameplayTags & selection.excludedGameplayTags) != 0)
            return Failure<void>(WorldStreamingErrors::SourceDescriptorInvalid);
        const auto bounds = StreamingSourceQueryBounds(request.shape, config.stability.ExitMarginMillimeters());
        if (bounds.HasError())
            return Result<void>::Failure(bounds.ErrorValue());
        if (const auto query = partition.BeginQuery({bounds.Value(), selection.layer, selection.lod}); query.HasError())
            return Result<void>::Failure(query.ErrorValue());
        // The existing admission/range boundary owns shape capability selection.
        if (!partition.Cells().empty()) {
            const auto supported = EvaluateStreamingSourceCellRange(descriptor, admission, request.shape, config.shapeSupport,
                                                                    *partition.Grid(), partition.Cells().front().id);
            if (supported.HasError())
                return Result<void>::Failure(supported.ErrorValue());
        }
        if (request.prediction) {
            if (request.intent != StreamingSourceIntent::Camera && request.intent != StreamingSourceIntent::Gameplay)
                return Failure<void>(WorldStreamingErrors::PrefetchUnsupported);
            const auto &sample = *request.prediction;
            if (const auto moved = SourcePlanning::Reposition(request.shape, sample.position); moved.HasError())
                return Result<void>::Failure(moved.ErrorValue());
            const StreamingPrefetchEvaluationContext context{config.prefetch.Id(), config.prefetch.Revision(),          owner.partition,
                                                             owner.epoch,          sample.sampledAtServiceMilliseconds, admission,
                                                             config.shapeSupport,  StreamingPrefetchLifecycle::Active};
            const auto prefetch =
                EvaluateStreamingVelocityPrefetch(config.prefetch, context,
                                                  {descriptor, sample.position, sample.velocity, sample.sampledAtServiceMilliseconds});
            if (prefetch.HasError())
                return Result<void>::Failure(prefetch.ErrorValue());
        }
        return Result<void>::Success();
    }

    void StreamingSourcePlannerState::Invalidate() noexcept {
        publication.publishedRevision = 0;
        cycle.cycleActive = false;
        cycle.cursor.reset();
        cycle.queryShape.reset();
        staged.clear();
        readyHeap.clear();
        nextPublished.clear();
        nextPublishedContributions.clear();
        stagedContributions.clear();
        cycle.sourceSlot = 0;
        cycle.historySlot = 0;
        cycle.predicting = false;
        if (revision == std::numeric_limits<std::uint64_t>::max())
            closed = true;
        else
            ++revision;
    }

    bool StreamingSourcePlannerState::Selected(const StreamingSourceRequest &request) const noexcept {
        const auto &selection = request.selection;
        return (!selection.grid || *selection.grid == partition.Binding().registry) &&
               (!selection.profile || *selection.profile == environment.profile) &&
               (selection.platformMask & environment.platformBit) != 0 &&
               (selection.requiredGameplayTags & environment.gameplayTags) == selection.requiredGameplayTags &&
               (selection.excludedGameplayTags & environment.gameplayTags) == 0;
    }

    /** @copydoc StreamingSourceRegistration::StreamingSourceRegistration */
    StreamingSourceRegistration::StreamingSourceRegistration(std::shared_ptr<StreamingSourcePlannerState> state,
                                                             const StreamingSourceOwnerToken &owner) noexcept
        : state_(std::move(state)), owner_(owner) {}

    /** @copydoc StreamingSourceRegistration::StreamingSourceRegistration */
    StreamingSourceRegistration::StreamingSourceRegistration(StreamingSourceRegistration &&other) noexcept
        : state_(std::move(other.state_)), owner_(std::exchange(other.owner_, {})) {}

    /** @copydoc StreamingSourceRegistration::operator= */
    StreamingSourceRegistration &StreamingSourceRegistration::operator=(StreamingSourceRegistration &&other) noexcept {
        if (this != &other) {
            if (const auto closed = Close(); closed.HasError())
                std::terminate();
            state_ = std::move(other.state_);
            owner_ = std::exchange(other.owner_, {});
        }
        return *this;
    }

    /** @copydoc StreamingSourceRegistration::~StreamingSourceRegistration */
    StreamingSourceRegistration::~StreamingSourceRegistration() noexcept {
        if (const auto closed = Close(); closed.HasError())
            std::terminate();
    }

    /** @copydoc StreamingSourceRegistration::Owner */
    const StreamingSourceOwnerToken &StreamingSourceRegistration::Owner() const noexcept {
        return owner_;
    }

    /** @copydoc StreamingSourceRegistration::OwnerState */
    StreamingSourcePlannerState *StreamingSourceRegistration::OwnerState() noexcept {
        return state_ && state_->OnLane() ? state_.get() : nullptr;
    }

    /** @copydoc StreamingSourcePlanner::OwnerState */
    StreamingSourcePlannerState *StreamingSourcePlanner::OwnerState() noexcept {
        return state_ && state_->OnLane() ? state_.get() : nullptr;
    }

    /** @copydoc StreamingSourceRegistration::Replace */
    Result<void> StreamingSourceRegistration::Replace(const StreamingSourceRequest &request) {
        auto *state = OwnerState();
        if (!state)
            return Failure<void>(state_ ? WorldStreamingErrors::SourceLifecycleUnavailable : WorldStreamingErrors::SourceOwnerStale);
        if (const auto valid = state->ValidateOwner(owner_); valid.HasError())
            return valid;
        const auto &previous = *state->sources[owner_.slot].request;
        if (request.id != previous.id || request.intent != previous.intent)
            return Failure<void>(WorldStreamingErrors::SourceOwnerStale);
        if (state->revision == std::numeric_limits<std::uint64_t>::max())
            return Failure<void>(WorldStreamingErrors::GenerationExhausted);
        if (const auto valid = state->ValidateRequest(request, owner_, previous.revision); valid.HasError())
            return valid;
        state->sources[owner_.slot].request = request;
        state->sources[owner_.slot].motion = request.prediction;
        state->Invalidate();
        return Result<void>::Success();
    }

    /** @copydoc StreamingSourceRegistration::SampleMotion */
    Result<void> StreamingSourceRegistration::SampleMotion(const StreamingSourceMotionSample &sample) {
        auto *state = OwnerState();
        if (!state)
            return Failure<void>(state_ ? WorldStreamingErrors::SourceLifecycleUnavailable : WorldStreamingErrors::SourceOwnerStale);
        if (const auto valid = state->ValidateOwner(owner_); valid.HasError())
            return valid;
        auto &source = state->sources[owner_.slot];
        if (source.request->intent == StreamingSourceIntent::Preload)
            return Failure<void>(WorldStreamingErrors::SourceIntentUnsupported);
        if (source.motion && sample.sampledAtServiceMilliseconds <= source.motion->sampledAtServiceMilliseconds)
            return Failure<void>(WorldStreamingErrors::PrefetchStale);
        if (const auto moved = SourcePlanning::Reposition(source.request->shape, sample.position); moved.HasError())
            return Result<void>::Failure(moved.ErrorValue());
        if (source.request->prediction) {
            auto candidate = *source.request;
            candidate.prediction = sample;
            if (const auto valid = state->ValidateRequest(candidate, owner_); valid.HasError())
                return valid;
        }
        source.motion = sample;
        return Result<void>::Success();
    }

    /** @copydoc StreamingSourceRegistration::Close */
    Result<void> StreamingSourceRegistration::Close() {
        if (!state_)
            return Result<void>::Success();
        if (!state_->OnLane())
            return Failure<void>(WorldStreamingErrors::SourceLifecycleUnavailable);
        if (!state_->closed && owner_.slot < state_->sources.size() && state_->Token(owner_.slot) == owner_ &&
            state_->sources[owner_.slot].request) {
            state_->sources[owner_.slot].request.reset();
            if (const auto slot = std::ranges::find(state_->sourceOrder, owner_.slot); slot != state_->sourceOrder.end())
                state_->sourceOrder.erase(slot);
            state_->Invalidate();
        }
        state_.reset();
        owner_ = {};
        return Result<void>::Success();
    }

    /** @copydoc StreamingSourcePlanner::StreamingSourcePlanner */
    StreamingSourcePlanner::StreamingSourcePlanner(std::shared_ptr<StreamingSourcePlannerState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc StreamingSourcePlanner::~StreamingSourcePlanner */
    StreamingSourcePlanner::~StreamingSourcePlanner() noexcept {
        if (const auto closed = Shutdown(); closed.HasError())
            std::terminate();
    }

    /** @copydoc StreamingSourcePlanner::Create */
    Result<std::unique_ptr<StreamingSourcePlanner>> StreamingSourcePlanner::Create(StreamingSourcePlannerConfig config,
                                                                                   WorldPartitionRegistrySnapshot partition,
                                                                                   const StreamingSourceEnvironment &environment) {
        const auto limits = config.limits;
        if (!partition.IsValid() || limits.sources == 0 || limits.sources > 1'024 || limits.cells == 0 || limits.cells > 65'536 ||
            limits.contributions == 0 || limits.contributions > 65'536 || limits.sourceVisitsPerFrame == 0 ||
            limits.sourceVisitsPerFrame > limits.sources || limits.nodesPerFrame == 0 || limits.nodesPerFrame > 1'048'576 ||
            limits.candidatesPerFrame == 0 || limits.candidatesPerFrame > 65'536 || limits.stabilityRowsPerFrame == 0 ||
            limits.stabilityRowsPerFrame > limits.cells || limits.maximumPlanAgeMilliseconds == 0 ||
            limits.maximumPlanAgeMilliseconds > 60'000 || limits.cells > config.stability.MaximumTrackedCells())
            return Failure<std::unique_ptr<StreamingSourcePlanner>>(WorldStreamingErrors::SourceDescriptorInvalid);
        try {
            auto state = std::make_shared<StreamingSourcePlannerState>(std::move(config), std::move(partition));
            if (const auto valid = state->ValidateEnvironment(environment, state->partition); valid.HasError())
                return Result<std::unique_ptr<StreamingSourcePlanner>>::Failure(valid.ErrorValue());
            state->sources.resize(limits.sources);
            state->sourceOrder.reserve(limits.sources);
            state->staged.reserve(limits.cells);
            state->published.reserve(limits.cells);
            state->nextPublished.reserve(limits.cells);
            state->lookup.resize(state->partition.Cells().size());
            state->readyHeap.reserve(limits.cells);
            state->stagedContributions.reserve(limits.contributions);
            state->publishedContributions.reserve(limits.contributions);
            state->nextPublishedContributions.reserve(limits.contributions);
            state->enabledLayers.assign(environment.enabledLayers.begin(), environment.enabledLayers.end());
            state->environment = environment;
            state->environment.enabledLayers = state->enabledLayers;
            auto planner =
                std::unique_ptr<StreamingSourcePlanner>(  // NOSONAR(cpp:S5950) Sealed private constructor; immediate RAII ownership.
                    new StreamingSourcePlanner(std::move(state)));
            return Result<std::unique_ptr<StreamingSourcePlanner>>::Success(std::move(planner));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<StreamingSourcePlanner>>(WorldStreamingErrors::PartitionRegistryStorageUnavailable);
        }
    }

    /** @copydoc StreamingSourcePlanner::Register */
    Result<StreamingSourceRegistration> StreamingSourcePlanner::Register(const StreamingSourceRequest &request) {
        auto *state = OwnerState();
        if (!state || state->closed)
            return Failure<StreamingSourceRegistration>(WorldStreamingErrors::SourceLifecycleUnavailable);
        if (state->revision == std::numeric_limits<std::uint64_t>::max())
            return Failure<StreamingSourceRegistration>(WorldStreamingErrors::GenerationExhausted);
        std::optional<std::size_t> available;
        for (std::size_t slot = 0; slot < state->sources.size(); ++slot) {
            const auto &source = state->sources[slot];
            if (source.request && source.request->id == request.id)
                return Failure<StreamingSourceRegistration>(WorldStreamingErrors::SourceReductionIdentityConflict);
            if (!source.request && source.generation != std::numeric_limits<std::uint32_t>::max() && !available.has_value())
                available = slot;
        }
        if (!available.has_value())
            return Failure<StreamingSourceRegistration>(WorldStreamingErrors::SourceCapacityExceeded);
        auto token = state->Token(*available);
        ++token.generation;
        if (const auto valid = state->ValidateRequest(request, token); valid.HasError())
            return Result<StreamingSourceRegistration>::Failure(valid.ErrorValue());
        auto &source = state->sources[*available];
        source.request = request;
        source.motion = request.prediction;
        source.generation = token.generation;
        const auto position = std::ranges::lower_bound(state->sourceOrder, request.id, {}, [&](const auto slot) {
            return state->sources[slot].request->id;
        });
        state->sourceOrder.insert(position, static_cast<std::uint32_t>(*available));
        state->Invalidate();
        return Result<StreamingSourceRegistration>::Success(StreamingSourceRegistration{state_, token});
    }

    /** @copydoc StreamingSourcePlanner::ReplaceEnvironment */
    Result<void> StreamingSourcePlanner::ReplaceEnvironment(const StreamingSourceEnvironment &environment) {
        auto *state = OwnerState();
        if (!state || state->closed)
            return Failure<void>(WorldStreamingErrors::SourceLifecycleUnavailable);
        if (environment.revision <= state->environment.revision)
            return Failure<void>(WorldStreamingErrors::SourceRevisionStale);
        if (state->revision == std::numeric_limits<std::uint64_t>::max())
            return Failure<void>(WorldStreamingErrors::GenerationExhausted);
        if (const auto valid = state->ValidateEnvironment(environment, state->partition); valid.HasError())
            return valid;
        try {
            std::vector<StreamingLayerId> layers(environment.enabledLayers.begin(), environment.enabledLayers.end());
            state->enabledLayers.swap(layers);
            state->environment = environment;
            state->environment.enabledLayers = state->enabledLayers;
            state->Invalidate();
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure<void>(WorldStreamingErrors::PartitionRegistryStorageUnavailable);
        }
    }

    /** @copydoc StreamingSourcePlanner::ReplacePartition */
    Result<void> StreamingSourcePlanner::ReplacePartition(WorldPartitionRegistrySnapshot partition,
                                                          const StreamingSourceEnvironment &environment) {
        auto *state = OwnerState();
        if (!state || state->closed)
            return Failure<void>(WorldStreamingErrors::SourceLifecycleUnavailable);
        if (partition.Binding() == state->partition.Binding())
            return Failure<void>(WorldStreamingErrors::PartitionRegistryStale);
        if (state->revision == std::numeric_limits<std::uint64_t>::max())
            return Failure<void>(WorldStreamingErrors::GenerationExhausted);
        if (const auto valid = state->ValidateEnvironment(environment, partition); valid.HasError())
            return valid;
        try {
            std::vector<StreamingLayerId> layers(environment.enabledLayers.begin(), environment.enabledLayers.end());
            std::vector<StreamingSourcePlannerState::Lookup> lookup(partition.Cells().size());
            state->lookup.swap(lookup);
            state->partition = std::move(partition);
            state->enabledLayers.swap(layers);
            state->environment = environment;
            state->environment.enabledLayers = state->enabledLayers;
            for (auto &source : state->sources)
                source.request.reset();
            state->sourceOrder.clear();
            state->published.clear();
            state->Invalidate();
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure<void>(WorldStreamingErrors::PartitionRegistryStorageUnavailable);
        }
    }

    /** @copydoc StreamingSourcePlanner::Shutdown */
    Result<void> StreamingSourcePlanner::Shutdown() {
        auto *state = OwnerState();
        if (!state)
            return Failure<void>(WorldStreamingErrors::SourceLifecycleUnavailable);
        state->closed = true;
        for (auto &source : state->sources)
            source.request.reset();
        state->sourceOrder.clear();
        state->published.clear();
        state->Invalidate();
        return Result<void>::Success();
    }
}  // namespace Horo::WorldStreaming
