#include "Horo/Navigation/NavigationWorldLifecycle.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <atomic>
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
            std::atomic<bool> revoked{false};
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
        return record_->revoked.load();
    }

    /** @copydoc NavigationWorldLifecycle::NavigationWorldLifecycle(std::uint32_t,
     * std::vector<std::shared_ptr<Detail::NavigationWorldRecord>>) */
    NavigationWorldLifecycle::NavigationWorldLifecycle(const std::uint32_t maximumRetiredWorlds,
                                                       std::vector<std::shared_ptr<Detail::NavigationWorldRecord>> retired) noexcept
        : maximumRetiredWorlds_(maximumRetiredWorlds), retired_(std::move(retired)) {}

    /** @copydoc NavigationWorldLifecycle::NavigationWorldLifecycle(NavigationWorldLifecycle&&) */
    NavigationWorldLifecycle::NavigationWorldLifecycle(NavigationWorldLifecycle &&other) noexcept
        : maximumRetiredWorlds_(other.maximumRetiredWorlds_), staged_(std::move(other.staged_)), active_(std::move(other.active_)),
          retired_(std::move(other.retired_)), state_(other.state_) {
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
        using enum NavigationWorldLifecycleState;
        if (state_ == ShuttingDown || state_ == Closed || staged_)
            return Failure<void>(NavigationErrors::CapabilityUnavailable);
        if (!descriptor.IsValid() || !backend || !ValidateNavigationProviderCapabilities(backend->Capabilities()))
            return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
        if (active_ && active_->descriptor.world == descriptor.world) {
            if (const auto valid = ValidateTopologyReplacement(active_->descriptor, descriptor); valid.HasError())
                return valid;
        } else if (ContainsWorld(retired_, descriptor.world)) {
            return Failure<void>(NavigationErrors::InvalidWorld);
        }
        try {
            staged_ = std::make_shared<Detail::NavigationWorldRecord>(descriptor, std::move(backend));
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure<void>(NavigationErrors::CapacityExceeded);
        }
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

        const bool keepPaused = state_ == Paused && active_ && active_->descriptor.world == staged_->descriptor.world;
        RetireActive();
        active_ = std::move(staged_);
        state_ = keepPaused ? Paused : Active;
        return Result<void>::Success();
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
            Revoke(staged_);
            staged_.reset();
        }
        RetireActive();
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
            Revoke(staged_);
            staged_.reset();
        }
        if (active_)
            Revoke(active_);
        for (const auto &record : retired_)
            Revoke(record);
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
        if (state_ != NavigationWorldLifecycleState::Active || active_->revoked.load())
            return Failure<NavigationWorldReadLease>(NavigationErrors::CapabilityUnavailable);
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
    void NavigationWorldLifecycle::Revoke(const std::shared_ptr<Detail::NavigationWorldRecord> &record) noexcept {
        record->revoked.store(true);
        record->cancellation.RequestCancellation();
    }

    /** @copydoc NavigationWorldLifecycle::RetireActive */
    void NavigationWorldLifecycle::RetireActive() noexcept {
        if (!active_)
            return;
        Revoke(active_);
        if (active_.use_count() > 1)
            retired_.push_back(active_);
        active_.reset();
    }
}  // namespace Horo::Navigation
