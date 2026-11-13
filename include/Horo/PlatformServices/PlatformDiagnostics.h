#pragma once

/** @file PlatformDiagnostics.h
 * @brief Owner-lane privacy-safe diagnostics and product-gated offline debug commands.
 */

#include "Horo/PlatformServices/PlatformOfflineQueue.h"
#include "Horo/PlatformServices/PlatformServicesComposition.h"

#include <functional>
#include <thread>

namespace Horo::PlatformServices {
    /** @brief Immutable aggregate publication; deliberately excludes account, intent, payload and native identity. */
    struct PlatformDiagnosticsSnapshot final {
        PlatformServicesCompositionDiagnostics composition;
        PlatformOfflineQueueDiagnostics offline;
        PlatformOfflineQueueGeneration queueGeneration;
        bool canSuspendPending{};
    };

    /** @brief Read/control capability injected by host composition; never owns a provider or offline state. */
    class IPlatformDiagnostics {
    public:
        virtual ~IPlatformDiagnostics() = default;
        /** @brief Copies coherent bounded evidence. @return Snapshot or typed unavailable/wrong-lane failure. */
        [[nodiscard]] virtual Result<PlatformDiagnosticsSnapshot> Query() const = 0;
        /** @brief Requests suspension against the displayed generations; never forces replay or bypasses access.
         * @param provider Expected activation. @param queue Expected offline lifetime.
         * @return Count after the host owner has checkpointed suspension, or typed denial/stale/storage failure. */
        [[nodiscard]] virtual Result<std::size_t> SuspendPending(PlatformProviderGeneration provider,
                                                                 PlatformOfflineQueueGeneration queue) = 0;
    };

    /**
     * @brief Host-owned owner-lane adapter over borrowed composition and queue evidence.
     * @details All methods run on the constructing lane. Borrowed owners and the command target outlive this adapter;
     * the UI releases it on detach. The command is injected by the durable owner and must commit before returning success.
     * No callback is invoked for wrong-lane, stale, Null, closed, unsupported or shipping requests. It is not a scheduler.
     */
    class PlatformDiagnostics final : public IPlatformDiagnostics {
    public:
        /** @brief Composes a presenter capability without querying a provider.
         * @param composition Live host-owned product authority.
         * @param queue Read-only policy evidence; no mutable service state escapes.
         * @param suspend Optional durable-owner command; absent means read-only in every profile. */
        PlatformDiagnostics(const PlatformServicesComposition &composition, const PlatformOfflineQueue &queue,
                            std::function<Result<std::size_t>()> suspend = {});
        /** @copydoc IPlatformDiagnostics::Query */
        [[nodiscard]] Result<PlatformDiagnosticsSnapshot> Query() const override;
        /** @copydoc IPlatformDiagnostics::SuspendPending */
        [[nodiscard]] Result<std::size_t> SuspendPending(PlatformProviderGeneration provider,
                                                         PlatformOfflineQueueGeneration queue) override;

    private:
        const PlatformServicesComposition &composition_;
        const PlatformOfflineQueue &queue_;
        std::function<Result<std::size_t>()> suspend_;
        std::thread::id owner_;
    };
}  // namespace Horo::PlatformServices
