#include "Horo/Network/ReplicationWorldLifecycle.h"

#include "AuthoredSceneBindingValidation.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkMetrics.h"
#include "Horo/Network/NetworkPrefabLifecycle.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <thread>
#include <utility>

namespace Horo::Network {
    namespace Detail {
        /** @brief Immutable identity/capability record retained by world leases after logical revocation. */
        struct ReplicationWorldRecord final {
            ReplicationWorldRecord(const ReplicationWorldActivationDescriptor &activation,
                                   const ReplicationWorldCapabilities worldCapabilities, NetworkObjectMapping worldMapping,
                                   CancellationToken activationToken) noexcept
                : descriptor(activation), capabilities(worldCapabilities), mapping(std::move(worldMapping)),
                  activationCancellation(std::move(activationToken)) {}

            ReplicationWorldActivationDescriptor descriptor;
            ReplicationWorldCapabilities capabilities;
            NetworkObjectMapping mapping;
            CancellationToken activationCancellation;
            CancellationSource cancellation;
            std::atomic<bool> revoked{false};
            std::atomic<std::uint64_t> mappingRevision{1};
            std::thread::id owner{std::this_thread::get_id()};
            bool paused{}; /**< Owner-lane structural admission. Worker capture pins never access this field. */
        };
    }  // namespace Detail

    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Keeps an unpublished mapping candidate bound to one exact active world revision. */
        class ObjectBatchCandidate final : public Runtime::SceneStructuralCandidate {
        public:
            ObjectBatchCandidate(const std::shared_ptr<Detail::ReplicationWorldRecord> &world, NetworkObjectMapping mapping,
                                 const CancellationToken &cancellation, std::size_t added, std::size_t retired, NetworkMetrics *metrics)
                : world_(world), replacement_(std::move(mapping)), cancellation_(cancellation), revision_(world->mappingRevision.load()),
                  added_(added), retired_(retired), metrics_(metrics) {}

            /** @brief Checks identity, owner lane, and revocation before any participant publishes. */
            Result<void> ValidatePublication() const override {
                const auto world = ActiveWorld();
                if (!world || !replacement_ || world->mappingRevision.load() != revision_)
                    return Failure<void>(NetworkErrors::ReplicationWorldStale);
                if (world->paused)
                    return Failure<void>(NetworkErrors::ReplicationWorldUnavailable);
                if (cancellation_.IsCancellationRequested())
                    return Failure<void>(NetworkErrors::ReplicationWorldCancelled);
                return Result<void>::Success();
            }

            /** @brief Swaps both mapping indexes without callbacks while Scene publishes its owner aggregate. */
            void Publish() noexcept override {
                const auto world = ActiveWorld();
                if (!world || !replacement_ || world->paused || world->mappingRevision.load() != revision_)
                    std::terminate();
                world->mapping = std::move(*replacement_);
                world->mappingRevision.store(revision_ + 1);
                replacement_.reset();
                published_ = true;
            }

            /** @brief Reports the committed batch only after all Scene entities and owners are visible. */
            Result<void> AfterPublication() override {
                if (!std::exchange(published_, false))
                    return Failure<void>(NetworkErrors::ReplicationWorldStale);
                if (metrics_ && ActiveWorld()) {
                    if (added_ != 0)
                        (void)metrics_->RecordReplication(NetworkMetricReplication::ObjectRegistered, added_);
                    if (retired_ != 0)
                        (void)metrics_->RecordReplication(NetworkMetricReplication::ObjectRetired, retired_);
                }
                return Result<void>::Success();
            }

        private:
            /** @brief Borrows the live epoch only on its owner lane. */
            std::shared_ptr<Detail::ReplicationWorldRecord> ActiveWorld() const noexcept {
                auto world = world_.lock();
                if (!world || world->owner != std::this_thread::get_id() || world->revoked.load())
                    return {};
                return world;
            }

            std::weak_ptr<Detail::ReplicationWorldRecord> world_;
            std::optional<NetworkObjectMapping> replacement_;
            CancellationToken cancellation_;
            std::uint64_t revision_;
            std::size_t added_;
            std::size_t retired_;
            NetworkMetrics *metrics_;
            bool published_{};
        };

