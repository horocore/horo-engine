#pragma once
/** @file NavigationBakeWorkspace.h
 * @brief Explicit project bake capabilities; surfaces never own workers or publication.
 */
#include "Horo/Application/NavigationBakeApplication.h"
#include "Horo/Editor/NavigationViewportSnapshot.h"
#include "Horo/Navigation/NavigationAgentProfiles.h"

namespace Horo::Editor {
    /** @brief Immutable authored choices from the host's background project capture. */
    struct NavigationBakeCatalog final {
        Assets::AssetId definition;
        std::vector<Navigation::NavigationAgentProfileDescriptor> profiles;
        std::vector<Navigation::NavigationBakeSurfaceKey> surfaces;
        Navigation::NavigationProjectProfileInput projectProfile;
    };

    /** @brief Exact application opening and its command identity; reloading a project cannot retarget stale decisions. */
    struct NavigationBakeOperationIdentity final {
        std::uint64_t opening{};
        std::uint64_t command{};
        [[nodiscard]] bool operator==(const NavigationBakeOperationIdentity &) const noexcept = default;
    };

    /** @brief One host-owned view; missing catalog denotes asynchronous preparation. */
    struct NavigationBakeWorkspaceSnapshot final {
        std::uint64_t opening{};
        std::shared_ptr<const NavigationViewportSnapshot> overlays; /**< Owned saved/cooked evidence; never provider memory. */
        std::shared_ptr<const NavigationBakeCatalog> catalog;
        std::optional<Application::NavigationBakeCommandSnapshot> operation;
        std::optional<Error> diagnostic;
        std::chrono::milliseconds elapsed{};
        Navigation::NavigationBakeJobBudget budget;
    };

    /** @brief Owner-lane application capabilities, retained independently of panel/modal lifetime.
     * @details Prepare queues bounded I/O, Query copies memory only, and Start always captures fresh source.
     * The executable pumps and drains the owner even when every navigation surface is closed.
     * Retry names the exact terminal identity and preserves its execution/surface intent, never its capture.
     */
    struct NavigationBakeWorkspaceAccess final {
        std::function<Result<void>(const std::filesystem::path &)> prepare;
        std::function<std::shared_ptr<const NavigationViewportSnapshot>(const std::filesystem::path &)>
            overlays; /**< Memory-only geometry query; no operation or provider leases. */
        std::function<Result<void>(const std::filesystem::path &)> reload;
        std::function<Result<NavigationBakeWorkspaceSnapshot>(const std::filesystem::path &)> query;
        std::function<Result<NavigationBakeOperationIdentity>(const std::filesystem::path &, Application::NavigationBakeCommandRequest)>
            start;
        std::function<Result<NavigationBakeOperationIdentity>(const std::filesystem::path &, NavigationBakeOperationIdentity)> retry;
        std::function<Result<void>(const std::filesystem::path &, NavigationBakeOperationIdentity)> cancel;
    };

    /** @brief Exact-operation cancellation decision; dismissal and destruction have no domain side effects. */
    class NavigationBakeCancelDecision final {
    public:
        /** @brief Owns the narrow cancellation capability and exact opening identity.
         * @param cancel Explicit owner-lane cancellation action. @param id Captured operation identity.
         */
        NavigationBakeCancelDecision(std::function<Result<void>(NavigationBakeOperationIdentity)> cancel,
                                     NavigationBakeOperationIdentity id)
            : cancel_(std::move(cancel)), id_(id) {}

        /** @brief Requests cancellation only after explicit confirmation; errors allow a deliberate retry.
         * @return Original operation result. Repeated successful confirmation is idempotent.
         */
        [[nodiscard]] Result<void> Confirm();

    private:
        std::function<Result<void>(NavigationBakeOperationIdentity)> cancel_;
        NavigationBakeOperationIdentity id_;
        bool confirmed_{};
    };
}  // namespace Horo::Editor
