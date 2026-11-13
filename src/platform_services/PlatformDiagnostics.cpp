#include "Horo/PlatformServices/PlatformDiagnostics.h"

#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"

#include <utility>

namespace Horo::PlatformServices {
    namespace {
        /** @brief Exhaustive developer election; unknown and shipping products remain denied. */
        [[nodiscard]] bool DeveloperProfile(const PlatformServicesProductProfile profile) noexcept {
            return profile == PlatformServicesProductProfile::Editor || profile == PlatformServicesProductProfile::DevelopmentGame ||
                   profile == PlatformServicesProductProfile::Test;
        }
    }  // namespace

    /** @copydoc PlatformDiagnostics::PlatformDiagnostics */
    PlatformDiagnostics::PlatformDiagnostics(const PlatformServicesComposition &composition, const PlatformOfflineQueue &queue,
                                             std::function<Result<std::size_t>()> suspend)
        : composition_(composition), queue_(queue), suspend_(std::move(suspend)), owner_(std::this_thread::get_id()) {}

    /** @copydoc PlatformDiagnostics::Query */
    Result<PlatformDiagnosticsSnapshot> PlatformDiagnostics::Query() const {
        if (std::this_thread::get_id() != owner_)
            return Result<PlatformDiagnosticsSnapshot>::Failure(MakeError(OfflineQueueErrors::QueueUnavailable));
        auto composition = composition_.Diagnostics();
        const auto offline = queue_.Diagnostics();
        const bool allowed = DeveloperProfile(composition.profile) &&
                             (composition.mode == PlatformServicesCompositionMode::Test ||
                              composition.mode == PlatformServicesCompositionMode::ExactProvider) &&
                             !offline.closed && static_cast<bool>(suspend_) && composition_.Frontend().HasValue();
        return Result<PlatformDiagnosticsSnapshot>::Success({.composition = std::move(composition),
                                                             .offline = offline,
                                                             .queueGeneration = queue_.Generation(),
                                                             .canSuspendPending = allowed});
    }

    /** @copydoc PlatformDiagnostics::SuspendPending */
    Result<std::size_t> PlatformDiagnostics::SuspendPending(const PlatformProviderGeneration provider,
                                                            const PlatformOfflineQueueGeneration queue) {
        const auto snapshot = Query();
        if (snapshot.HasError())
            return Result<std::size_t>::Failure(snapshot.ErrorValue());
        if (!snapshot.Value().canSuspendPending)
            return Result<std::size_t>::Failure(MakeError(FrontendErrors::OperationDenied));
        if (provider != snapshot.Value().composition.generation || queue != snapshot.Value().queueGeneration)
            return Result<std::size_t>::Failure(MakeError(OfflineQueueErrors::Stale));
        try {
            return suspend_();
        } catch (...) {  // NOSONAR: host command adapters are an extension boundary.
            return Result<std::size_t>::Failure(MakeError(OfflineQueueErrors::QueueUnavailable));
        }
    }
}  // namespace Horo::PlatformServices
