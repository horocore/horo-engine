#include "Horo/Navigation/NavigationWorldLifecycle.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <new>
#include <utility>

namespace Horo::Navigation {
    namespace Detail {
        /** @brief Immutable activation and provider ownership retained by read leases after logical revocation. */
        struct NavigationWorldRecord final {
            NavigationWorldRecord(const NavigationWorldActivationDescriptor &activation, std::unique_ptr<INavigationQueryBackend> provider)
                : descriptor(activation), backend(std::move(provider)) {}

            ~NavigationWorldRecord() = default;

            NavigationWorldActivationDescriptor descriptor;
            std::unique_ptr<INavigationQueryBackend> backend;
            CancellationSource cancellation;
            std::atomic<NavigationWorldRevocation> revocation{NavigationWorldRevocation::None};
        };
    }  // namespace Detail

    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool ContainsWorld(const std::vector<std::shared_ptr<Detail::NavigationWorldRecord>> &records,
                                         const NavigationWorldId world) noexcept {
            return std::ranges::any_of(records, [world](const auto &record) {
                return record->descriptor.world == world;
            });
        }

        /** @brief Validate identity continuity for an immutable topology replacement within the active world. */
        [[nodiscard]] Result<void> ValidateTopologyReplacement(const NavigationWorldActivationDescriptor &active,
                                                               const NavigationWorldActivationDescriptor &candidate) {
            if (candidate.scene != active.scene || candidate.sceneGeneration != active.sceneGeneration)
                return Failure<void>(NavigationErrors::InvalidWorld);
            if (candidate.topology == active.topology)
                return Failure<void>(NavigationErrors::InvalidWorld);
            if (candidate.topology < active.topology)
                return Failure<void>(NavigationErrors::StaleSnapshot);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc NavigationWorldActivationDescriptor::IsValid */
    bool NavigationWorldActivationDescriptor::IsValid() const noexcept {
        return scene.IsValid() && sceneGeneration.IsValid() && world.IsValid() && topology.IsValid();
    }

    /** @brief Construct the owner-issued lease around one retained record. */
    NavigationWorldReadLease::NavigationWorldReadLease(std::shared_ptr<const Detail::NavigationWorldRecord> record) noexcept
        : record_(std::move(record)) {}

    /** @copydoc NavigationWorldReadLease::NavigationWorldReadLease(const NavigationWorldReadLease&) */
    NavigationWorldReadLease::NavigationWorldReadLease(const NavigationWorldReadLease &other) noexcept = default;
    /** @copydoc NavigationWorldReadLease::operator=(const NavigationWorldReadLease&) */
    NavigationWorldReadLease &NavigationWorldReadLease::operator=(const NavigationWorldReadLease &other) noexcept = default;
    /** @copydoc NavigationWorldReadLease::NavigationWorldReadLease(NavigationWorldReadLease&&) */
    NavigationWorldReadLease::NavigationWorldReadLease(NavigationWorldReadLease &&other) noexcept = default;
    /** @copydoc NavigationWorldReadLease::operator=(NavigationWorldReadLease&&) */
    NavigationWorldReadLease &NavigationWorldReadLease::operator=(NavigationWorldReadLease &&other) noexcept = default;
    /** @copydoc NavigationWorldReadLease::~NavigationWorldReadLease */
    NavigationWorldReadLease::~NavigationWorldReadLease() = default;

    /** @copydoc NavigationWorldReadLease::IsValid */
    bool NavigationWorldReadLease::IsValid() const noexcept {
        return static_cast<bool>(record_);
    }

    /** @copydoc NavigationWorldReadLease::Descriptor */
    const NavigationWorldActivationDescriptor &NavigationWorldReadLease::Descriptor() const noexcept {
        return record_->descriptor;
    }

    /** @copydoc NavigationWorldReadLease::Backend */
    const INavigationQueryBackend &NavigationWorldReadLease::Backend() const noexcept {
        return *record_->backend;
    }

    /** @copydoc NavigationWorldReadLease::Cancellation */
    CancellationToken NavigationWorldReadLease::Cancellation() const noexcept {
        return record_->cancellation.Token();
    }

    /** @copydoc NavigationWorldReadLease::IsRevoked */
    bool NavigationWorldReadLease::IsRevoked() const noexcept {
        return Revocation() != NavigationWorldRevocation::None;
    }

    /** @copydoc NavigationWorldReadLease::Revocation */
    NavigationWorldRevocation NavigationWorldReadLease::Revocation() const noexcept {
        return record_ ? record_->revocation.load() : NavigationWorldRevocation::None;
    }

    /** @copydoc NavigationWorldLifecycle::NavigationWorldLifecycle(std::uint32_t,
     * std::vector<std::shared_ptr<Detail::NavigationWorldRecord>>) */
    NavigationWorldLifecycle::NavigationWorldLifecycle(const std::uint32_t maximumRetiredWorlds,
                                                       std::vector<std::shared_ptr<Detail::NavigationWorldRecord>> retired) noexcept
        : maximumRetiredWorlds_(maximumRetiredWorlds), retired_(std::move(retired)) {}

    /** @copydoc NavigationWorldLifecycle::NavigationWorldLifecycle(NavigationWorldLifecycle&&) */
    NavigationWorldLifecycle::NavigationWorldLifecycle(NavigationWorldLifecycle &&other) noexcept
        : maximumRetiredWorlds_(other.maximumRetiredWorlds_), staged_(std::move(other.staged_)), active_(std::move(other.active_)),
          retired_(std::move(other.retired_)), lastStagedTopology_(other.lastStagedTopology_), state_(other.state_) {
        other.maximumRetiredWorlds_ = 0;
        other.state_ = NavigationWorldLifecycleState::Closed;
    }

    /** @copydoc NavigationWorldLifecycle::~NavigationWorldLifecycle */
    NavigationWorldLifecycle::~NavigationWorldLifecycle() {
        BeginShutdown();
    }

    /** @copydoc NavigationWorldLifecycle::Create */
    Result<NavigationWorldLifecycle> NavigationWorldLifecycle::Create(const std::uint32_t maximumRetiredWorlds) {
        if (maximumRetiredWorlds == 0 || maximumRetiredWorlds > MaximumRetiredWorlds)
            return Failure<NavigationWorldLifecycle>(NavigationErrors::CapabilityDescriptorInvalid);
        try {
            std::vector<std::shared_ptr<Detail::NavigationWorldRecord>> retired;
            retired.reserve(maximumRetiredWorlds);
            return Result<NavigationWorldLifecycle>::Success(NavigationWorldLifecycle{maximumRetiredWorlds, std::move(retired)});
        } catch (const std::bad_alloc &) {
            return Failure<NavigationWorldLifecycle>(NavigationErrors::CapacityExceeded);
        }
    }

    /** @copydoc NavigationWorldLifecycle::Stage */
    Result<void> NavigationWorldLifecycle::Stage(const NavigationWorldActivationDescriptor &descriptor,
                                                 std::unique_ptr<INavigationQueryBackend> backend) {
        return StageCandidate(descriptor, std::move(backend), false);
    }

    /** @copydoc NavigationWorldLifecycle::StageCandidate */
    Result<void> NavigationWorldLifecycle::StageCandidate(const NavigationWorldActivationDescriptor &descriptor,
                                                          std::unique_ptr<INavigationQueryBackend> backend, bool emptyAllowed) {
        using enum NavigationWorldLifecycleState;
        if (state_ == ShuttingDown || state_ == Closed || staged_)
            return Failure<void>(NavigationErrors::CapabilityUnavailable);
        if (!descriptor.IsValid() || (backend ? !ValidateNavigationProviderCapabilities(backend->Capabilities()) : !emptyAllowed))
            return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
        const bool replacesTopology = active_ && active_->descriptor.world == descriptor.world;
        if (replacesTopology) {
            if (const auto valid = ValidateTopologyReplacement(active_->descriptor, descriptor); valid.HasError())
                return valid;
            if (descriptor.topology <= lastStagedTopology_)
                return Failure<void>(NavigationErrors::StaleSnapshot);
        } else if (ContainsWorld(retired_, descriptor.world)) {
            return Failure<void>(NavigationErrors::InvalidWorld);
        }
        try {
            staged_ = std::make_shared<Detail::NavigationWorldRecord>(descriptor, std::move(backend));
            if (replacesTopology)
                lastStagedTopology_ = descriptor.topology;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure<void>(NavigationErrors::CapacityExceeded);
        }
    }

    /** @copydoc NavigationWorldLifecycle::StageTopology */
    Result<void> NavigationWorldLifecycle::StageTopology(const NavigationWorldActivationDescriptor &expectedActive,
                                                         NavigationGeneration nextTopology,
                                                         std::unique_ptr<INavigationQueryBackend> backend) {
        if (!IsCurrentTopology(expectedActive))
            return Failure<void>(NavigationErrors::StaleSnapshot);
        auto candidate = expectedActive;
        candidate.topology = nextTopology;
        return Stage(candidate, std::move(backend));
    }

    /** @copydoc NavigationWorldLifecycle::StageEmptyTopology */
    Result<void> NavigationWorldLifecycle::StageEmptyTopology(const NavigationWorldActivationDescriptor &expectedActive,
                                                              NavigationGeneration nextTopology) {
        if (!IsCurrentTopology(expectedActive))
            return Failure<void>(NavigationErrors::StaleSnapshot);
        auto candidate = expectedActive;
        candidate.topology = nextTopology;
        return StageCandidate(candidate, {}, true);
    }

    /** @copydoc NavigationWorldLifecycle::StageInitialEmptyTopology */
    Result<void> NavigationWorldLifecycle::StageInitialEmptyTopology(const NavigationWorldActivationDescriptor &descriptor) {
        if (state_ != NavigationWorldLifecycleState::Empty)
            return Failure<void>(NavigationErrors::StaleSnapshot);
        return StageCandidate(descriptor, {}, true);
    }

    /** @copydoc NavigationWorldLifecycle::IsCurrentTopology */
    bool NavigationWorldLifecycle::IsCurrentTopology(const NavigationWorldActivationDescriptor &expected) const noexcept {
        return active_ && active_->descriptor == expected && active_->revocation.load() == NavigationWorldRevocation::None;
    }

    /** @copydoc NavigationWorldLifecycle::IsStagedTopology */
    bool NavigationWorldLifecycle::IsStagedTopology(const NavigationWorldActivationDescriptor &candidate) const noexcept {
        return active_ && staged_ && staged_->descriptor == candidate && candidate.world == active_->descriptor.world;
    }

    /** @copydoc NavigationWorldLifecycle::DiscardStagedTopology */
    Result<void> NavigationWorldLifecycle::DiscardStagedTopology(const NavigationWorldActivationDescriptor &candidate) {
        return TryDiscardStagedTopology(candidate) ? Result<void>::Success() : Failure<void>(NavigationErrors::StaleSnapshot);
    }

    /** @copydoc NavigationWorldLifecycle::TryDiscardStagedTopology */
    bool NavigationWorldLifecycle::TryDiscardStagedTopology(const NavigationWorldActivationDescriptor &candidate) noexcept {
        if (!IsStagedTopology(candidate))
            return false;
        Revoke(staged_, NavigationWorldRevocation::WorldRetired);
        staged_.reset();
        return true;
    }

    /** @copydoc NavigationWorldLifecycle::CommitStagedTopology */
    Result<void> NavigationWorldLifecycle::CommitStagedTopology(const NavigationWorldActivationDescriptor &candidate) {
        if (auto valid = ValidateStagedTopology(candidate); valid.HasError())
            return valid;
        PublishStaged();
        return Result<void>::Success();
    }

    /** @copydoc NavigationWorldLifecycle::ValidateStagedTopology */
    Result<void> NavigationWorldLifecycle::ValidateStagedTopology(const NavigationWorldActivationDescriptor &candidate) const {
        if (!IsStagedTopology(candidate))
            return Failure<void>(NavigationErrors::StaleSnapshot);
        if (!CanRetireActive())
            return Failure<void>(NavigationErrors::CapacityExceeded);
        return Result<void>::Success();
    }

    /** @copydoc NavigationWorldLifecycle::PublishValidatedTopology */
    void NavigationWorldLifecycle::PublishValidatedTopology(const NavigationWorldActivationDescriptor &candidate,
                                                            NavigationTopologyChange change) noexcept {
        assert(IsStagedTopology(candidate) && CanRetireActive());
        (void)candidate;
        const auto reason = change == NavigationTopologyChange::ForcedCellEviction ? NavigationWorldRevocation::TileEvicted
                                                                                   : NavigationWorldRevocation::TopologyReplaced;
        PublishStaged(/*deferCancellation=*/true, reason);
    }

    /** @copydoc NavigationWorldLifecycle::NotifyRetired */
    void NavigationWorldLifecycle::NotifyRetired(const NavigationWorldReadLease &predecessor) noexcept {
        if (predecessor.IsRevoked())
            predecessor.record_->cancellation.RequestCancellation();
    }

    /** @copydoc NavigationWorldLifecycle::CommitAtSafePoint */
    Result<void> NavigationWorldLifecycle::CommitAtSafePoint(const NavigationSceneRuntimeId expectedScene,
                                                             const NavigationSceneGeneration expectedGeneration) {
        using enum NavigationWorldLifecycleState;
        if (state_ == ShuttingDown || state_ == Closed || !staged_)
            return Failure<void>(NavigationErrors::CapabilityUnavailable);
        if (staged_->descriptor.scene != expectedScene || staged_->descriptor.sceneGeneration != expectedGeneration)
            return Failure<void>(NavigationErrors::StaleSnapshot);
        if (!CanRetireActive())
            return Failure<void>(NavigationErrors::CapacityExceeded);

        PublishStaged();
        return Result<void>::Success();
    }

    /** @copydoc NavigationWorldLifecycle::PublishStaged */
    void NavigationWorldLifecycle::PublishStaged(bool deferCancellation, NavigationWorldRevocation topologyReason) noexcept {
        using enum NavigationWorldLifecycleState;
        const bool replacesTopology = active_ && active_->descriptor.world == staged_->descriptor.world;
        const bool keepPaused = state_ == Paused && replacesTopology;
        RetireActive(replacesTopology ? topologyReason : NavigationWorldRevocation::WorldRetired, deferCancellation);
        active_ = std::move(staged_);
        lastStagedTopology_ = active_->descriptor.topology;
        state_ = keepPaused ? Paused : Active;
    }

    /** @copydoc NavigationWorldLifecycle::Pause */
    Result<void> NavigationWorldLifecycle::Pause(const NavigationWorldId world) noexcept {
        using enum NavigationWorldLifecycleState;
        if (!active_ || active_->descriptor.world != world)
            return Failure<void>(NavigationErrors::InvalidWorld);
        if (state_ != Active && state_ != Paused)
            return Failure<void>(NavigationErrors::CapabilityUnavailable);
        state_ = Paused;
        return Result<void>::Success();
    }

    /** @copydoc NavigationWorldLifecycle::Resume */
    Result<void> NavigationWorldLifecycle::Resume(const NavigationWorldId world) noexcept {
        using enum NavigationWorldLifecycleState;
        if (!active_ || active_->descriptor.world != world)
            return Failure<void>(NavigationErrors::InvalidWorld);
        if (state_ != Paused)
            return Failure<void>(NavigationErrors::CapabilityUnavailable);
        state_ = Active;
        return Result<void>::Success();
    }

    /** @copydoc NavigationWorldLifecycle::Unload */
    Result<void> NavigationWorldLifecycle::Unload(const NavigationWorldId world) noexcept {
        using enum NavigationWorldLifecycleState;
        if (!active_ || active_->descriptor.world != world)
            return Failure<void>(NavigationErrors::InvalidWorld);
        if (state_ == ShuttingDown || state_ == Closed)
            return Failure<void>(NavigationErrors::CapabilityUnavailable);
        if (!CanRetireActive())
            return Failure<void>(NavigationErrors::CapacityExceeded);
        if (staged_) {
            Revoke(staged_, NavigationWorldRevocation::WorldRetired);
            staged_.reset();
        }
        RetireActive(NavigationWorldRevocation::WorldRetired);
        lastStagedTopology_ = {};
        state_ = Empty;
        return Result<void>::Success();
    }

    /** @copydoc NavigationWorldLifecycle::BeginShutdown */
    void NavigationWorldLifecycle::BeginShutdown() noexcept {
        using enum NavigationWorldLifecycleState;
        if (state_ == Closed)
            return;
        state_ = ShuttingDown;
        if (staged_) {
            Revoke(staged_, NavigationWorldRevocation::WorldRetired);
            staged_.reset();
        }
        if (active_)
            Revoke(active_, NavigationWorldRevocation::WorldRetired);
        for (const auto &record : retired_)
            Revoke(record, NavigationWorldRevocation::WorldRetired);
        static_cast<void>(CollectRetired());
    }

    /** @copydoc NavigationWorldLifecycle::CollectRetired */
    NavigationWorldLifecycleState NavigationWorldLifecycle::CollectRetired() noexcept {
        using enum NavigationWorldLifecycleState;
        std::erase_if(retired_, [](const auto &record) {
            return record.use_count() == 1;
        });
        if (state_ == ShuttingDown && active_ && active_.use_count() == 1)
            active_.reset();
        if (state_ == ShuttingDown && !active_ && !staged_ && retired_.empty())
            state_ = Closed;
        return state_;
    }

    /** @copydoc NavigationWorldLifecycle::Acquire */
    Result<NavigationWorldReadLease> NavigationWorldLifecycle::Acquire(const NavigationWorldId world) const {
        if (!active_)
            return Failure<NavigationWorldReadLease>(NavigationErrors::NoNavigationData);
        if (active_->descriptor.world != world)
            return Failure<NavigationWorldReadLease>(NavigationErrors::InvalidWorld);
        if (state_ != NavigationWorldLifecycleState::Active || active_->revocation.load() != NavigationWorldRevocation::None)
            return Failure<NavigationWorldReadLease>(NavigationErrors::CapabilityUnavailable);
        if (!active_->backend)
            return Failure<NavigationWorldReadLease>(NavigationErrors::NoNavigationData);
        return Result<NavigationWorldReadLease>::Success(NavigationWorldReadLease{active_});
    }

    /** @copydoc NavigationWorldLifecycle::ActiveDescriptor */
    Result<NavigationWorldActivationDescriptor> NavigationWorldLifecycle::ActiveDescriptor() const {
        if (!active_)
            return Failure<NavigationWorldActivationDescriptor>(NavigationErrors::NoNavigationData);
        return Result<NavigationWorldActivationDescriptor>::Success(active_->descriptor);
    }

    /** @copydoc NavigationWorldLifecycle::State */
    NavigationWorldLifecycleState NavigationWorldLifecycle::State() const noexcept {
        return state_;
    }

    /** @copydoc NavigationWorldLifecycle::RetiredCount */
    std::size_t NavigationWorldLifecycle::RetiredCount() const noexcept {
        return retired_.size();
    }

    /** @copydoc NavigationWorldLifecycle::HasStagedCandidate */
    bool NavigationWorldLifecycle::HasStagedCandidate() const noexcept {
        return static_cast<bool>(staged_);
    }

    /** @copydoc NavigationWorldLifecycle::CanRetireActive */
    bool NavigationWorldLifecycle::CanRetireActive() const noexcept {
        return !active_ || active_.use_count() == 1 || retired_.size() < maximumRetiredWorlds_;
    }

    /** @copydoc NavigationWorldLifecycle::Revoke */
    void NavigationWorldLifecycle::Revoke(const std::shared_ptr<Detail::NavigationWorldRecord> &record,
                                          NavigationWorldRevocation reason) noexcept {
        auto current = NavigationWorldRevocation::None;
        if (record->revocation.compare_exchange_strong(current, reason))
            record->cancellation.RequestCancellation();
    }

    /** @copydoc NavigationWorldLifecycle::RetireActive */
    void NavigationWorldLifecycle::RetireActive(NavigationWorldRevocation reason, bool deferCancellation) noexcept {
        if (!active_)
            return;
        if (deferCancellation) {
            auto current = NavigationWorldRevocation::None;
            active_->revocation.compare_exchange_strong(current, reason);
        } else {
            Revoke(active_, reason);
        }
        if (active_.use_count() > 1)
            retired_.push_back(active_);
        active_.reset();
    }
}  // namespace Horo::Navigation
