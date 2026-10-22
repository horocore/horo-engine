#include "Horo/Editor/WorldStreamingPreviewRuntime.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        /** @brief Creates non-zero sequence seeds and already-validated host-issued identities. */
        template <typename T> T Id(const std::uint64_t value = 1) {
            return T::Create(value).Value();
        }

        /** @brief Checks complete native/document lifetime identities before any ownership is consumed. */
        bool ValidMountIdentity(const WS::StreamingRuntimeOwnerToken &owner, const WorldDocumentFence &document) {
            return owner.IsValid() && document.owner.IsValid() && document.generation.IsValid() && document.revision.IsValid();
        }

        /** @brief Creates actual scheduler ceilings for an inspection owner that admits no load/activation operations. */
        WS::StreamingSchedulerAdmissionLimits SchedulerLimits() {
            return {.concurrentOperations = 1,
                    .capacityUnits = 1,
                    .concurrency = {.profile = WS::WorldPartitionProjectProfile::Editor,
                                    .revision = Id<WS::StreamingConcurrencyRevision>(),
                                    .loads = 1,
                                    .activations = 1,
                                    .retirements = 1}};
        }

        /** @brief Converts canonical authored intent into a native planner request without inventing motion samples. */
        WS::StreamingSourceRequest Request(const PartitionedWorldSource &source) {
            return {.id = source.id,
                    .revision = source.revision,
                    .intent = source.intent,
                    .priority = source.priority,
                    .shape = source.shape,
                    .residency = source.target,
                    .retention = WS::StreamingRetention::Releasable};
        }
    }  // namespace

    /** @copydoc MakeWorldStreamingPreviewRuntimePolicy */
    Result<WorldStreamingPreviewRuntimePolicy> MakeWorldStreamingPreviewRuntimePolicy() {
        WS::StreamingPriorityPolicyRequest priority{.id = Id<WS::StreamingPriorityPolicyId>(),
                                                    .revision = Id<WS::StreamingPriorityPolicyRevision>()};
        WS::StreamingCellStabilityPolicyRequest stability{.id = Id<WS::StreamingCellStabilityPolicyId>(),
                                                          .revision = Id<WS::StreamingCellStabilityPolicyRevision>(),
                                                          .maximumTrackedCells = 4096};
        WS::StreamingPrefetchPolicyRequest prefetch{.id = Id<WS::StreamingPrefetchPolicyId>(),
                                                    .revision = Id<WS::StreamingPrefetchPolicyRevision>()};
        const auto priorityPolicy = WS::StreamingPriorityPolicy::Create(priority);
        if (priorityPolicy.HasError())
            return Result<WorldStreamingPreviewRuntimePolicy>::Failure(priorityPolicy.ErrorValue());
        const auto stabilityPolicy = WS::StreamingCellStabilityPolicy::Create(stability);
        if (stabilityPolicy.HasError())
            return Result<WorldStreamingPreviewRuntimePolicy>::Failure(stabilityPolicy.ErrorValue());
        const auto prefetchPolicy = WS::StreamingPrefetchPolicy::Create(prefetch);
        if (prefetchPolicy.HasError())
            return Result<WorldStreamingPreviewRuntimePolicy>::Failure(prefetchPolicy.ErrorValue());
        return Result<WorldStreamingPreviewRuntimePolicy>::Success(
            {.planner = {.limits = {1024, 4096, 16384, 32, 128, 256, 256, 60000},
                         .shapeSupport = {true, true, true, true},
                         .priority = priorityPolicy.Value(),
                         .stability = stabilityPolicy.Value(),
                         .prefetch = prefetchPolicy.Value()},
             .artifact = {.maximumArtifactBytes = 8 * 1024 * 1024,
                          .maximumStringBytes = 65536,
                          .maximumVolumes = 1024,
                          .partition = {256, 4096, 65536},
                          .cooked = {4096, 4096, 65536, std::numeric_limits<std::uint64_t>::max(),
                                     std::numeric_limits<std::uint64_t>::max()}}});
    }

    /** @copydoc WorldStreamingPreviewRuntime::Create */
    Result<std::unique_ptr<WorldStreamingPreviewRuntime>> WorldStreamingPreviewRuntime::Create(
        WS::StreamingRuntimeOwnerToken owner, WorldDocumentFence document, WS::WorldPartitionDescriptor &&descriptor,
        const WorldStreamingPreviewRuntimePolicy &policy) {
        using Output = std::unique_ptr<WorldStreamingPreviewRuntime>;
        if (!ValidMountIdentity(owner, document))
            return Result<Output>::Failure(MakeError(WorldDocumentErrors::Invalid));
        if (owner.partition != descriptor.Partition())
            return Result<Output>::Failure(MakeError(WorldDocumentErrors::Stale));
        auto registry = WS::WorldPartitionRegistry::Create(Id<WS::WorldPartitionRegistryId>(owner.owner.Value()), owner, {4096, 4096});
        if (registry.HasError())
            return Result<Output>::Failure(registry.ErrorValue());
        const auto published = registry.Value()->Publish(std::move(descriptor), Id<WS::WorldPartitionRegistryRevision>());
        if (published.HasError())
            return Result<Output>::Failure(published.ErrorValue());
        const auto partition = registry.Value()->Snapshot();
        if (partition.HasError())
            return Result<Output>::Failure(partition.ErrorValue());
        std::vector<WS::StreamingLayerId> enabled;
        for (const auto &layer : partition.Value().Layers())
            enabled.push_back(layer.id);
        const WS::StreamingSourceEnvironment environment{1, {1}, 1, 0, enabled};
        auto planner = WS::StreamingSourcePlanner::Create(policy.planner, partition.Value(), environment);
        if (planner.HasError())
            return Result<Output>::Failure(planner.ErrorValue());
        auto scheduler =
            WS::StreamingSchedulerAdmissionLedger::Create(Id<WS::StreamingSchedulerLedgerId>(owner.owner.Value()), SchedulerLimits());
        if (scheduler.HasError())
            return Result<Output>::Failure(scheduler.ErrorValue());
        auto residency = WS::StreamingCellStateLedger::Create({owner, 1024});
        if (residency.HasError())
            return Result<Output>::Failure(residency.ErrorValue());
        return Result<Output>::Success(
            Output{new WorldStreamingPreviewRuntime{owner, document, std::move(registry).Value(), std::move(planner).Value(),
                                                    std::make_unique<WS::StreamingSchedulerAdmissionLedger>(std::move(scheduler).Value()),
                                                    std::make_unique<WS::StreamingCellStateLedger>(std::move(residency).Value()),
                                                    policy.planner.limits.sources}});
    }

    /** @copydoc WorldStreamingPreviewRuntime::WorldStreamingPreviewRuntime */
    WorldStreamingPreviewRuntime::WorldStreamingPreviewRuntime(WS::StreamingRuntimeOwnerToken owner, WorldDocumentFence document,
                                                               std::unique_ptr<WS::WorldPartitionRegistry> registry,
                                                               std::unique_ptr<WS::StreamingSourcePlanner> planner,
                                                               std::unique_ptr<WS::StreamingSchedulerAdmissionLedger> scheduler,
                                                               std::unique_ptr<WS::StreamingCellStateLedger> residency,
                                                               const std::uint32_t maximumSources)
        : owner_(owner), document_(document), registry_(std::move(registry)), planner_(std::move(planner)),
          scheduler_(std::move(scheduler)), residency_(std::move(residency)), maximumSources_(maximumSources) {
        sources_.reserve(maximumSources);
    }

    /** @copydoc WorldStreamingPreviewRuntime::~WorldStreamingPreviewRuntime */
    WorldStreamingPreviewRuntime::~WorldStreamingPreviewRuntime() {
        static_cast<void>(Shutdown());
    }

    /** @copydoc WorldStreamingPreviewRuntime::Synchronize */
    Result<bool> WorldStreamingPreviewRuntime::Synchronize(const PartitionedWorldSnapshotLease &source) {
        if (!source || source->document.settings.partition != owner_.partition || source->fence.owner != document_.owner ||
            source->fence.generation != document_.generation || source->fence.revision.Value() < document_.revision.Value())
            return Result<bool>::Failure(MakeError(WorldDocumentErrors::Stale));
        if (source->document.sources.size() > maximumSources_)
            return Result<bool>::Failure(MakeError(WorldDocumentErrors::Capacity));
        const bool same = std::ranges::equal(sources_, source->document.sources, {}, &Source::authored);
        document_ = source->fence;
        if (same)
            return Result<bool>::Success(false);
        sources_.clear();  // Actual RAII withdrawal invalidates the canonical planner publication before replacement.
        for (const auto &authored : source->document.sources) {
            auto registration = planner_->Register(Request(authored));
            if (registration.HasError()) {
                sources_.clear();
                return Result<bool>::Failure(registration.ErrorValue());
            }
            sources_.push_back({authored, std::move(registration).Value()});
        }
        return Result<bool>::Success(true);
    }

    /** @copydoc WorldStreamingPreviewRuntime::PublishDiagnostics */
    Result<void> WorldStreamingPreviewRuntime::PublishDiagnostics() {
        if (diagnosticRevision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Exhausted));
        std::vector<WS::StreamingSourceDesiredState> sources;
        sources.reserve(sources_.size());
        for (const auto &source : sources_) {
            const WS::StreamingSourceDescriptor descriptor{source.authored.id, source.registration.Owner(), source.authored.intent,
                                                           source.authored.priority, source.authored.revision};
            const auto desired =
                WS::StreamingSourceDesiredState::Create(descriptor, source.authored.target, WS::StreamingRetention::Releasable);
            if (desired.HasError())
                return Result<void>::Failure(desired.ErrorValue());
            sources.push_back(desired.Value());
        }
        const WS::WorldStreamingDiagnosticSnapshotInput input{.owner = owner_,
                                                              .revision = Id<WS::WorldStreamingDiagnosticRevision>(diagnosticRevision_ + 1),
                                                              .ownerRevision = Id<WS::StreamingRuntimeCompositionRevision>(),
                                                              .lifecycle = WS::WorldStreamingRuntimeCompositionState::Active,
                                                              .instrumentation = WS::StreamingDiagnosticInstrumentation::Disabled,
                                                              .limits = {1024, 1024, 1024},
                                                              .queue = {scheduler_->Owner(), scheduler_->Limits(), scheduler_->State(),
                                                                        static_cast<std::uint32_t>(scheduler_->ReservedCount()),
                                                                        scheduler_->ReservedCapacityUnits()}};
        const auto captured =
            WS::WorldStreamingDiagnosticSnapshot::CreateStateOnly(input, {.sources = sources, .cells = residency_->Records()});
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        diagnostics_ = captured.Value();
        ++diagnosticRevision_;
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingPreviewRuntime::CanReusePublication */
    bool WorldStreamingPreviewRuntime::CanReusePublication(const bool sourceChanged,
                                                           const std::chrono::steady_clock::time_point now) const {
        return !sourceChanged && diagnostics_.has_value() && serviced_ != std::chrono::steady_clock::time_point{} &&
               now - serviced_ < std::chrono::milliseconds(100) && planner_->Plan().HasValue();
    }

    /** @copydoc WorldStreamingPreviewRuntime::Service */
    Result<bool> WorldStreamingPreviewRuntime::Service(const PartitionedWorldSnapshotLease &source) {
        if (std::this_thread::get_id() != lane_)
            return Result<bool>::Failure(MakeError(WorldDocumentErrors::WrongThread));
        if (closed_)
            return Result<bool>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
        const auto synchronized = Synchronize(source);
        if (synchronized.HasError())
            return synchronized;
        const auto now = std::chrono::steady_clock::now();
        if (CanReusePublication(synchronized.Value(), now))
            return Result<bool>::Success(true);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - created_).count();
        if (elapsed < 0)
            return Result<bool>::Failure(MakeError(WorldDocumentErrors::Stale));
        const auto progress = planner_->Service(static_cast<std::uint64_t>(elapsed));
        if (progress.HasError())
            return Result<bool>::Failure(progress.ErrorValue());
        if (!progress.Value().published)
            return Result<bool>::Success(false);
        const auto captured = PublishDiagnostics();
        if (captured.HasError())
            return Result<bool>::Failure(captured.ErrorValue());
        serviced_ = now;
        return Result<bool>::Success(true);
    }

    /** @copydoc WorldStreamingPreviewRuntime::Shutdown */
    Result<void> WorldStreamingPreviewRuntime::Shutdown() {
        if (std::this_thread::get_id() != lane_)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::WrongThread));
        if (closed_)
            return Result<void>::Success();
        sources_.clear();
        const auto stopped = planner_->Shutdown();
        if (stopped.HasError())
            return stopped;
        scheduler_->BeginShutdown();
        const auto drained = residency_->BeginShutdown(owner_);
        if (drained.HasError())
            return drained;
        registry_->Shutdown();
        closed_ = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
