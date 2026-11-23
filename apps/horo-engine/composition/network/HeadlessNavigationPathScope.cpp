#include "HeadlessNavigationPathScope.h"

#include "HeadlessNetworkServices.h"
#include "Horo/Animation/AnimationErrors.h"
#include "Horo/Foundation/JobAdmission.h"
#include "Horo/Navigation/NavigationCrowdBatch.h"
#include "Horo/Navigation/NavigationDefinition.h"
#include "ProjectSceneNavigationComposition.h"
#include "SceneAiNavigationMotion.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>

namespace Horo::Application::Internal {
    namespace Nav = Navigation;

    namespace {
        /** @brief Hashes canonical integer words without retaining native structs or process identities. */
        void HashWord(Sha256Builder &hash, std::uint64_t value) noexcept {
            std::array<std::byte, sizeof(value)> bytes;
            for (std::size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::byte>(value >> (8 * i));
            (void)hash.Update(bytes);
        }

        /** @brief Hashes finite solver coordinates with one representation for signed zero. */
        void HashFloat(Sha256Builder &hash, float value) noexcept {
            HashWord(hash, std::bit_cast<std::uint32_t>(value == 0 ? 0.0F : value));
        }

        /** @brief Identifies the selected immutable Navigation assets and explicit query policy. */
        Sha256Digest CaptureNavigationProfile(const ProjectSceneNavigationWorld &owner, const ProjectSceneNavigationSelection &selection,
                                              const HeadlessNavigationPathPolicy &policy,
                                              const Nav::NavigationCrowdQualification *crowd) noexcept {
            Sha256Builder hash;
            const auto assets = owner.assets->ActiveAssetProvenance();
            HashWord(hash, assets.size());
            for (const auto &asset : assets)
                (void)hash.Update(std::as_bytes(std::span{asset.cookedContentDigest.bytes}));
            const auto query = selection.policy.MaximumQuery();
            HashWord(hash, selection.profile.Value());
            HashWord(hash, selection.policy.Revision().Value());
            HashWord(hash, policy.filter.Value());
            HashWord(hash, policy.deadlineTicks);
            HashWord(hash, static_cast<std::uint64_t>(query.quality));
            HashWord(hash, query.limits.maximumNodeExpansions);
            HashWord(hash, query.limits.maximumResultPoints);
            HashFloat(hash, query.limits.maximumSearchDistanceMeters);
            if (policy.execution == Nav::NavigationPathExecution::FixedOwner)
                HashWord(hash, static_cast<std::uint64_t>(policy.execution));
            if (crowd) {
                for (const auto *digest :
                     {&crowd->provider, &crowd->buildPlatform, &crowd->goldenEvidence, &crowd->topologyContent, &crowd->profile})
                    (void)hash.Update(std::as_bytes(std::span{digest->bytes}));
                HashFloat(hash, crowd->fixedStepSeconds);
                HashWord(hash, crowd->maximumAgents);
            }
            return hash.Finalize();
        }

        /** @brief Observes an already published completion before the consumer transfers its ownership. */
        void ObserveNavigationCompletion(void *storage, const Nav::NavigationPathCompletion &completion) noexcept {
            auto &replay = *static_cast<HeadlessReplayNavigation *>(storage);
            if (replay.count == replay.results.size()) {
                replay.truncated = true;
                return;
            }
            auto &record = replay.results[replay.count++];
            record.sequence = completion.acceptedSequence;
            if (completion.result.HasError()) {
                const std::string_view code = completion.result.ErrorValue().code.Value();
                record.digest = ComputeSha256(std::as_bytes(std::span{code}));
                return;
            }
            const auto &path = completion.result.Value();
            record.status = path.status;
            record.stopReason = path.stopReason;
            record.points = path.points.size();
            Sha256Builder hash;
            HashWord(hash, static_cast<std::uint64_t>(path.status));
            HashWord(hash, static_cast<std::uint64_t>(path.stopReason));
            HashWord(hash, path.stopPolygonIndex);
            HashWord(hash, path.points.size());
            for (const auto &point : path.points) {
                HashFloat(hash, point.x);
                HashFloat(hash, point.y);
                HashFloat(hash, point.z);
            }
            for (const auto value : {path.stopPosition.x, path.stopPosition.y, path.stopPosition.z, path.cost, path.lengthMeters})
                HashFloat(hash, value);
            record.digest = hash.Finalize();
        }
    }  // namespace

