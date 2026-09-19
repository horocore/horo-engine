#include "Horo/Network/ReplicationWorldLifecycle.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Network {
    namespace Detail {
        /** @brief Immutable identity/capability record retained by world leases after logical revocation. */
        struct ReplicationWorldRecord final {
            ReplicationWorldRecord(const ReplicationWorldActivationDescriptor &activation,
                                   const ReplicationWorldCapabilities worldCapabilities, NetworkObjectMapping worldMapping) noexcept
                : descriptor(activation), capabilities(worldCapabilities), mapping(std::move(worldMapping)) {}

            ReplicationWorldActivationDescriptor descriptor;
            ReplicationWorldCapabilities capabilities;
            NetworkObjectMapping mapping;
            CancellationSource cancellation;
            std::atomic<bool> revoked{false};
        };
    }  // namespace Detail

    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
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

    ReplicationWorldLifecycle::ReplicationWorldLifecycle(const ReplicationWorldLimits limits,
                                                         std::vector<std::shared_ptr<Detail::ReplicationWorldRecord>> retired) noexcept
        : limits_(limits), retired_(std::move(retired)) {}

    /** @copydoc ReplicationWorldLifecycle::ReplicationWorldLifecycle(ReplicationWorldLifecycle&&) */
    ReplicationWorldLifecycle::ReplicationWorldLifecycle(ReplicationWorldLifecycle &&other) noexcept
        : limits_(other.limits_), staged_(std::move(other.staged_)), active_(std::move(other.active_)), retired_(std::move(other.retired_)),
          state_(other.state_) {
        other.limits_ = {};
        other.state_ = ReplicationWorldLifecycleState::Closed;
    }

    /** @copydoc ReplicationWorldLifecycle::~ReplicationWorldLifecycle */
    ReplicationWorldLifecycle::~ReplicationWorldLifecycle() {
        BeginShutdown();
    }

    /** @copydoc ReplicationWorldLifecycle::Create */
    Result<ReplicationWorldLifecycle> ReplicationWorldLifecycle::Create(const ReplicationWorldLimits limits) {
        if (limits.maximumObjects == 0 || limits.maximumRetiredWorlds == 0 || limits.maximumRetiredWorlds > MaximumRetiredWorlds)
            return Failure<ReplicationWorldLifecycle>(NetworkErrors::ReplicationWorldInvalid);
        try {
            std::vector<std::shared_ptr<Detail::ReplicationWorldRecord>> retired;
            retired.reserve(limits.maximumRetiredWorlds);
            return Result<ReplicationWorldLifecycle>::Success(ReplicationWorldLifecycle{limits, std::move(retired)});
        } catch (const std::bad_alloc &) {
            return Failure<ReplicationWorldLifecycle>(NetworkErrors::ReplicationWorldCapacityExceeded);
        }
    }

    /** @copydoc ReplicationWorldLifecycle::Stage */
    Result<void> ReplicationWorldLifecycle::Stage(const ReplicationWorldActivationDescriptor &descriptor,
                                                  const CancellationToken &cancellation) {
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

        const auto capabilities = CapabilitiesForRole(descriptor.role);
        if (capabilities.HasError())
            return Result<void>::Failure(capabilities.ErrorValue());
        auto mapping = NetworkObjectMapping::Create(descriptor.authority, descriptor.scene, limits_.maximumObjects);
        if (mapping.HasError())
            return Result<void>::Failure(mapping.ErrorValue());
        try {
            staged_ = std::make_shared<Detail::ReplicationWorldRecord>(descriptor, capabilities.Value(), std::move(mapping).Value());
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure<void>(NetworkErrors::ReplicationWorldCapacityExceeded);
        }
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
        if (!CanRetireActive())
            return Failure<void>(NetworkErrors::ReplicationWorldCapacityExceeded);

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
        return active_->mapping.Register(entry);
    }

    /** @copydoc ReplicationWorldLifecycle::RetireObject */
    // Mutates the owner-held mapping through its active record; no lifecycle member is changed directly.
    Result<void> ReplicationWorldLifecycle::RetireObject(  // NOSONAR(cpp:S5817)
        const Runtime::SceneRuntimeId scene, const NetworkSessionGeneration session, const NetworkObjectId object) {
        if (const auto active = RequireActive(scene, session); active.HasError())
            return active;
        return active_->mapping.Retire(object);
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