        /** @brief Performs fallible delta work exclusively on private mapping storage. */
        Result<void> ApplyObjectDelta(NetworkObjectMapping &mapping, std::span<const NetworkObjectMappingEntry> added,
                                      std::span<const NetworkObjectId> retired) {
            for (const auto object : retired)
                if (auto removed = mapping.Retire(object); removed.HasError())
                    return removed;
            for (const auto &entry : added)
                if (auto inserted = mapping.Register(entry); inserted.HasError())
                    return inserted;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ReplicationWorldCapabilities> CapabilitiesForRole(const ReplicationExecutionRole role) {
            using enum ReplicationExecutionRole;
            switch (role) {
                case AuthorityServer:
                    return Result<ReplicationWorldCapabilities>::Success({.captureCanonicalState = true,
                                                                          .applyAuthoritativeState = false,
                                                                          .submitCommands = false,
                                                                          .publishAuthority = true});
                case AutonomousClient:
                    return Result<ReplicationWorldCapabilities>::Success({.captureCanonicalState = false,
                                                                          .applyAuthoritativeState = true,
                                                                          .submitCommands = true,
                                                                          .publishAuthority = false});
                case SimulatedClient:
                    return Result<ReplicationWorldCapabilities>::Success({.captureCanonicalState = false,
                                                                          .applyAuthoritativeState = true,
                                                                          .submitCommands = false,
                                                                          .publishAuthority = false});
                case Standalone:
                case Count:
                    return Failure<ReplicationWorldCapabilities>(NetworkErrors::ReplicationWorldInvalid);
            }
            return Failure<ReplicationWorldCapabilities>(NetworkErrors::ReplicationWorldInvalid);
        }

        [[nodiscard]] bool SameWorld(const ReplicationWorldActivationDescriptor &descriptor, const Runtime::SceneRuntimeId scene,
                                     const NetworkSessionGeneration session) noexcept {
            return descriptor.scene == scene && descriptor.session == session;
        }

        [[nodiscard]] bool ContainsWorld(const std::vector<std::shared_ptr<Detail::ReplicationWorldRecord>> &records,
                                         const ReplicationWorldActivationDescriptor &descriptor) noexcept {
            return std::ranges::any_of(records, [&descriptor](const auto &record) {
                return record->descriptor.scene == descriptor.scene && record->descriptor.session == descriptor.session;
            });
        }

        /** @brief Registers the complete sorted declaration set into a mapping that is still private. */
        Result<void> MapAuthoredObjects(NetworkObjectMapping &mapping, ReplicationAuthorityEpoch authority,
                                        std::vector<ReplicationAuthoredSceneObject> objects, const CancellationToken &cancellation) {
            std::ranges::sort(objects, {}, [](const auto &row) {
                return row.provenance.authoredObject;
            });
            std::optional<Runtime::SceneObjectId> previous;
            std::uint64_t slot = 1;
            for (const auto &row : objects) {
                if (cancellation.IsCancellationRequested())
                    return Failure<void>(NetworkErrors::ReplicationWorldCancelled);
                if (!row.provenance.authoredObject)
                    return Failure<void>(NetworkErrors::NetworkObjectMappingInvalid);
                if (previous == row.provenance.authoredObject)
                    return Failure<void>(NetworkErrors::NetworkObjectMappingConflict);
                auto identity = NetworkObjectId::Create(authority, slot++, 1);
                if (identity.HasError())
                    return Result<void>::Failure(identity.ErrorValue());
                if (auto registered = mapping.Register({identity.Value(), row.entity, row.provenance}); registered.HasError())
                    return registered;
                previous = row.provenance.authoredObject;
            }
            return Result<void>::Success();
        }

        /** @brief Constructs the detached authority-only mapping, including admission of the declared object budget. */
        Result<NetworkObjectMapping> PrepareWorldMapping(const ReplicationWorldActivationDescriptor &descriptor,
                                                         ReplicationWorldCapabilities capabilities, std::size_t maximumObjects,
                                                         std::span<const ReplicationAuthoredSceneObject> objects,
                                                         const CancellationToken &cancellation) {
            if (!objects.empty() && !capabilities.publishAuthority)
                return Failure<NetworkObjectMapping>(NetworkErrors::ReplicationAuthorityDenied);
            if (objects.size() > maximumObjects)
                return Failure<NetworkObjectMapping>(NetworkErrors::ReplicationWorldCapacityExceeded);
            auto created = NetworkObjectMapping::Create(descriptor.authority, descriptor.scene, maximumObjects);
            if (created.HasError())
                return created;
            auto mapping = std::move(created).Value();
            if (auto mapped = MapAuthoredObjects(mapping, descriptor.authority, {objects.begin(), objects.end()}, cancellation);
                mapped.HasError())
                return Result<NetworkObjectMapping>::Failure(mapped.ErrorValue());
            return Result<NetworkObjectMapping>::Success(std::move(mapping));
        }

        /** @brief Rejects a stale or incomplete remote inventory before allocating candidate storage. */
        Result<void> ValidateReplicaInventory(const ReplicationWorldActivationDescriptor &descriptor,
                                              const ReplicationAuthoredSceneInventory &inventory, std::size_t localCount,
                                              std::size_t maximumObjects) {
            if (descriptor.session != inventory.session || descriptor.authority != inventory.authority)
                return Failure<void>(NetworkErrors::ReplicationWorldStale);
            if (inventory.objects.size() > maximumObjects || localCount > maximumObjects)
                return Failure<void>(NetworkErrors::ReplicationWorldCapacityExceeded);
            if (localCount != inventory.objects.size())
                return Failure<void>(NetworkErrors::ReplicationDescriptorIncompatible);
            return Result<void>::Success();
        }

        /** @brief Matches stable authored provenance to local entities before changing the candidate map. */
        Result<void> MapReplicaAuthoredObjects(NetworkObjectMapping &mapping, const ReplicationAuthoredSceneInventory &inventory,
                                               std::vector<ReplicationAuthoredSceneObject> local, const CancellationToken &cancellation) {
            std::ranges::sort(local, {}, [](const ReplicationAuthoredSceneObject &row) {
                return row.provenance.authoredObject;
            });
            std::optional<Runtime::SceneObjectId> previous;
            for (std::size_t index = 0; index < local.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Failure<void>(NetworkErrors::ReplicationWorldCancelled);
                const auto &published = inventory.objects[index];
                if (!Detail::ValidAuthoredBinding(published, inventory.authority, index + 1, previous))
                    return Failure<void>(NetworkErrors::NetworkObjectMappingInvalid);
                const auto &entity = local[index];
                if (entity.provenance != published.provenance)
                    return Failure<void>(NetworkErrors::ReplicationDescriptorIncompatible);
                if (auto registered = mapping.Register({published.object, entity.entity, published.provenance}); registered.HasError())
                    return registered;
                previous = published.provenance.authoredObject;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ReplicationWorldActivationDescriptor::IsValid */
    bool ReplicationWorldActivationDescriptor::IsValid() const noexcept {
        return scene.IsValid() && session.IsValid() && authority.IsValid() && role != ReplicationExecutionRole::Standalone &&
               role != ReplicationExecutionRole::Count && phases.IsValid();
    }

    /** @copydoc ReplicationWorldReadLease::ReplicationWorldReadLease(std::shared_ptr<const Detail::ReplicationWorldRecord>,
     * NetworkObjectMappingSnapshot) */
    ReplicationWorldReadLease::ReplicationWorldReadLease(std::shared_ptr<const Detail::ReplicationWorldRecord> record,
                                                         NetworkObjectMappingSnapshot mapping) noexcept
        : record_(std::move(record)), mapping_(std::move(mapping)) {}

    /** @copydoc ReplicationWorldReadLease::~ReplicationWorldReadLease */
    ReplicationWorldReadLease::~ReplicationWorldReadLease() = default;

    /** @copydoc ReplicationWorldReadLease::IsValid */
    bool ReplicationWorldReadLease::IsValid() const noexcept {
        return static_cast<bool>(record_) && mapping_.has_value();
    }

    /** @copydoc ReplicationWorldReadLease::Descriptor */
    const ReplicationWorldActivationDescriptor &ReplicationWorldReadLease::Descriptor() const noexcept {
        return record_->descriptor;
    }

    /** @copydoc ReplicationWorldReadLease::Capabilities */
    const ReplicationWorldCapabilities &ReplicationWorldReadLease::Capabilities() const noexcept {
        return record_->capabilities;
    }

    /** @copydoc ReplicationWorldReadLease::Mapping */
    const NetworkObjectMappingSnapshot &ReplicationWorldReadLease::Mapping() const noexcept {
        return *mapping_;
    }

    /** @copydoc ReplicationWorldReadLease::Cancellation */
    CancellationToken ReplicationWorldReadLease::Cancellation() const noexcept {
        return record_ ? record_->cancellation.Token() : CancellationToken{};
    }

    /** @copydoc ReplicationWorldReadLease::IsRevoked */
    bool ReplicationWorldReadLease::IsRevoked() const noexcept {
        return !record_ || record_->revoked.load();
    }

    namespace {
        /** @brief Copies the canonical authored rows from detached or published mapping storage. */
        Result<ReplicationAuthoredSceneInventory> CopyAuthoredInventory(const ReplicationWorldActivationDescriptor &descriptor,
                                                                        std::span<const NetworkObjectMappingEntry> entries) {
            using Outcome = Result<ReplicationAuthoredSceneInventory>;
            ReplicationAuthoredSceneInventory inventory{descriptor.session, descriptor.authority};
            try {
                std::optional<Runtime::SceneObjectId> previous;
                for (const auto &entry : entries) {
                    if (!entry.provenance.authoredObject)
                        continue;
                    const ReplicationAuthoredObjectBinding binding{entry.object, entry.provenance};
                    if (!Detail::ValidAuthoredBinding(binding, inventory.authority, inventory.objects.size() + 1, previous))
                        return Failure<ReplicationAuthoredSceneInventory>(NetworkErrors::NetworkObjectMappingInvalid);
                    inventory.objects.push_back(binding);
                    previous = binding.provenance.authoredObject;
                }
                return Outcome::Success(std::move(inventory));
            } catch (const std::bad_alloc &) {
                return Failure<ReplicationAuthoredSceneInventory>(NetworkErrors::ReplicationWorldCapacityExceeded);
            }
        }
    }  // namespace

    /** @copydoc CaptureReplicationAuthoredScene */
    Result<ReplicationAuthoredSceneInventory> CaptureReplicationAuthoredScene(const ReplicationWorldReadLease &world) {
        if (!world.IsValid() || world.IsRevoked())
            return Failure<ReplicationAuthoredSceneInventory>(NetworkErrors::ReplicationWorldUnavailable);
        if (!world.Capabilities().publishAuthority)
            return Failure<ReplicationAuthoredSceneInventory>(NetworkErrors::ReplicationAuthorityDenied);
        auto captured = CopyAuthoredInventory(world.Descriptor(), world.Mapping().Entries());
        if (world.IsRevoked())
            return Failure<ReplicationAuthoredSceneInventory>(NetworkErrors::ReplicationWorldStale);
        return captured;
    }

    /** @copydoc ReplicationWorldLifecycle::CaptureStagedAuthoredScene */
    Result<ReplicationAuthoredSceneInventory> ReplicationWorldLifecycle::CaptureStagedAuthoredScene() const {
        using Outcome = Result<ReplicationAuthoredSceneInventory>;
        if (!staged_ || !staged_->capabilities.publishAuthority)
            return Failure<ReplicationAuthoredSceneInventory>(NetworkErrors::ReplicationAuthorityDenied);
        auto mapping = staged_->mapping.Snapshot();
        if (mapping.HasError())
            return Outcome::Failure(mapping.ErrorValue());
        return CopyAuthoredInventory(staged_->descriptor, mapping.Value().Entries());
    }

    ReplicationWorldLifecycle::ReplicationWorldLifecycle(const ReplicationWorldLimits limits,
                                                         std::vector<std::shared_ptr<Detail::ReplicationWorldRecord>> retired,
                                                         NetworkMetrics *metrics) noexcept
        : limits_(limits), retired_(std::move(retired)), metrics_(metrics) {}

    /** @copydoc ReplicationWorldLifecycle::ReplicationWorldLifecycle(ReplicationWorldLifecycle&&) */
    ReplicationWorldLifecycle::ReplicationWorldLifecycle(ReplicationWorldLifecycle &&other) noexcept
        : limits_(other.limits_), staged_(std::move(other.staged_)), active_(std::move(other.active_)), retired_(std::move(other.retired_)),
          state_(other.state_), metrics_(other.metrics_) {
        other.limits_ = {};
        other.state_ = ReplicationWorldLifecycleState::Closed;
        other.metrics_ = nullptr;
    }

    /** @copydoc ReplicationWorldLifecycle::~ReplicationWorldLifecycle */
    ReplicationWorldLifecycle::~ReplicationWorldLifecycle() {
        BeginShutdown();
    }

    /** @copydoc ReplicationWorldLifecycle::Create */
    Result<ReplicationWorldLifecycle> ReplicationWorldLifecycle::Create(const ReplicationWorldLimits limits, NetworkMetrics *metrics) {
        if (limits.maximumObjects == 0 || limits.maximumRetiredWorlds == 0 || limits.maximumRetiredWorlds > MaximumRetiredWorlds)
            return Failure<ReplicationWorldLifecycle>(NetworkErrors::ReplicationWorldInvalid);
        try {
            std::vector<std::shared_ptr<Detail::ReplicationWorldRecord>> retired;
            retired.reserve(limits.maximumRetiredWorlds);
            return Result<ReplicationWorldLifecycle>::Success(ReplicationWorldLifecycle{limits, std::move(retired), metrics});
        } catch (const std::bad_alloc &) {
            return Failure<ReplicationWorldLifecycle>(NetworkErrors::ReplicationWorldCapacityExceeded);
        }
    }

    /** @copydoc ReplicationWorldLifecycle::Stage */
    Result<void> ReplicationWorldLifecycle::Stage(const ReplicationWorldActivationDescriptor &descriptor,
                                                  const CancellationToken &cancellation) {
        return StageAuthoredScene(descriptor, {}, cancellation);
    }

    /** @copydoc ReplicationWorldLifecycle::StageAuthoredScene */
    Result<void> ReplicationWorldLifecycle::StageAuthoredScene(const ReplicationWorldActivationDescriptor &descriptor,
                                                               const std::span<const ReplicationAuthoredSceneObject> objects,
                                                               const CancellationToken &cancellation) {
        if (auto valid = ValidateStage(descriptor, cancellation); valid.HasError())
            return valid;
        const auto capabilities = CapabilitiesForRole(descriptor.role);
        if (capabilities.HasError())
            return Result<void>::Failure(capabilities.ErrorValue());
        try {
            auto mapping = PrepareWorldMapping(descriptor, capabilities.Value(), limits_.maximumObjects, objects, cancellation);
            if (mapping.HasError())
                return Result<void>::Failure(mapping.ErrorValue());
            return StoreStagedWorld(descriptor, capabilities.Value(), std::move(mapping).Value(), cancellation);
        } catch (const std::bad_alloc &) {
            return Failure<void>(NetworkErrors::ReplicationWorldCapacityExceeded);
        }
    }

    /** @copydoc ReplicationWorldLifecycle::StageReplicaAuthoredScene */
    Result<void> ReplicationWorldLifecycle::StageReplicaAuthoredScene(const ReplicationWorldActivationDescriptor &descriptor,
                                                                      std::span<const ReplicationAuthoredSceneObject> localObjects,
                                                                      const ReplicationAuthoredSceneInventory &inventory,
                                                                      const CancellationToken &cancellation) {
        if (auto valid = ValidateStage(descriptor, cancellation); valid.HasError())
            return valid;
        const auto capabilities = CapabilitiesForRole(descriptor.role);
        if (capabilities.HasError() || !capabilities.Value().applyAuthoritativeState)
            return Failure<void>(NetworkErrors::ReplicationAuthorityDenied);
        if (auto valid = ValidateReplicaInventory(descriptor, inventory, localObjects.size(), limits_.maximumObjects); valid.HasError())
            return valid;
        try {
            auto mapping = NetworkObjectMapping::Create(descriptor.authority, descriptor.scene, limits_.maximumObjects);
            if (mapping.HasError())
                return Result<void>::Failure(mapping.ErrorValue());
            auto candidate = std::move(mapping).Value();
            if (auto matched = MapReplicaAuthoredObjects(candidate, inventory, {localObjects.begin(), localObjects.end()}, cancellation);
                matched.HasError())
                return matched;
            return StoreStagedWorld(descriptor, capabilities.Value(), std::move(candidate), cancellation);
        } catch (const std::bad_alloc &) {
            return Failure<void>(NetworkErrors::ReplicationWorldCapacityExceeded);
        }
    }

    /** @copydoc ReplicationWorldLifecycle::StoreStagedWorld */
    Result<void> ReplicationWorldLifecycle::StoreStagedWorld(const ReplicationWorldActivationDescriptor &descriptor,
                                                             ReplicationWorldCapabilities capabilities, NetworkObjectMapping mapping,
                                                             const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Failure<void>(NetworkErrors::ReplicationWorldCancelled);
        staged_ = std::make_shared<Detail::ReplicationWorldRecord>(descriptor, capabilities, std::move(mapping), cancellation);
        return Result<void>::Success();
    }

    /** @copydoc ReplicationWorldLifecycle::ValidateStage */
    Result<void> ReplicationWorldLifecycle::ValidateStage(const ReplicationWorldActivationDescriptor &descriptor,
                                                          const CancellationToken &cancellation) const {
        using enum ReplicationWorldLifecycleState;
        if (state_ == ShuttingDown || state_ == Closed)
            return Failure<void>(NetworkErrors::ReplicationWorldShuttingDown);
        if (staged_)
            return Failure<void>(NetworkErrors::ReplicationWorldUnavailable);
        if (cancellation.IsCancellationRequested())
            return Failure<void>(NetworkErrors::ReplicationWorldCancelled);
        if (!descriptor.IsValid())
            return Failure<void>(NetworkErrors::ReplicationWorldInvalid);
        if ((active_ && SameWorld(active_->descriptor, descriptor.scene, descriptor.session)) || ContainsWorld(retired_, descriptor))
            return Failure<void>(NetworkErrors::ReplicationWorldStale);
        return Result<void>::Success();
    }

    /** @copydoc ReplicationWorldLifecycle::CommitAtSafePoint(Runtime::SceneRuntimeId, NetworkSessionGeneration) */
    Result<void> ReplicationWorldLifecycle::CommitAtSafePoint(const Runtime::SceneRuntimeId expectedScene,
                                                              const NetworkSessionGeneration expectedSession) {
        return CommitAtSafePoint(expectedScene, expectedSession, Runtime::RuntimePhase::CommitDeferredLifecycleChanges);
    }

    /** @copydoc ReplicationWorldLifecycle::CommitAtSafePoint(Runtime::SceneRuntimeId, NetworkSessionGeneration, Runtime::RuntimePhase) */
    Result<void> ReplicationWorldLifecycle::CommitAtSafePoint(const Runtime::SceneRuntimeId expectedScene,
                                                              const NetworkSessionGeneration expectedSession,
                                                              const Runtime::RuntimePhase phase) {
        using enum ReplicationWorldLifecycleState;
        if (phase != Runtime::RuntimePhase::CommitDeferredLifecycleChanges)
            return Failure<void>(NetworkErrors::ReplicationWorldPhaseInvalid);
        if (state_ == ShuttingDown || state_ == Closed)
            return Failure<void>(NetworkErrors::ReplicationWorldShuttingDown);
        if (!staged_)
            return Failure<void>(NetworkErrors::ReplicationWorldUnavailable);
        if (!expectedScene.IsValid() || !expectedSession.IsValid() || !SameWorld(staged_->descriptor, expectedScene, expectedSession))
            return Failure<void>(NetworkErrors::ReplicationWorldStale);
        if (staged_->activationCancellation.IsCancellationRequested()) {
            staged_.reset();
            return Failure<void>(NetworkErrors::ReplicationWorldCancelled);
        }
        if (!CanRetireActive())
            return Failure<void>(NetworkErrors::ReplicationWorldCapacityExceeded);

        staged_->activationCancellation = {};
        if (active_)
            RetireActive();
        active_ = std::move(staged_);
        state_ = Active;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationWorldLifecycle::Pause */
    Result<void> ReplicationWorldLifecycle::Pause(const Runtime::SceneRuntimeId scene, const NetworkSessionGeneration session) noexcept {
        using enum ReplicationWorldLifecycleState;
        if (const auto identity = RequireWorldIdentity(scene, session); identity.HasError())
            return identity;
        if (state_ != Active && state_ != Paused)
            return Failure<void>(NetworkErrors::ReplicationWorldUnavailable);
        state_ = Paused;
        active_->paused = true;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationWorldLifecycle::Resume */
    Result<void> ReplicationWorldLifecycle::Resume(const Runtime::SceneRuntimeId scene, const NetworkSessionGeneration session) noexcept {
        using enum ReplicationWorldLifecycleState;
        if (const auto identity = RequireWorldIdentity(scene, session); identity.HasError())
            return identity;
        if (state_ != Paused)
            return Failure<void>(NetworkErrors::ReplicationWorldUnavailable);
        state_ = Active;
        active_->paused = false;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationWorldLifecycle::Revoke */
    Result<void> ReplicationWorldLifecycle::Revoke(const Runtime::SceneRuntimeId scene, const NetworkSessionGeneration session) noexcept {
        using enum ReplicationWorldLifecycleState;
        if (const auto identity = RequireWorldIdentity(scene, session); identity.HasError())
            return identity;
        if (!CanRetireActive())
            return Failure<void>(NetworkErrors::ReplicationWorldCapacityExceeded);
        if (staged_) {
            RevokeRecord(staged_);
            staged_.reset();
        }
        RetireActive();
        state_ = Empty;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationWorldLifecycle::Unload */
    Result<void> ReplicationWorldLifecycle::Unload(const Runtime::SceneRuntimeId scene, const NetworkSessionGeneration session) noexcept {
        return Revoke(scene, session);
    }

    /** @copydoc ReplicationWorldLifecycle::RegisterObject */
    // Mutates the owner-held mapping through its active record; no lifecycle member is changed directly.
    Result<void> ReplicationWorldLifecycle::RegisterObject(  // NOSONAR(cpp:S5817)
        const Runtime::SceneRuntimeId scene, const NetworkSessionGeneration session, const NetworkObjectMappingEntry &entry) {
        if (const auto active = RequireActive(scene, session); active.HasError())
            return active;
        if (active_->mappingRevision.load() == std::numeric_limits<std::uint64_t>::max())
            return Failure<void>(NetworkErrors::ReplicationWorldCapacityExceeded);
        auto result = active_->mapping.Register(entry);
        if (result.HasValue())
            active_->mappingRevision.fetch_add(1);
        if (result.HasValue() && metrics_)
            (void)metrics_->RecordReplication(NetworkMetricReplication::ObjectRegistered);
        return result;
    }

    /** @copydoc ReplicationWorldLifecycle::RetireObject */
    // Mutates the owner-held mapping through its active record; no lifecycle member is changed directly.
    Result<void> ReplicationWorldLifecycle::RetireObject(  // NOSONAR(cpp:S5817)
        const Runtime::SceneRuntimeId scene, const NetworkSessionGeneration session, const NetworkObjectId object) {
        if (const auto active = RequireActive(scene, session); active.HasError())
            return active;
        if (active_->mappingRevision.load() == std::numeric_limits<std::uint64_t>::max())
            return Failure<void>(NetworkErrors::ReplicationWorldCapacityExceeded);
        auto result = active_->mapping.Retire(object);
        if (result.HasValue())
            active_->mappingRevision.fetch_add(1);
        if (result.HasValue() && metrics_)
            (void)metrics_->RecordReplication(NetworkMetricReplication::ObjectRetired);
        return result;
    }

    /** @copydoc ReplicationWorldLifecycle::ValidateObjectBatch */
    Result<void> ReplicationWorldLifecycle::ValidateObjectBatch(const ReplicationWorldWorkRequest &request,
                                                                ReplicationWorldCapability capability) const {
        if (auto active = RequireActive(request.scene, request.session); active.HasError())
            return active;
        if (active_->owner != std::this_thread::get_id() || active_->paused)
            return Failure<void>(NetworkErrors::ReplicationWorldUnavailable);
        if (request.phase != Runtime::RuntimePhase::CommitDeferredLifecycleChanges || !active_->descriptor.phases.Contains(request.phase))
            return Failure<void>(NetworkErrors::ReplicationWorldPhaseInvalid);
        if (!active_->capabilities.Contains(capability))
            return Failure<void>(NetworkErrors::ReplicationAuthorityDenied);
        if (request.cancellation.IsCancellationRequested())
            return Failure<void>(NetworkErrors::ReplicationWorldCancelled);
        return Result<void>::Success();
    }

    /** @copydoc ReplicationWorldLifecycle::CopyMappingForBatch */
    Result<NetworkObjectMapping> ReplicationWorldLifecycle::CopyMappingForBatch(const ReplicationWorldWorkRequest &request,
                                                                                std::size_t added, std::size_t retired,
                                                                                ReplicationWorldCapability capability) const {
        if (auto valid = ValidateObjectBatch(request, capability); valid.HasError())
            return Result<NetworkObjectMapping>::Failure(valid.ErrorValue());
        if (added == 0 && retired == 0)
            return Failure<NetworkObjectMapping>(NetworkErrors::ReplicationWorldInvalid);
        if (std::max(added, retired) > limits_.maximumObjects ||
            active_->mappingRevision.load() == std::numeric_limits<std::uint64_t>::max())
            return Failure<NetworkObjectMapping>(NetworkErrors::ReplicationWorldCapacityExceeded);
        return active_->mapping.CloneForPublication();
    }

    /** @copydoc ReplicationWorldLifecycle::FinishObjectBatch */
    Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> ReplicationWorldLifecycle::FinishObjectBatch(
        NetworkObjectMapping mapping, const CancellationToken &cancellation, std::size_t added, std::size_t retired) const {
        return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(
            std::make_unique<ObjectBatchCandidate>(active_, std::move(mapping), cancellation, added, retired, metrics_));
    }

    /** @copydoc ReplicationWorldLifecycle::PrepareObjectBatch */
    Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> ReplicationWorldLifecycle::PrepareObjectBatch(
        const ReplicationWorldWorkRequest &request, const std::span<const NetworkObjectMappingEntry> added,
        const std::span<const NetworkObjectId> retired) const {
        using Prepared = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
        try {
            auto copied = CopyMappingForBatch(request, added.size(), retired.size());
            if (copied.HasError())
                return Prepared::Failure(copied.ErrorValue());
            auto replacement = std::move(copied).Value();
            if (auto applied = ApplyObjectDelta(replacement, added, retired); applied.HasError())
                return Prepared::Failure(applied.ErrorValue());
            return FinishObjectBatch(std::move(replacement), request.cancellation, added.size(), retired.size());
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(NetworkErrors::ReplicationWorldCapacityExceeded);
        }
    }

    /** @copydoc ReplicationWorldLifecycle::AddDynamicObjects */
    Result<void> ReplicationWorldLifecycle::AddDynamicObjects(NetworkObjectMapping &mapping,
                                                              std::span<const ReplicationDynamicSceneObject> added) {
        if (added.empty())
            return Result<void>::Success();
        const auto &first = added.front().prefab;
        NetworkObjectId occurrence;
        if (first) {
            auto identified = mapping.NextDynamicObject();
            if (identified.HasError())
                return Result<void>::Failure(identified.ErrorValue());
            occurrence = identified.Value();
        }
        std::optional<std::uint32_t> previousSlot;
        for (const auto &entry : added) {
            NetworkObjectProvenance provenance{.schema = entry.schema, .schemaVersion = entry.version};
            if (entry.prefab.has_value() != first.has_value())
                return Failure<void>(NetworkErrors::NetworkObjectMappingInvalid);
            if (entry.prefab) {
                const auto slot = entry.prefab->entitySlot;
                if (entry.prefab->recipe != first->recipe || (previousSlot && slot <= *previousSlot))
                    return Failure<void>(NetworkErrors::NetworkObjectMappingInvalid);
                previousSlot = slot;
                provenance.prefab.emplace(*entry.prefab, occurrence);
            }
            if (auto inserted = mapping.RegisterDynamic(entry.entity, provenance, entry.prefabIntent); inserted.HasError())
                return inserted;
        }
        return Result<void>::Success();
    }

    /** @copydoc ReplicationWorldLifecycle::PrepareDynamicObjectBatch */
    Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> ReplicationWorldLifecycle::PrepareDynamicObjectBatch(
        const ReplicationWorldWorkRequest &request, const std::span<const ReplicationDynamicSceneObject> added,
        const std::span<const Runtime::EntityRef> retired) const {
        using Prepared = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
        try {
            auto copied = CopyMappingForBatch(request, added.size(), retired.size());
            if (copied.HasError())
                return Prepared::Failure(copied.ErrorValue());
            auto replacement = std::move(copied).Value();
            for (const auto entity : retired)
                if (auto removed = replacement.RetireDynamic(entity); removed.HasError())
                    return Prepared::Failure(removed.ErrorValue());
            if (auto inserted = AddDynamicObjects(replacement, added); inserted.HasError())
                return Prepared::Failure(inserted.ErrorValue());
            return FinishObjectBatch(std::move(replacement), request.cancellation, added.size(), retired.size());
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(NetworkErrors::ReplicationWorldCapacityExceeded);
        }
    }

    /** @copydoc ReplicationWorldLifecycle::ApplyReplicaPrefab */
    Result<void> ReplicationWorldLifecycle::ApplyReplicaPrefab(NetworkObjectMapping &mapping, const NetworkPrefabLifecycleRecord &spawn,
                                                               std::span<const Runtime::EntityRef> members, bool creating) {
        for (const auto &member : spawn.spawn->members) {
            const NetworkObjectMappingEntry expected{member.object,
                                                     members[member.entitySlot],
                                                     {member.schema, member.version, std::nullopt,
                                                      NetworkPrefabMemberProvenance{{spawn.spawn->recipe, member.entitySlot},
                                                                                    spawn.occurrence}}};
            if (creating) {
                if (auto added = mapping.RegisterReplica(expected); added.HasError())
                    return added;
                continue;
            }
            if (const auto live = mapping.ResolveEntry(member.object); live.HasError() || live.Value() != expected)
                return Failure<void>(NetworkErrors::NetworkObjectMappingUnknown);
            if (auto removed = mapping.Retire(member.object); removed.HasError())
                return removed;
        }
        return Result<void>::Success();
    }

    /** @copydoc ReplicationWorldLifecycle::PrepareReplicaPrefab */
    Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> ReplicationWorldLifecycle::PrepareReplicaPrefab(
        const ReplicationWorldWorkRequest &request, const NetworkPrefabLifecycleRecord &spawn, std::span<const Runtime::EntityRef> members,
        bool creating) const {
        using Prepared = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
        if (!spawn.spawn || spawn.session != request.session || spawn.spawn->recipe.entityCount != members.size())
            return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(NetworkErrors::ReplicationWorldInvalid);
        if (std::ranges::any_of(members, [&request](Runtime::EntityRef entity) {
            return !entity.IsValid() || entity.runtime != request.scene;
        }))
            return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(NetworkErrors::ReplicationWorldInvalid);
        if (auto valid = ValidateNetworkPrefabLifecycle(spawn); valid.HasError())
            return Prepared::Failure(valid.ErrorValue());
        const auto count = spawn.spawn->members.size();
        const auto added = creating ? count : 0;
        const auto retired = creating ? 0 : count;
        try {
            auto copied = CopyMappingForBatch(request, added, retired, ReplicationWorldCapability::ApplyAuthoritativeState);
            if (copied.HasError())
                return Prepared::Failure(copied.ErrorValue());
            auto replacement = std::move(copied).Value();
            if (auto applied = ApplyReplicaPrefab(replacement, spawn, members, creating); applied.HasError())
                return Prepared::Failure(applied.ErrorValue());
            return FinishObjectBatch(std::move(replacement), request.cancellation, added, retired);
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(NetworkErrors::ReplicationWorldCapacityExceeded);
        }
    }

    /** @copydoc ReplicationWorldLifecycle::Acquire */
    Result<ReplicationWorldReadLease> ReplicationWorldLifecycle::Acquire(const ReplicationWorldWorkRequest &request) const {
        return AcquireInternal(request, nullptr);
    }

    /** @copydoc ReplicationWorldLifecycle::AcquireFor */
    Result<ReplicationWorldReadLease> ReplicationWorldLifecycle::AcquireFor(const ReplicationWorldWorkRequest &request,
                                                                            const ReplicationWorldCapability capability) const {
        return AcquireInternal(request, &capability);
    }

    Result<ReplicationWorldReadLease> ReplicationWorldLifecycle::AcquireInternal(const ReplicationWorldWorkRequest &request,
                                                                                 const ReplicationWorldCapability *capability) const {
        using enum ReplicationWorldLifecycleState;
        if (!request.scene.IsValid() || !request.session.IsValid())
            return Failure<ReplicationWorldReadLease>(NetworkErrors::ReplicationWorldInvalid);
        if (state_ == ShuttingDown || state_ == Closed)
            return Failure<ReplicationWorldReadLease>(NetworkErrors::ReplicationWorldShuttingDown);
        if (!active_ || !SameWorld(active_->descriptor, request.scene, request.session))
            return Failure<ReplicationWorldReadLease>(NetworkErrors::ReplicationWorldStale);
        if (state_ != Active)
            return Failure<ReplicationWorldReadLease>(NetworkErrors::ReplicationWorldUnavailable);
        if (!active_->descriptor.phases.Contains(request.phase))
            return Failure<ReplicationWorldReadLease>(NetworkErrors::ReplicationWorldPhaseInvalid);
        if (request.phase == Runtime::RuntimePhase::FixedUpdate && request.simulationTick == 0)
            return Failure<ReplicationWorldReadLease>(NetworkErrors::ReplicationWorldInvalid);
        if (request.cancellation.IsCancellationRequested() || active_->cancellation.Token().IsCancellationRequested())
            return Failure<ReplicationWorldReadLease>(NetworkErrors::ReplicationWorldCancelled);
        if (capability != nullptr && !active_->capabilities.Contains(*capability))
            return Failure<ReplicationWorldReadLease>(NetworkErrors::ReplicationAuthorityDenied);

        auto mapping = active_->mapping.Snapshot();
        if (mapping.HasError())
            return Failure<ReplicationWorldReadLease>(NetworkErrors::ReplicationWorldUnavailable);
        try {
            return Result<ReplicationWorldReadLease>::Success(ReplicationWorldReadLease{active_, std::move(mapping).Value()});
        } catch (const std::bad_alloc &) {
            return Failure<ReplicationWorldReadLease>(NetworkErrors::ReplicationWorldCapacityExceeded);
        }
    }

    /** @copydoc ReplicationWorldCaptureRead::ReplicationWorldCaptureRead */
    ReplicationWorldCaptureRead::ReplicationWorldCaptureRead(std::shared_ptr<const Detail::ReplicationWorldRecord> record,
                                                             const std::uint64_t revision, const std::uint64_t tick) noexcept
        : record_(std::move(record)), revision_(revision), tick_(tick) {}

    /** @copydoc ReplicationWorldCaptureRead::IsCurrent */
    bool ReplicationWorldCaptureRead::IsCurrent() const noexcept {
        return record_ && !record_->revoked.load() && record_->mappingRevision.load() == revision_;
    }

    /** @copydoc ReplicationWorldCaptureRead::Descriptor */
    const ReplicationWorldActivationDescriptor &ReplicationWorldCaptureRead::Descriptor() const noexcept {
        return record_->descriptor;
    }

    /** @copydoc ReplicationWorldCaptureRead::MappingRevision */
    std::uint64_t ReplicationWorldCaptureRead::MappingRevision() const noexcept {
        return revision_;
    }

    /** @copydoc ReplicationWorldCaptureRead::SimulationTick */
    std::uint64_t ReplicationWorldCaptureRead::SimulationTick() const noexcept {
        return tick_;
    }

    /** @copydoc ReplicationWorldCaptureRead::Resolve */
    Result<NetworkObjectMappingEntry> ReplicationWorldCaptureRead::Resolve(const NetworkObjectId object) const {
        if (!IsCurrent() || record_->owner != std::this_thread::get_id())
            return Failure<NetworkObjectMappingEntry>(NetworkErrors::ReplicationWorldStale);
        return record_->mapping.ResolveEntry(object);
    }

    /** @copydoc ReplicationWorldLifecycle::ObjectMappingRevision */
    std::uint64_t ReplicationWorldLifecycle::ObjectMappingRevision() const noexcept {
        return state_ == ReplicationWorldLifecycleState::Active && active_ ? active_->mappingRevision.load() : 0;
    }

    /** @copydoc ReplicationWorldLifecycle::FindAuthorityObject */
    Result<NetworkObjectId> ReplicationWorldLifecycle::FindAuthorityObject(Runtime::EntityRef entity,
                                                                           NetworkSessionGeneration session) const {
        if (auto active = RequireActive(entity.runtime, session); active.HasError())
            return Result<NetworkObjectId>::Failure(active.ErrorValue());
        if (active_->owner != std::this_thread::get_id() || !active_->capabilities.publishAuthority)
            return Failure<NetworkObjectId>(NetworkErrors::ReplicationAuthorityDenied);
        return active_->mapping.Find(entity);
    }

    /** @copydoc ReplicationWorldLifecycle::ResolveReplicaObject */
    Result<Runtime::EntityRef> ReplicationWorldLifecycle::ResolveReplicaObject(Runtime::SceneRuntimeId scene,
                                                                               NetworkSessionGeneration session,
                                                                               NetworkObjectId object) const {
        if (auto active = RequireActive(scene, session); active.HasError())
            return Result<Runtime::EntityRef>::Failure(active.ErrorValue());
        if (active_->owner != std::this_thread::get_id() || !active_->capabilities.applyAuthoritativeState)
            return Failure<Runtime::EntityRef>(NetworkErrors::ReplicationAuthorityDenied);
        return active_->mapping.Resolve(object);
    }

    /** @copydoc ReplicationWorldLifecycle::AcquireCaptureRead */
    Result<ReplicationWorldCaptureRead> ReplicationWorldLifecycle::AcquireCaptureRead(const ReplicationWorldWorkRequest &request) const {
        if (const auto active = RequireActive(request.scene, request.session); active.HasError())
            return Result<ReplicationWorldCaptureRead>::Failure(active.ErrorValue());
        if (active_->owner != std::this_thread::get_id() || state_ != ReplicationWorldLifecycleState::Active)
            return Failure<ReplicationWorldCaptureRead>(NetworkErrors::ReplicationWorldUnavailable);
        if (request.phase != Runtime::RuntimePhase::NetworkFlush || !active_->descriptor.phases.Contains(request.phase))
            return Failure<ReplicationWorldCaptureRead>(NetworkErrors::ReplicationWorldPhaseInvalid);
        if (request.simulationTick == 0)
            return Failure<ReplicationWorldCaptureRead>(NetworkErrors::ReplicationWorldInvalid);
        if (request.cancellation.IsCancellationRequested() || active_->cancellation.Token().IsCancellationRequested())
            return Failure<ReplicationWorldCaptureRead>(NetworkErrors::ReplicationWorldCancelled);
        if (!active_->capabilities.captureCanonicalState)
            return Failure<ReplicationWorldCaptureRead>(NetworkErrors::ReplicationAuthorityDenied);
        return Result<ReplicationWorldCaptureRead>::Success(
            ReplicationWorldCaptureRead{active_, active_->mappingRevision.load(), request.simulationTick});
    }

    /** @copydoc ReplicationWorldLifecycle::BeginShutdown */
    void ReplicationWorldLifecycle::BeginShutdown() noexcept {
        using enum ReplicationWorldLifecycleState;
        if (state_ == Closed)
            return;
        state_ = ShuttingDown;
        if (staged_) {
            RevokeRecord(staged_);
            staged_.reset();
        }
        if (active_)
            RevokeRecord(active_);
        for (const auto &record : retired_)
            RevokeRecord(record);
        static_cast<void>(CollectRetired());
    }

    /** @copydoc ReplicationWorldLifecycle::CollectRetired */
    ReplicationWorldLifecycleState ReplicationWorldLifecycle::CollectRetired() noexcept {
        using enum ReplicationWorldLifecycleState;
        std::erase_if(retired_, [](const auto &record) {
            return record.use_count() == 1;
        });
        if (state_ == ShuttingDown && active_ && active_.use_count() == 1)
            active_.reset();
        if (state_ == ShuttingDown && !active_ && !staged_ && retired_.empty())
            state_ = Closed;
        return state_;
    }

    /** @copydoc ReplicationWorldLifecycle::ActiveDescriptor */
    Result<ReplicationWorldActivationDescriptor> ReplicationWorldLifecycle::ActiveDescriptor() const {
        if (!active_)
            return Failure<ReplicationWorldActivationDescriptor>(NetworkErrors::ReplicationWorldUnavailable);
        return Result<ReplicationWorldActivationDescriptor>::Success(active_->descriptor);
    }

    /** @copydoc ReplicationWorldLifecycle::State */
    ReplicationWorldLifecycleState ReplicationWorldLifecycle::State() const noexcept {
        return state_;
    }

    /** @copydoc ReplicationWorldLifecycle::RetiredCount */
    std::size_t ReplicationWorldLifecycle::RetiredCount() const noexcept {
        return retired_.size();
    }

    /** @copydoc ReplicationWorldLifecycle::HasStagedCandidate */
    bool ReplicationWorldLifecycle::HasStagedCandidate() const noexcept {
        return static_cast<bool>(staged_);
    }

    Result<void> ReplicationWorldLifecycle::RequireWorldIdentity(const Runtime::SceneRuntimeId scene,
                                                                 const NetworkSessionGeneration session) const {
        using enum ReplicationWorldLifecycleState;
        if (!scene.IsValid() || !session.IsValid())
            return Failure<void>(NetworkErrors::ReplicationWorldInvalid);
        if (state_ == ShuttingDown || state_ == Closed)
            return Failure<void>(NetworkErrors::ReplicationWorldShuttingDown);
        if (!active_ || !SameWorld(active_->descriptor, scene, session))
            return Failure<void>(NetworkErrors::ReplicationWorldStale);
        return Result<void>::Success();
    }

    Result<void> ReplicationWorldLifecycle::RequireActive(const Runtime::SceneRuntimeId scene,
                                                          const NetworkSessionGeneration session) const {
        if (const auto identity = RequireWorldIdentity(scene, session); identity.HasError())
            return identity;
        if (state_ != ReplicationWorldLifecycleState::Active && state_ != ReplicationWorldLifecycleState::Paused)
            return Failure<void>(NetworkErrors::ReplicationWorldUnavailable);
        return Result<void>::Success();
    }

    bool ReplicationWorldLifecycle::CanRetireActive() const noexcept {
        return !active_ || active_.use_count() == 1 || retired_.size() < limits_.maximumRetiredWorlds;
    }

    /** @copydoc ReplicationWorldLifecycle::RevokeRecord */
    void ReplicationWorldLifecycle::RevokeRecord(const std::shared_ptr<Detail::ReplicationWorldRecord> &record) noexcept {
        record->revoked.store(true);
        record->cancellation.RequestCancellation();
        record->mapping.BeginShutdown();
    }

    /** @copydoc ReplicationWorldLifecycle::RetireActive */
    void ReplicationWorldLifecycle::RetireActive() noexcept {
        if (!active_)
            return;
        RevokeRecord(active_);
        if (active_.use_count() > 1)
            retired_.push_back(active_);
        active_.reset();
    }
}  // namespace Horo::Network
