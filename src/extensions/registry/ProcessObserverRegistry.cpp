#include "Horo/Extensions/ProcessObserverRegistry.h"

#include "../ExtensionAuthorityIdentityValidation.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <mutex>
#include <ranges>
#include <thread>
#include <type_traits>
#include <utility>

namespace Horo::Extensions {
    struct ProcessObserverEntry final {
        ProcessObserverDescriptor descriptor;
        ExtensionCapabilityHandle authority;
        // Destroy the callback before the lease that keeps its executable code loaded.
        std::shared_ptr<void> codeLease;
        ProcessObserverCallback callback;
        bool registered{true};  // Protected by ProcessObserverRegistryState::mutex.
    };

    struct ProcessObserverRegistryState final {
        explicit ProcessObserverRegistryState(std::vector<ProcessObserverEventKind> allowed) : hostAllowedEvents(std::move(allowed)) {}

        std::mutex mutex;
        const std::thread::id ownerThread{std::this_thread::get_id()};
        const std::vector<ProcessObserverEventKind> hostAllowedEvents;
        std::vector<std::shared_ptr<ProcessObserverEntry>> entries;
        bool shutdown{};
        bool dispatching{};
    };

    namespace {
        [[nodiscard]] bool Known(const ProcessObserverEventKind kind) noexcept {
            return kind >= ProcessObserverEventKind::HostStarted && kind <= ProcessObserverEventKind::DiagnosticRaised;
        }