    /** @copydoc HeadlessNavigationPathScope::Create */
    Result<std::unique_ptr<HeadlessNavigationPathScope>> HeadlessNavigationPathScope::Create(
        ProjectSceneNavigationWorld &owner, const ProjectSceneNavigationSelection &selection, const HeadlessNavigationPathPolicy &policy,
        const Runtime::SceneRuntimeId scene, const std::string_view moduleId, const bool tacticalQueries,
        const Nav::NavigationCrowdQualification *crowd) {
        using Scope = Result<std::unique_ptr<HeadlessNavigationPathScope>>;
        if (!policy.IsValid() || !selection.catalog || selection.policy.MaximumQuery().query != Nav::NavigationQueryKind::Path)
            return Scope::Failure(MakeError(Nav::NavigationErrors::CapabilityDescriptorInvalid));
        if (owner.streamCoverage && policy.execution == Nav::NavigationPathExecution::FixedOwner)
            return Scope::Failure(MakeError(Nav::NavigationErrors::OperationUnsupported));
        const auto filter = selection.catalog->Registry().ResolveFilter(policy.filter);
        if (filter.HasError())
            return Scope::Failure(filter.ErrorValue());
        auto world = owner.assets ? owner.assets->ActiveWorld() : nullptr;
        if (!world)
            return Scope::Failure(MakeError(Nav::NavigationErrors::NoNavigationData));
        const auto active = world->ActiveDescriptor();
        if (active.HasError())
            return Scope::Failure(active.ErrorValue());
        if (active.Value().scene.Value() != scene.value)
            return Scope::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        const auto lease = world->Acquire(active.Value().world);
        if (lease.HasError() &&
            (!owner.streamCoverage || lease.ErrorValue().code.Value() != Nav::NavigationErrors::NoNavigationData.code.Value()))
            return Scope::Failure(lease.ErrorValue());
        const auto requirement = selection.policy.MaximumQuery();
        const auto profileRevision = selection.policy.Revision().Value();
        if (lease.HasValue()) {
            if (auto admitted = Nav::AdmitNavigationQuery(lease.Value().Backend().Capabilities(), profileRevision, requirement);
                admitted.HasError())
                return Scope::Failure(admitted.ErrorValue());
        }

        // The supported provider seals its filter table, static obstacle-free overlay, and baked coordinate frame in
        // the immutable topology. Any change to these requires a different world. The project query profile has its own revision.
        const auto epoch = active.Value().topology.Value();
        const Nav::NavigationOutcomeProvenance source{.snapshot = Nav::NavigationSnapshotToken::Create(epoch).Value(),
                                                      .world = active.Value().world,
                                                      .topology = active.Value().topology,
                                                      .obstacleRevision = epoch,
                                                      .filterRevision = epoch,
                                                      .profileRevision = profileRevision,
                                                      .originRevision = epoch};
        // There is exactly one native gameplay principal in this standalone world, with an independent caller epoch per world.
        const Nav::NavigationPathCaller caller{Nav::NavigationDynamicOwnerId::Create(active.Value().world.Value()).Value(),
                                               Nav::NavigationDynamicOwnerGeneration::Create(active.Value().sceneGeneration.Value())
                                                   .Value()};
        const auto tacticalId = std::numeric_limits<std::uint64_t>::max();
        if (tacticalQueries && caller.owner.Value() == tacticalId)
            return Scope::Failure(MakeError(Nav::NavigationErrors::GenerationExhausted));
        const Nav::NavigationAgentSceneBinding binding{active.Value().world, active.Value().scene, active.Value().sceneGeneration};
        try {
            std::vector callers{caller};
            std::optional<Nav::NavigationPathSubmission> tacticalPolicy;
            if (tacticalQueries) {
                const Nav::NavigationPathCaller tacticalCaller{Nav::NavigationDynamicOwnerId::Create(tacticalId).Value(),
                                                               caller.generation};
                callers.push_back(tacticalCaller);
                tacticalPolicy = Nav::NavigationPathSubmission{.caller = tacticalCaller,
                                                               .request = {.filter = policy.filter, .requirement = requirement},
                                                               .deadlineTick = policy.deadlineTicks};
            }
            auto context = Gameplay::GameplayNavigationContext::CreateWithBehaviorAgentPaths({std::string{moduleId}, binding, true, false},
                                                                                             *world, owner.coordinator, owner.agents,
                                                                                             {.caller = caller,
                                                                                              .request = {.filter = policy.filter,
                                                                                                          .requirement = requirement},
                                                                                              .capabilityRevision = profileRevision,
                                                                                              .deadlineTick = policy.deadlineTicks},
                                                                                             owner.maximumPendingPathRequests);
            if (context.HasError())
                return Scope::Failure(context.ErrorValue());
            if (auto refreshed = context.Value()->Refresh({.binding = binding, .path = {.provenance = source}}); refreshed.HasError())
                return Scope::Failure(refreshed.ErrorValue());
            return Scope::Success(std::unique_ptr<HeadlessNavigationPathScope>{
                new HeadlessNavigationPathScope{std::move(world),
                                                owner.coordinator,
                                                active.Value(),
                                                source,
                                                owner.agents,
                                                std::move(callers),
                                                std::move(tacticalPolicy),
                                                std::move(context).Value(),
                                                {CaptureNavigationProfile(owner, selection, policy, crowd),
                                                 policy.execution == Nav::NavigationPathExecution::FixedOwner}}});
        } catch (const std::bad_alloc &) {
            return Scope::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
        }
    }

