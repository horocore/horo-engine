#include "Horo/Scene/ScenePrefabExpansionOwner.h"

#include <algorithm>
#include <exception>
#include <new>

namespace Horo::SceneSource {
    namespace {
        constexpr std::size_t MaximumSceneSourceBytes = 16U * 1024U * 1024U;
        constexpr std::size_t MaximumSceneObjects = 65'536;

        /** @brief Bounds detached input and codec work before copying or allocating a JSON tree. */
        class InputBudget final {
        public:
            bool Add(const std::size_t count, const std::size_t width = 1) noexcept {
                if (width == 0 || count > remaining_ / width)
                    return false;
                remaining_ -= count * width;
                return true;
            }

        private:
            std::size_t remaining_{64U * 1024U * 1024U};
        };

        /** @brief Bounds authored physics contributors and per-shape material bindings. */
        bool AdmitPhysics(InputBudget &budget, const SceneObjectComponentSet &components) {
            if (!budget.Add(components.colliders.size(), sizeof(Runtime::ColliderComponent)) ||
                !budget.Add(components.physicsConstraints.size(), sizeof(Runtime::PhysicsConstraintComponent)))
                return false;
            for (const auto &collider : components.colliders) {
                if (!budget.Add(collider.materials.size(), sizeof(Runtime::PhysicsColliderMaterialBinding)))
                    return false;
            }
            return true;
        }

        /** @brief Bounds opaque gameplay bytes before hexadecimal projection. */
        bool AdmitGameplay(InputBudget &budget, const SceneObjectComponentSet &components) {
            if (!budget.Add(components.gameplayComponents.size(), sizeof(Gameplay::SerializedComponent)))
                return false;
            for (const auto &component : components.gameplayComponents) {
                if (!budget.Add(component.typeId.Value().size()) || !budget.Add(component.payload.size(), 2))
                    return false;
            }
            return true;
        }

        /** @brief Bounds behavior metadata and worst-case JSON escaping. */
        bool AdmitBehaviors(InputBudget &budget, const SceneObjectComponentSet &components) {
            if (!budget.Add(components.behaviors.size(), sizeof(Gameplay::BehaviorComponent)))
                return false;
            for (const auto &behavior : components.behaviors) {
                if (!budget.Add(behavior.typeId.Value().size()) || !budget.Add(behavior.fields.size(), sizeof(Gameplay::BehaviorField)))
                    return false;
                for (const auto &field : behavior.fields) {
                    if (!budget.Add(field.name.size(), 6))
                        return false;
                    if (const auto text = std::get_if<std::string>(&field.value); text && !budget.Add(text->size(), 6))
                        return false;
                }
            }
            return true;
        }

        /** @brief Bounds both variable navigation profile lists. */
        bool AdmitNavigation(InputBudget &budget, const SceneObjectComponentSet &components) {
            if (components.navigationSurface &&
                !budget.Add(components.navigationSurface->profiles.size(), sizeof(Navigation::NavigationAgentProfileId)))
                return false;
            if (components.navigationLink &&
                !budget.Add(components.navigationLink->profiles.size(), sizeof(Navigation::NavigationAgentProfileId)))
                return false;
            return true;
        }

        /** @brief Bounds object containers and all component payload ownership before JSON-tree construction. */
        bool AdmitObject(InputBudget &budget, const SceneObjectSnapshot &object) {
            const auto &components = object.components;
            return budget.Add(object.name.size(), 6) && AdmitPhysics(budget, components) && AdmitGameplay(budget, components) &&
                   AdmitBehaviors(budget, components) && AdmitNavigation(budget, components);
        }

        /** @brief Bounds the complete document input before encoding. */
        bool AdmitDocument(const SceneSourceDocument &document) {
            if (document.objects.size() > MaximumSceneObjects ||
                document.prefabInstances.size() > Prefab::PrefabHardLimits::SourceObjectCount)
                return false;
            InputBudget budget;
            if (!budget.Add(document.objects.size(), sizeof(SceneObjectSnapshot)) ||
                !budget.Add(document.prefabInstances.size(), sizeof(ScenePrefabInstance)))
                return false;
            for (const auto &object : document.objects) {
                if (!AdmitObject(budget, object))
                    return false;
            }
            return true;
        }