        [[nodiscard]] bool ValidKinds(const std::vector<ProcessObserverEventKind> &kinds, const bool requireNonempty) {
            if (kinds.size() > ProcessObserverRegistry::MaximumEventKinds || (requireNonempty && kinds.empty()))
                return false;
            for (std::size_t i = 0; i < kinds.size(); ++i) {
                if (!Known(kinds[i]) || std::ranges::find(kinds.begin(), kinds.begin() + static_cast<std::ptrdiff_t>(i), kinds[i]) !=
                                            kinds.begin() + static_cast<std::ptrdiff_t>(i))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidEvent(const ProcessObserverEvent &event) noexcept {
            if (!Known(event.kind) || event.revision == 0)
                return false;
            using enum ProcessObserverEventKind;
            switch (event.kind) {
                case HostStarted:
                case HostStopping:
                    return event.operationId == 0 && event.outcome == ProcessObserverOutcome::None &&
                           event.diagnostic == ProcessObserverDiagnostic::None;
                case OperationStarted:
                    return event.operationId != 0 && event.outcome == ProcessObserverOutcome::None &&
                           event.diagnostic == ProcessObserverDiagnostic::None;
                case OperationFinished:
                    return event.operationId != 0 &&
                           (event.outcome == ProcessObserverOutcome::Succeeded || event.outcome == ProcessObserverOutcome::Failed ||
                            event.outcome == ProcessObserverOutcome::Cancelled) &&
                           event.diagnostic == ProcessObserverDiagnostic::None;
                case DiagnosticRaised:
                    return event.outcome == ProcessObserverOutcome::None &&
                           (event.diagnostic == ProcessObserverDiagnostic::OperationRejected ||
                            event.diagnostic == ProcessObserverDiagnostic::OperationTimedOut ||
                            event.diagnostic == ProcessObserverDiagnostic::ProviderFailed);
            }
            return false;
        }

        void Remove(const std::shared_ptr<ProcessObserverRegistryState> &state,
                    const std::shared_ptr<ProcessObserverEntry> &entry) noexcept {
            std::scoped_lock lock{state->mutex};
            entry->registered = false;
            std::erase_if(state->entries, [&entry](const auto &candidate) {
                return candidate == entry;
            });
        }

        /** @brief Clears the dispatching flag on every exit from a completed dispatch. */
        class DispatchGuard final {
        public:
            explicit DispatchGuard(std::shared_ptr<ProcessObserverRegistryState> state) : state_(std::move(state)) {}

            ~DispatchGuard() {
                std::scoped_lock lock{state_->mutex};
                state_->dispatching = false;
            }

            DispatchGuard(const DispatchGuard &) = delete;
            DispatchGuard &operator=(const DispatchGuard &) = delete;
            DispatchGuard(DispatchGuard &&) = delete;
            DispatchGuard &operator=(DispatchGuard &&) = delete;

        private:
            std::shared_ptr<ProcessObserverRegistryState> state_;
        };

        static_assert(!std::is_copy_constructible_v<DispatchGuard> && !std::is_copy_assignable_v<DispatchGuard>);
        static_assert(!std::is_move_constructible_v<DispatchGuard> && !std::is_move_assignable_v<DispatchGuard>);

        [[nodiscard]] bool DispatchToCandidate(const std::shared_ptr<ProcessObserverRegistryState> &state,
                                               const std::shared_ptr<ProcessObserverEntry> &entry, const ProcessObserverEvent &event) {
            Result<ExtensionCapabilityUseLease> use = [&]() {
                std::scoped_lock lock{state->mutex};
                if (state->shutdown || !entry->registered ||
                    std::ranges::find(entry->descriptor.allowedEvents, event.kind) == entry->descriptor.allowedEvents.end())
                    return Result<ExtensionCapabilityUseLease>::Failure(MakeError(ExtensionErrors::CapabilityRevoked));
                return entry->authority.AcquireUse(entry->descriptor.extensionId, entry->descriptor.moduleId,
                                                   entry->descriptor.activationGeneration);
            }();
            if (!use.HasValue())  // NOSONAR(cpp:S6004) The use lease must outlive the callback below.
                return false;
            bool failed = false;
            try {
                failed = !entry->callback(event).HasValue();
            } catch (...) {  // NOSONAR(cpp:S2738) Provider callback may throw an arbitrary type.
                failed = true;
            }
            if (failed)
                Remove(state, entry);
            return failed;
        }
    }  // namespace

    ProcessObserverRegistration::ProcessObserverRegistration(std::weak_ptr<ProcessObserverRegistryState> registry,
                                                             std::shared_ptr<ProcessObserverEntry> entry) noexcept
        : registry_(std::move(registry)), entry_(std::move(entry)) {}

    ProcessObserverRegistration::~ProcessObserverRegistration() noexcept {
        Reset();
    }

    ProcessObserverRegistration::ProcessObserverRegistration(ProcessObserverRegistration &&other) noexcept
        : registry_(std::move(other.registry_)), entry_(std::move(other.entry_)) {}

    ProcessObserverRegistration &ProcessObserverRegistration::operator=(ProcessObserverRegistration &&other) noexcept {
        if (this != &other) {
            Reset();
            registry_ = std::move(other.registry_);
            entry_ = std::move(other.entry_);
        }
        return *this;
    }

    /** @copydoc ProcessObserverRegistration::Reset */
    void ProcessObserverRegistration::Reset() noexcept {
        if (entry_ == nullptr)
            return;
        if (const auto state = registry_.lock())
            Remove(state, entry_);
        entry_.reset();
        registry_.reset();
    }

    /** @copydoc ProcessObserverRegistration::IsRegistered */
    bool ProcessObserverRegistration::IsRegistered() const noexcept {
        if (entry_ == nullptr)
            return false;
        const auto state = registry_.lock();
        if (state == nullptr)
            return false;
        std::scoped_lock lock{state->mutex};
        return entry_->registered && !state->shutdown;
    }

    /** @copydoc ProcessObserverRegistry::ProcessObserverRegistry */
    ProcessObserverRegistry::ProcessObserverRegistry(std::vector<ProcessObserverEventKind> hostAllowedEvents)
        : state_(std::make_shared<ProcessObserverRegistryState>(std::move(hostAllowedEvents))) {}

    ProcessObserverRegistry::~ProcessObserverRegistry() noexcept {
        BeginShutdown();
    }

    /** @copydoc ProcessObserverRegistry::Register */
    Result<ProcessObserverRegistration> ProcessObserverRegistry::Register(  // NOSONAR(cpp:S5817) Publication mutates owner state.
        ProcessObserverDescriptor descriptor, ExtensionCapabilityHandle authority, ProcessObserverCallback callback,
        std::shared_ptr<void> codeLease) {
        if (!ValidKinds(state_->hostAllowedEvents, true) || !ValidKinds(descriptor.allowedEvents, true) ||
            !Detail::IsCanonicalExtensionAuthorityId(descriptor.observerId) ||
            !Detail::IsCanonicalExtensionAuthorityId(descriptor.extensionId) ||
            !Detail::IsCanonicalExtensionAuthorityId(descriptor.moduleId) || descriptor.activationGeneration == 0 || !callback ||
            codeLease == nullptr || std::ranges::any_of(descriptor.allowedEvents, [this](const ProcessObserverEventKind kind) {
            return std::ranges::find(state_->hostAllowedEvents, kind) == state_->hostAllowedEvents.end();
        })) {
            return Result<ProcessObserverRegistration>::Failure(MakeError(ExtensionErrors::ProcessObserverInvalid));
        }
        if (authority.Capability().value != "horo.process.observe")
            return Result<ProcessObserverRegistration>::Failure(MakeError(ExtensionErrors::PermissionDenied));
        auto use = authority.AcquireUse(descriptor.extensionId, descriptor.moduleId, descriptor.activationGeneration);
        if (!use.HasValue())  // NOSONAR(cpp:S6004) Keep admission leased through publication.
            return Result<ProcessObserverRegistration>::Failure(use.ErrorValue());

        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return Result<ProcessObserverRegistration>::Failure(MakeError(ExtensionErrors::ProcessObserverShutdown));
        if (std::ranges::any_of(state_->entries, [&descriptor](const auto &entry) {
            return entry->descriptor.observerId == descriptor.observerId;
        }))
            return Result<ProcessObserverRegistration>::Failure(MakeError(ExtensionErrors::ProcessObserverDuplicate));
        if (state_->entries.size() == MaximumObservers)
            return Result<ProcessObserverRegistration>::Failure(MakeError(ExtensionErrors::ProcessObserverCapacityExceeded));

        auto entry =
            std::make_shared<ProcessObserverEntry>(std::move(descriptor), std::move(authority), std::move(codeLease), std::move(callback));
        state_->entries.push_back(entry);
        std::ranges::sort(state_->entries, {}, [](const auto &candidate) -> const std::string & {
            return candidate->descriptor.observerId;
        });
        return Result<ProcessObserverRegistration>::Success(ProcessObserverRegistration{state_, std::move(entry)});
    }

    /** @copydoc ProcessObserverRegistry::Dispatch */
    Result<void> ProcessObserverRegistry::Dispatch(  // NOSONAR(cpp:S5817) Dispatch mutates owner lifecycle state.
        const ProcessObserverEvent &event) {
        if (std::this_thread::get_id() != state_->ownerThread)
            return Result<void>::Failure(MakeError(ExtensionErrors::ProcessObserverThreadViolation));
        if (!ValidKinds(state_->hostAllowedEvents, true) || !ValidEvent(event) ||
            std::ranges::find(state_->hostAllowedEvents, event.kind) == state_->hostAllowedEvents.end())
            return Result<void>::Failure(MakeError(ExtensionErrors::ProcessObserverInvalid));

        std::vector<std::shared_ptr<ProcessObserverEntry>> candidates;
        {
            std::scoped_lock lock{state_->mutex};
            if (state_->shutdown)
                return Result<void>::Failure(MakeError(ExtensionErrors::ProcessObserverShutdown));
            if (state_->dispatching)
                return Result<void>::Failure(MakeError(ExtensionErrors::ProcessObserverReentrant));
            candidates = state_->entries;
            state_->dispatching = true;
        }

        DispatchGuard guard{state_};

        std::string failedObserver;
        for (const auto &entry : candidates) {
            if (DispatchToCandidate(state_, entry, event) && failedObserver.empty())
                failedObserver = entry->descriptor.observerId;
        }
        if (!failedObserver.empty())
            return Result<void>::Failure(
                MakeError(ExtensionErrors::ProcessObserverCallbackFailed, "Process observer failed: " + failedObserver));
        return Result<void>::Success();
    }

    /** @copydoc ProcessObserverRegistry::BeginShutdown */
    void ProcessObserverRegistry::BeginShutdown() noexcept {  // NOSONAR(cpp:S5817) Shutdown mutates owner lifecycle state.
        std::scoped_lock lock{state_->mutex};
        state_->shutdown = true;
        for (const auto &entry : state_->entries)
            entry->registered = false;
        state_->entries.clear();
    }

    /** @copydoc ProcessObserverRegistry::IsShutdown */
    bool ProcessObserverRegistry::IsShutdown() const noexcept {
        std::scoped_lock lock{state_->mutex};
        return state_->shutdown;
    }
}  // namespace Horo::Extensions