    struct HeadlessNavigationPathScope::CrowdTick final {
        using Collection = Result<std::optional<std::vector<Nav::NavigationCrowdBatchResult>>>;

        struct Pending final {
            Actor *actor;
            Nav::NavigationVelocityWork work;

            std::size_t AgentIndex() const noexcept {
                return work.request.agentIndex;
            }
        };

        std::vector<Pending> pending;
        std::vector<Nav::NavigationAvoidanceRequest> requests;
        Nav::NavigationCrowdSnapshot capture;
        std::unique_ptr<Nav::NavigationCrowdBatch> batch;
        std::optional<std::vector<Nav::NavigationCrowdBatchResult>> qualifiedResults;

        /** @brief Solve the entire captured population, including stationary requests for inactive motion owners. */
        Result<void> SolveQualified(const HeadlessNavigationMotionPolicy &policy, const Nav::NavigationGeneration topology,
                                    const float step, const CancellationToken &cancellation) {
            const auto &qualified = *policy.qualified;
            if (capture.Agents().size() > qualified.admitted.maximumAgents)
                return Result<void>::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
            try {
                auto idle = policy.limits;
                idle.preferredVelocity = {};
                idle.stepSeconds = step;
                requests.assign(capture.Agents().size(), idle);
                for (std::size_t index = 0; index < requests.size(); ++index)
                    requests[index].agentIndex = index;
                for (const auto &entry : pending)
                    requests[entry.AgentIndex()] = entry.work.request;
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
            }
            auto solved =
                Nav::NavigationCrowdBatch::RunQualified(capture, requests, *qualified.provider, qualified.admitted, topology, cancellation);
            if (solved.HasError())
                return Result<void>::Failure(std::move(solved).ErrorValue());
            qualifiedResults = std::move(solved).Value();
            return Result<void>::Success();
        }