        /** @brief Caps all copied resolver sources, including unreachable publications, before owned worker capture. */
        Result<void> AdmitResolver(const Prefab::PrefabSourceResolverSnapshot &resolver, const CancellationToken &cancellation) {
            std::size_t remaining = Prefab::PrefabHardLimits::SourceDocumentBytes;
            if (resolver.Sources().size() > Prefab::PrefabHardLimits::SourceObjectCount)
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheCapacityExceeded));
            for (const auto &source : resolver.Sources()) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
                const auto bytes = source.document.SerializeCanonical();
                if (bytes.HasError())
                    return Result<void>::Failure(bytes.ErrorValue());
                if (bytes.Value().size() > remaining)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheCapacityExceeded));
                remaining -= bytes.Value().size();
            }
            return Result<void>::Success();
        }

        /** @brief Commits every current authored value through the existing canonical Scene codec, not a revision counter alone. */
        Result<Sha256Digest> DocumentDigest(const SceneSourceDocument &document) {
            if (!AdmitDocument(document))
                return Result<Sha256Digest>::Failure(MakeError(Prefab::PrefabErrors::PayloadTooLarge));
            try {
                const auto bytes = EncodeSceneSource({document.objects, document.prefabInstances});
                if (bytes.size() > MaximumSceneSourceBytes)
                    return Result<Sha256Digest>::Failure(MakeError(Prefab::PrefabErrors::PayloadTooLarge));
                return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span{bytes})));
            } catch (const std::bad_alloc &) {
                throw;  // The public admission/completion boundary returns the allocation-specific typed failure.
            } catch (const std::exception &error) {
                return Result<Sha256Digest>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected, error.what()));
            }
        }

        /** @brief Recognizes terminal scheduler state; its snapshot lock synchronizes the completed owned envelope. */
        bool Terminal(const JobState state) noexcept {
            return state == JobState::Succeeded || state == JobState::Failed || state == JobState::Cancelled;
        }
    }  // namespace

    /** @brief The worker alone writes detached candidates/result; owner reads only after scheduler terminal synchronization. */
    struct ScenePrefabExpansionOwner::Work final {
        ScenePrefabExpansionRequest request;
        Sha256Digest documentDigest;
        CancellationToken cancellation;
        std::vector<Prefab::PrefabExpansionCacheKey> keys;
        std::vector<std::shared_ptr<const Prefab::EffectivePrefabCandidate>> hits;
        std::vector<std::optional<Prefab::EffectivePrefabCandidate>> candidates;
        std::optional<Runtime::RuntimeSceneDefinition> definition;

        /** @brief Checks all captured document/session/settings evidence before dependency recapture. */
        bool MatchesCurrent(const ScenePrefabExpansionRequest &current, const Sha256Digest &digest, const bool closed) const {
            return !closed && !cancellation.IsCancellationRequested() && definition && current.documentSession == request.documentSession &&
                   current.scene == request.scene && current.revision == request.revision &&
                   current.resolver.RegistryRevision() == request.resolver.RegistryRevision() &&
                   current.limits.Policy() == request.limits.Policy() && digest == documentDigest;
        }

        /** @brief Recaptures source identities and cancellation at the publication boundary, without mutating cache state. */
        Result<void> ValidateCurrent(const ScenePrefabExpansionRequest &current, const Prefab::PrefabExpansionCache &cache,
                                     const bool closed) const {
            const auto digest = DocumentDigest(current.document);
            if (digest.HasError())
                return Result<void>::Failure(digest.ErrorValue());
            if (!MatchesCurrent(current, digest.Value(), closed))
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
            for (std::size_t index = 0; index < keys.size(); ++index) {
                const auto &placement = current.document.prefabInstances[index];
                const auto key = cache.CaptureKey(current.resolver, placement.sourcePrefab.Asset(), placement.instanceId, current.limits);
                if (key.HasError())
                    return Result<void>::Failure(key.ErrorValue());
                if (key.Value() != keys[index])
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
            }
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
            return Result<void>::Success();
        }

        /** @brief Resolves missing exact keys and invokes the ordinary transactional Scene conversion without live owner access. */
        Result<void> Execute(const CancellationToken &token) {
            ScenePrefabProjection projection;
            const auto &placements = request.document.prefabInstances;
            projection.instances.reserve(placements.size());
            candidates.resize(placements.size());
            for (std::size_t index = 0; index < placements.size(); ++index) {
                if (token.IsCancellationRequested())
                    return JobCancelled();
                const auto &placement = placements[index];
                ScenePrefabInstanceProjection entry{.authored = placement};
                if (hits[index])
                    entry.expanded = *hits[index];
                else {
                    auto candidate = request.resolver.Resolve(placement.sourcePrefab.Asset(), placement.instanceId, request.limits, token);
                    if (candidate.HasError()) {
                        if (token.IsCancellationRequested())
                            return JobCancelled(candidate.ErrorValue());
                        return Result<void>::Failure(candidate.ErrorValue());
                    }
                    candidates[index] = std::move(candidate).Value();
                    entry.expanded = *candidates[index];
                }
                projection.instances.push_back(std::move(entry));
            }
            if (token.IsCancellationRequested())
                return JobCancelled();
            auto converted = ConvertScenePrefabProjectionToRuntime({request.document.objects, placements}, request.scene, request.revision,
                                                                   projection, request.resolver, request.limits);
            if (converted.HasError())
                return Result<void>::Failure(converted.ErrorValue());
            if (token.IsCancellationRequested())
                return JobCancelled();
            definition = std::move(converted).Value();
            return Result<void>::Success();
        }
    };

    /** @copydoc ScenePrefabExpansionOwner::ScenePrefabExpansionOwner */
    ScenePrefabExpansionOwner::ScenePrefabExpansionOwner(JobSystem &jobs, const Prefab::PrefabExpansionCacheLimits limits)
        : jobs_(jobs), cache_(limits), owner_(std::this_thread::get_id()) {}

    /** @copydoc ScenePrefabExpansionOwner::~ScenePrefabExpansionOwner */
    ScenePrefabExpansionOwner::~ScenePrefabExpansionOwner() {
        if (work_) {
            static_cast<void>(handle_.RequestCancel());
            // Teardown fallback owns no document/service borrows; explicit bounded Shutdown is the normal host path.
            static_cast<void>(handle_.Wait());
        }
    }

    bool ScenePrefabExpansionOwner::IsOwner() const noexcept {
        return owner_ == std::this_thread::get_id();
    }

    /** @copydoc ScenePrefabExpansionOwner::AdmitRequest */
    Result<void> ScenePrefabExpansionOwner::AdmitRequest(const ScenePrefabExpansionRequest &request,
                                                         const CancellationToken &cancellation) const {
        if (!IsOwner())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheThreadViolation));
        if (closed_ || work_)
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
        if (request.documentSession == 0 || request.scene.value == 0 || request.revision.value == 0)
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::IdentityInvalid));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
        return Result<void>::Success();
    }

    /** @copydoc ScenePrefabExpansionOwner::CaptureWork */
    Result<std::shared_ptr<ScenePrefabExpansionOwner::Work>> ScenePrefabExpansionOwner::CaptureWork(
        const ScenePrefabExpansionRequest &request, const CancellationToken &cancellation) {
        using Output = std::shared_ptr<Work>;
        if (const auto admitted = AdmitResolver(request.resolver, cancellation); admitted.HasError())
            return Result<Output>::Failure(admitted.ErrorValue());
        auto digest = DocumentDigest(request.document);
        if (digest.HasError())
            return Result<Output>::Failure(digest.ErrorValue());
        std::vector<Prefab::PrefabExpansionCacheKey> keys;
        std::vector<std::shared_ptr<const Prefab::EffectivePrefabCandidate>> hits;
        keys.reserve(request.document.prefabInstances.size());
        hits.reserve(request.document.prefabInstances.size());
        for (const auto &placement : request.document.prefabInstances) {
            auto key =
                cache_.CaptureKey(request.resolver, placement.sourcePrefab.Asset(), placement.instanceId, request.limits, cancellation);
            if (key.HasError())
                return Result<Output>::Failure(key.ErrorValue());
            hits.push_back(cache_.Find(key.Value()));
            keys.push_back(std::move(key).Value());
        }
        return Result<Output>::Success(
            std::make_shared<Work>(Work{request, digest.Value(), cancellation, std::move(keys), std::move(hits), {}, {}}));
    }

    /** @copydoc ScenePrefabExpansionOwner::Submit */
    Result<JobId> ScenePrefabExpansionOwner::Submit(const ScenePrefabExpansionRequest &request, CancellationToken cancellation) {
        try {
            if (const auto admitted = AdmitRequest(request, cancellation); admitted.HasError())
                return Result<JobId>::Failure(admitted.ErrorValue());
            auto captured = CaptureWork(request, cancellation);
            if (captured.HasError())
                return Result<JobId>::Failure(captured.ErrorValue());
            auto work = std::move(captured).Value();
            // Restrict, never widen, host permissions: owner admission cannot block on queue space.
            const JobProducerScope admission{JobProducerRole::ExternalUnknown};
            auto submitted = jobs_.SubmitResult({.parentCancellation = cancellation}, [work](const CancellationToken &token) {
                try {
                    return work->Execute(token);
                } catch (const std::bad_alloc &) {
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
                }
            });
            if (submitted.HasError())
                return Result<JobId>::Failure(submitted.ErrorValue());
            handle_ = std::move(submitted).Value();
            work_ = std::move(work);
            return Result<JobId>::Success(handle_.Id());
        } catch (const std::bad_alloc &) {
            return Result<JobId>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc ScenePrefabExpansionOwner::Memoize */
    Result<void> ScenePrefabExpansionOwner::Memoize(Work &completed) {
        for (std::size_t index = 0; index < completed.candidates.size(); ++index) {
            if (!completed.candidates[index])
                continue;
            try {
                const auto stored = cache_.Store(std::move(completed.keys[index]), std::move(*completed.candidates[index]));
                if (stored.HasError() &&
                    stored.ErrorValue().code.Value() != Prefab::PrefabErrors::ExpansionCacheCapacityExceeded.code.Value() &&
                    stored.ErrorValue().code.Value() != Prefab::PrefabErrors::ExpansionCacheAllocationFailed.code.Value())
                    return Result<void>::Failure(stored.ErrorValue());
            } catch (const std::bad_alloc &) {
                // Error construction itself may allocate; optional retention cannot discard validated output.
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc ScenePrefabExpansionOwner::TakeCompleted */
    Result<std::optional<Runtime::RuntimeSceneDefinition>> ScenePrefabExpansionOwner::TakeCompleted(
        const ScenePrefabExpansionRequest &current) {
        using Output = std::optional<Runtime::RuntimeSceneDefinition>;
        try {
            if (!IsOwner())
                return Result<Output>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheThreadViolation));
            if (!work_)
                return Result<Output>::Success(std::nullopt);
            const auto snapshot = handle_.Snapshot();
            if (!snapshot || !Terminal(snapshot->state))
                return Result<Output>::Success(std::nullopt);
            // Terminal scheduler synchronization precedes every read of worker-owned output.
            auto completed = std::move(work_);
            handle_ = {};
            if (snapshot->error)
                return Result<Output>::Failure(*snapshot->error);
            if (const auto validated = completed->ValidateCurrent(current, cache_, closed_); validated.HasError())
                return Result<Output>::Failure(validated.ErrorValue());
            if (const auto stored = Memoize(*completed); stored.HasError())
                return Result<Output>::Failure(stored.ErrorValue());
            if (completed->cancellation.IsCancellationRequested())
                return Result<Output>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
            return Result<Output>::Success(std::move(completed->definition));
        } catch (const std::bad_alloc &) {
            return Result<Output>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc ScenePrefabExpansionOwner::Join */
    Result<void> ScenePrefabExpansionOwner::Join(const JoinOptions &options) const {
        if (!IsOwner())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheThreadViolation));
        return work_ ? handle_.Wait(options) : Result<void>::Success();
    }

    Result<void> ScenePrefabExpansionOwner::Drain(const JoinOptions &options) {
        if (!IsOwner())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheThreadViolation));
        closed_ = true;
        if (work_) {
            static_cast<void>(handle_.RequestCancel());
            auto joined = handle_.Wait(options);
            const auto snapshot = handle_.Snapshot();
            if (!snapshot || !Terminal(snapshot->state))
                return joined;
            // Terminal job failure/cancellation has still drained ownership; it is not a teardown failure.
            work_.reset();
            handle_ = {};
        }
        return cache_.Clear();
    }

    /** @copydoc ScenePrefabExpansionOwner::CloseDocument */
    Result<void> ScenePrefabExpansionOwner::CloseDocument(const JoinOptions &options) {
        return Drain(options);
    }

    /** @copydoc ScenePrefabExpansionOwner::ReplaceScene */
    Result<void> ScenePrefabExpansionOwner::ReplaceScene(const JoinOptions &options) {
        auto drained = Drain(options);
        if (drained.HasValue() && !shutdown_)
            closed_ = false;
        return drained;
    }

    /** @copydoc ScenePrefabExpansionOwner::Shutdown */
    Result<void> ScenePrefabExpansionOwner::Shutdown(const JoinOptions &options) {
        if (!IsOwner())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheThreadViolation));
        shutdown_ = true;
        return Drain(options);
    }
}  // namespace Horo::SceneSource