        /** @brief Retains admitted work. A full queue leaves the prepared tick eligible for a stop command. */
        Result<void> Schedule(const JobSystem &jobs, const HeadlessNavigationMotionPolicy &policy, const Nav::NavigationGeneration topology,
                              const float step, const CancellationToken &cancellation) {
            if (policy.qualified)
                return SolveQualified(policy, topology, step, cancellation);
            for (const auto &entry : pending)
                requests.push_back(entry.work.request);
            auto submitted = Nav::NavigationCrowdBatch::Submit(jobs, capture, requests, policy.workerSolvers, cancellation);
            if (submitted.HasError()) {
                if (IsJobAdmissionPressure(submitted.ErrorValue()))
                    return Result<void>::Success();
                return Result<void>::Failure(std::move(submitted).ErrorValue());
            }
            batch = std::move(submitted).Value();
            return Result<void>::Success();
        }

        /** @brief Consumes either owner-ordered results or the complete detached worker batch. */
        Collection Collect(const std::uint64_t tick) {
            if (batch)
                return batch->TryTake(capture, tick);
            return Collection::Success(std::move(qualifiedResults));
        }

        /** @brief Delivers a worker error to the actors still owning motion. */
        void Reject(const Error &error) const {
            for (const auto &entry : pending) {
                if (auto *motion = entry.actor->owner->Motion())
                    motion->Reject(error);
            }
        }
    };

    HeadlessNavigationPathScope::HeadlessNavigationPathScope(
        std::shared_ptr<Nav::NavigationWorldLifecycle> lifecycle, Nav::NavigationCoordinator &coordinator,
        const Nav::NavigationWorldActivationDescriptor activation, const Nav::NavigationOutcomeProvenance source,
        Nav::NavigationAgentRegistry &agents, std::vector<Nav::NavigationPathCaller> callers,
        std::optional<Nav::NavigationPathSubmission> tacticalPolicy, std::shared_ptr<Gameplay::GameplayNavigationContext> context,
        const ReplayProfile &replayProfile) noexcept
        : lifecycle_(std::move(lifecycle)), coordinator_(coordinator), activation_(activation), source_(source), agents_(agents),
          callers_(std::move(callers)), tacticalPolicy_(std::move(tacticalPolicy)), context_(std::move(context)),
          replayProfile_(replayProfile) {}

    HeadlessNavigationPathScope::~HeadlessNavigationPathScope() = default;

    /** @copydoc HeadlessNavigationPathScope::PrepareActors */
    Result<void> HeadlessNavigationPathScope::PrepareActors(ScenePhysicsHost &physics, const Runtime::RuntimeSceneView &scene,
                                                            std::span<const HeadlessNavigationActorPolicy> selected) {
        if (selected.empty())
            return Result<void>::Success();
        auto population = agents_.Snapshot();
        if (population.HasError())
            return Result<void>::Failure(std::move(population).ErrorValue());
        if (!actors_.empty() || selected.size() > population.Value().Agents().size())
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::AdmissionRejected));
        try {
            auto callers = callers_;
            const auto lastActorId = std::numeric_limits<std::uint64_t>::max() - (tacticalPolicy_ ? 1 : 0);
            if (tacticalPolicy_)
                callers.pop_back();
            std::vector<Actor> actors;
            callers.reserve(callers.size() + selected.size());
            actors.reserve(selected.size());
            samples_.reserve(selected.size());
            auto crowd = std::make_unique<CrowdTick>();
            crowd->pending.reserve(selected.size());
            crowd->requests.reserve(selected.size());
            for (const auto &policy : selected) {
                const auto entity = scene.Find(policy.object);
                if (!entity || policy.principal == 0 || (policy.motionStartTrigger && !policy.motionStartTrigger->IsValid()))
                    return Result<void>::Failure(MakeError(Nav::NavigationErrors::AgentDescriptorInvalid));
                const auto previous = callers.back().owner.Value();
                if (previous >= lastActorId)
                    return Result<void>::Failure(MakeError(Nav::NavigationErrors::GenerationExhausted));
                const Nav::NavigationPathCaller caller{Nav::NavigationDynamicOwnerId::Create(previous + 1).Value(),
                                                       callers.front().generation};
                auto actor = SceneAiNavigationActor::CreateForScene(agents_, *lifecycle_, coordinator_, caller, physics, scene, *entity,
                                                                    {.identity = policy.principal,
                                                                     .source = CharacterInput::DesiredMotionSource::ExternalIntent,
                                                                     .permissionGranted = true});
                if (actor.HasError())
                    return Result<void>::Failure(std::move(actor).ErrorValue());
                if (std::ranges::any_of(actors, [&](const auto &existing) {
                    return existing.owner->Agent() == actor.Value()->Agent();
                }))
                    return Result<void>::Failure(MakeError(Nav::NavigationErrors::AgentDescriptorInvalid));
                actors.push_back({std::move(actor).Value(), {}, policy.motionStartTrigger});
                callers.push_back(caller);
            }
            if (tacticalPolicy_)
                callers.push_back(tacticalPolicy_->caller);
            actors_ = std::move(actors);
            callers_ = std::move(callers);
            crowdTick_ = std::move(crowd);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc HeadlessNavigationPathScope::BeginMotionTick */
    void HeadlessNavigationPathScope::BeginMotionTick(const std::uint64_t tick, const float stepSeconds) noexcept {
        DiscardCrowdTick();
        motionTick_ = tick;
        motionStep_ = stepSeconds;
    }

    Result<Nav::NavigationMovementObservation> HeadlessNavigationPathScope::ObserveActor(
        const Runtime::RuntimeSceneView &scene, const HeadlessNavigationMotionPolicy &policy, const SceneAiNavigationActor &actor,
        const std::optional<Nav::NavigationAgentOwner> target) const {
        using Observation = Result<Nav::NavigationMovementObservation>;
        Nav::NavigationMovementObservation current{.binding = context_->Binding().scene,
                                                   .stepSeconds = motionStep_,
                                                   .path = {.provenance = source_,
                                                            .linkRevision = activation_.topology.Value(),
                                                            .goalRevision = 1,
                                                            .tick = motionTick_}};
        if (target) {
            if (!policy.follow)
                return Observation::Failure(MakeError(Nav::NavigationErrors::CapabilityUnavailable));
            auto followed = policy.follow(scene, *target, motionTick_);
            if (followed.HasError())
                return Observation::Failure(std::move(followed).ErrorValue());
            current.target = std::move(followed).Value();
        }
        return actor.Capture(current);
    }

    /** @copydoc HeadlessNavigationPathScope::AdmitMotion */
    Result<void> HeadlessNavigationPathScope::AdmitMotion(const Runtime::RuntimeSceneView &scene,
                                                          const HeadlessNavigationMotionPolicy &policy, AI::AiRuntimeCapability ai,
                                                          AI::AiTaskContinuation task, Nav::NavigationMovementRequest request,
                                                          const std::uint64_t tick, Runtime::SceneAnimationRuntime *animation) {
        if (tick == 0 || tick != motionTick_ || scene.RuntimeId().value != activation_.scene.Value())
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        const auto entity = ai.ResolveAgentOwner(task.Operation().agent);
        if (entity.HasError())
            return Result<void>::Failure(entity.ErrorValue());
        const auto actor = agents_.FindOwner({activation_.scene, entity.Value().entity.index, entity.Value().entity.generation});
        if (!actor)
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::AgentRegistryStale));
        const auto selected = std::ranges::find_if(actors_, [&](const Actor &entry) {
            return entry.owner->Agent() == *actor;
        });
        if (selected == actors_.end())
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::AdmissionRejected));
        if (selected->motionStartTrigger && !animation)
            return Result<void>::Failure(MakeError(Animation::AnimationErrors::GraphAdmissionRejected));
        const auto target = request.entity;
        auto current = ObserveActor(scene, policy, *selected->owner, target);
        if (current.HasError())
            return Result<void>::Failure(std::move(current).ErrorValue());
        auto admitted = selected->owner->Admit(std::move(ai), std::move(task), std::move(request), current.Value());
        if (admitted.HasError())
            return admitted;
        selected->target = target;
        if (selected->motionStartTrigger) {
            const auto trigger = animation->QueueTrigger(entity.Value(), *selected->motionStartTrigger);
            if (!trigger.HasValue()) {
                auto error = Animation::DescribeGraphInstanceFailure(trigger);
                if (auto *motion = selected->owner->Motion())
                    motion->Reject(std::move(error));
                else
                    return Result<void>::Failure(std::move(error));
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc HeadlessNavigationPathScope::CaptureMotion */
    Result<Nav::NavigationCrowdSnapshot> HeadlessNavigationPathScope::CaptureMotion(const Runtime::RuntimeSceneView &scene,
                                                                                    const HeadlessNavigationMotionPolicy &policy) {
        using Capture = Result<Nav::NavigationCrowdSnapshot>;
        if (std::ranges::none_of(actors_, [](const Actor &entry) {
            return entry.owner->Motion() != nullptr;
        }))
            return Capture::Success({});
        auto population = agents_.Snapshot();
        if (population.HasError())
            return Capture::Failure(std::move(population).ErrorValue());
        samples_.clear();
        for (auto &actor : actors_) {
            auto sample = actor.owner->Sample(motionTick_);
            if (sample.HasValue())
                samples_.push_back(sample.Value());
            else if (auto *motion = actor.owner->Motion())
                motion->Reject(std::move(sample).ErrorValue());
        }
        auto captured = policy.capture(scene, activation_, population.Value(), samples_, motionTick_);
        if (captured.HasValue()) {
            const auto &snapshot = captured.Value();
            if (!snapshot.IsValid() || snapshot.Binding() != context_->Binding().scene || snapshot.CaptureTick() != motionTick_)
                return Capture::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        }
        return captured;
    }

    /** @copydoc HeadlessNavigationPathScope::ObserveMotionInput */
    Result<Nav::NavigationMovementObservation> HeadlessNavigationPathScope::ObserveMotionInput(
        const Runtime::RuntimeSceneView &scene, const HeadlessNavigationMotionPolicy &policy, const Actor &actor,
        const Nav::NavigationCrowdSnapshot &capture, Nav::NavigationAvoidanceRequest &limits) const {
        const auto sampled = capture.Agents();
        const auto sample = std::ranges::find(sampled, actor.owner->Agent(), &Nav::NavigationCrowdAgentFact::handle);
        if (sample == sampled.end())
            return Result<Nav::NavigationMovementObservation>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        limits.agentIndex = static_cast<std::size_t>(sample - sampled.begin());
        limits.stepSeconds = motionStep_;
        return ObserveActor(scene, policy, *actor.owner, actor.target);
    }

    /** @copydoc HeadlessNavigationPathScope::SubmitMotion */
    Result<void> HeadlessNavigationPathScope::SubmitMotion(const Runtime::RuntimeSceneView &scene,
                                                           const HeadlessNavigationMotionPolicy &policy) {
        auto capture = CaptureMotion(scene, policy);
        if (capture.HasError())
            return Result<void>::Failure(std::move(capture).ErrorValue());
        if (!capture.Value().IsValid())
            return Result<void>::Success();
        for (auto &actor : actors_) {
            auto *motion = actor.owner->Motion();
            if (!motion)
                continue;
            auto limits = policy.limits;
            auto current = ObserveMotionInput(scene, policy, actor, capture.Value(), limits);
            if (current.HasError()) {
                motion->Reject(std::move(current).ErrorValue());
                continue;
            }
            // Motion-stage failures are delivered by the binding to this actor's task.
            (void)motion->Submit(current.Value(), capture.Value(), limits, *policy.solver);
        }
        return Result<void>::Success();
    }

    /** @copydoc HeadlessNavigationPathScope::DispatchMotion */
    Result<void> HeadlessNavigationPathScope::DispatchMotion(const Runtime::RuntimeSceneView &scene,
                                                             const HeadlessNavigationMotionPolicy &policy, const JobSystem &jobs,
                                                             const CancellationToken &cancellation) {
        auto capture = CaptureMotion(scene, policy);
        if (capture.HasError())
            return Result<void>::Failure(std::move(capture).ErrorValue());
        if (!capture.Value().IsValid())
            return Result<void>::Success();
        auto &tick = *crowdTick_;
        tick.capture = std::move(capture).Value();
        for (auto &actor : actors_) {
            auto *motion = actor.owner->Motion();
            if (!motion)
                continue;
            auto limits = policy.limits;
            auto current = ObserveMotionInput(scene, policy, actor, tick.capture, limits);
            if (current.HasError()) {
                motion->Reject(std::move(current).ErrorValue());
                continue;
            }
            auto prepared = motion->Prepare(current.Value(), tick.capture, limits);
            if (prepared.HasValue() && prepared.Value())
                tick.pending.push_back({&actor, std::move(*prepared.Value())});
        }
        if (tick.pending.empty())
            return Result<void>::Success();
        std::ranges::sort(tick.pending, std::ranges::less{}, &CrowdTick::Pending::AgentIndex);
        auto scheduled = tick.Schedule(jobs, policy, activation_.topology, motionStep_, cancellation);
        if (scheduled.HasError())
            DiscardCrowdTick();
        return scheduled;
    }

    /** @copydoc HeadlessNavigationPathScope::PublishMotion */
    void HeadlessNavigationPathScope::PublishMotion(const Runtime::RuntimeSceneView &scene, const HeadlessNavigationMotionPolicy &policy,
                                                    const CancellationToken &cancellation) {
        if (!crowdTick_ || crowdTick_->pending.empty())
            return;
        if (cancellation.IsCancellationRequested()) {
            DiscardCrowdTick();
            return;
        }
        auto &tick = *crowdTick_;
        if (policy.qualified) {
            const auto current = lifecycle_->ActiveDescriptor();
            if (current.HasError() || current.Value() != activation_) {
                tick.Reject(MakeError(Nav::NavigationErrors::StaleSnapshot));
                DiscardCrowdTick();
                return;
            }
        }
        const auto completed = tick.Collect(motionTick_);
        if (completed.HasError()) {
            tick.Reject(completed.ErrorValue());
        } else if (const auto &results = completed.Value()) {
            for (const auto &result : *results)
                PublishCrowdResult(scene, policy, result.agentIndex, result.outcome);
        } else {
            const Nav::NavigationAvoidanceOutcome stopped{.stopReason = Nav::NavigationAvoidanceStopReason::CapacityBusy,
                                                          .binding = tick.capture.Binding(),
                                                          .dynamicRevision = tick.capture.DynamicRevision(),
                                                          .captureTick = tick.capture.CaptureTick()};
            for (const auto agent : tick.capture.CellAgentIndices())
                PublishCrowdResult(scene, policy, agent, stopped);
        }
        DiscardCrowdTick();
    }

    /** @copydoc HeadlessNavigationPathScope::PublishCrowdResult */
    void HeadlessNavigationPathScope::PublishCrowdResult(const Runtime::RuntimeSceneView &scene,
                                                         const HeadlessNavigationMotionPolicy &policy, const std::size_t agent,
                                                         const Nav::NavigationAvoidanceOutcome &outcome) {
        const auto &tick = *crowdTick_;
        const auto found = std::ranges::lower_bound(tick.pending, agent, std::ranges::less{}, &CrowdTick::Pending::AgentIndex);
        if (found == tick.pending.end() || found->AgentIndex() != agent)
            return;
        auto &actor = *found->actor;
        auto *motion = actor.owner->Motion();
        if (!motion)
            return;
        auto current = ObserveActor(scene, policy, *actor.owner, actor.target);
        if (current.HasError())
            motion->Reject(std::move(current).ErrorValue());
        else
            (void)motion->Complete(found->work, Result<Nav::NavigationAvoidanceOutcome>::Success(outcome), current.Value(), tick.capture);
    }

    /** @copydoc HeadlessNavigationPathScope::DiscardCrowdTick */
    void HeadlessNavigationPathScope::DiscardCrowdTick() noexcept {
        if (!crowdTick_)
            return;
        auto &tick = *crowdTick_;
        tick.batch.reset();
        tick.qualifiedResults.reset();
        for (const auto &pending : tick.pending) {
            if (auto *motion = pending.actor->owner->Motion())
                motion->DiscardPrepared();
        }
        tick.pending.clear();
        tick.requests.clear();
        tick.capture = {};
    }

    /** @copydoc HeadlessNavigationPathScope::ObserveMotion */
    void HeadlessNavigationPathScope::ObserveMotion(const std::uint64_t tick) {
        for (auto &actor : actors_) {
            if (auto *motion = actor.owner->Motion()) {
                if (auto result = motion->Observe(tick); result.HasError())
                    motion->Reject(std::move(result).ErrorValue());
            }
        }
    }

    /** @copydoc HeadlessNavigationPathScope::DiscardMotionFeedback */
    void HeadlessNavigationPathScope::DiscardMotionFeedback() noexcept {
        DiscardCrowdTick();
        for (auto &actor : actors_)
            if (auto *motion = actor.owner->Motion())
                motion->DiscardFeedback();
    }

    /** @copydoc HeadlessNavigationPathScope::Context */
    const std::shared_ptr<Gameplay::GameplayNavigationContext> &HeadlessNavigationPathScope::Context() const noexcept {
        return context_;
    }

    /** @copydoc HeadlessNavigationPathScope::CommitAtFixedTick */
    Result<void> HeadlessNavigationPathScope::CommitAtFixedTick(const std::uint64_t tick, HeadlessReplayNavigation *replay) {
        const auto current = lifecycle_->ActiveDescriptor();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        const auto &published = current.Value();
        if (published.world != activation_.world || published.scene != activation_.scene ||
            published.sceneGeneration != activation_.sceneGeneration || published.topology < activation_.topology)
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        if (published.topology != activation_.topology) {
            activation_ = published;
            const auto epoch = published.topology.Value();
            source_.snapshot = Nav::NavigationSnapshotToken::Create(epoch).Value();
            source_.topology = published.topology;
            source_.obstacleRevision = epoch;
            source_.filterRevision = epoch;
            source_.originRevision = epoch;
        }
        Nav::NavigationPathCommitObserver observer;
        if (replay) {
            *replay = {.profile = replayProfile_.digest,
                       .tick = tick,
                       .topology = activation_.topology.Value(),
                       .fixedOwner = replayProfile_.fixedOwner};
            observer = {replay, ObserveNavigationCompletion};
        }
        static_cast<void>(coordinator_.Commit({activation_, source_, callers_, tick}, observer));
        return context_->Refresh({.binding = context_->Binding().scene, .path = {.provenance = source_, .tick = tick}});
    }

    /** @copydoc HeadlessNavigationPathScope::ReadQueryAccess */
    std::optional<AI::QueryNavigationAccess> HeadlessNavigationPathScope::ReadQueryAccess(const std::uint64_t tick) const noexcept {
        if (!tacticalPolicy_ || revoked_ || lifecycle_->State() != Nav::NavigationWorldLifecycleState::Active)
            return std::nullopt;
        const auto active = lifecycle_->ActiveDescriptor();
        if (active.HasError() || active.Value() != activation_)
            return std::nullopt;
        return AI::QueryNavigationAccess{*lifecycle_,
                                         coordinator_.ObservePathRequests(),
                                         {activation_, source_, callers_, tick},
                                         tacticalPolicy_->caller,
                                         tacticalPolicy_->request.filter,
                                         tacticalPolicy_->request.requirement,
                                         tacticalPolicy_->deadlineTick};
    }

    /** @copydoc HeadlessNavigationPathScope::DispatchAtFixedTick */
    void HeadlessNavigationPathScope::DispatchAtFixedTick(const std::uint64_t tick, const CancellationToken &cancellation) const {
        static_cast<void>(coordinator_.Dispatch(tick, cancellation));
    }

    /** @copydoc HeadlessNavigationPathScope::Revoke */
    void HeadlessNavigationPathScope::Revoke() noexcept {
        revoked_ = true;
        DiscardCrowdTick();
        actors_.clear();
        context_->Revoke();
    }

    /** @copydoc HeadlessNavigationPathScope::PollRetirement */
    Result<bool> HeadlessNavigationPathScope::PollRetirement() const {
        return context_->DrainRetired();
    }
}  // namespace Horo::Application::Internal
