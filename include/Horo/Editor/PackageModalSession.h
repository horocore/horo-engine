#pragma once
/** @file PackageModalSession.h
 * @brief Headless-capable package modal projection over the shared application authority.
 */
#include "Horo/Application/PackageApplication.h"

namespace Horo::Editor {
    /** @brief One modal opening; stale controls cannot address a later opening. */
    struct PackageModalGeneration final {
        std::uint64_t value{};
        [[nodiscard]] auto operator<=>(const PackageModalGeneration &) const = default;
    };

    /** @brief Presentation state only; the application service retains every operation through cancellation. */
    class PackageModalSession final {
    public:
        /** @brief Retains the injected project capability without selecting policy or discovering services.
         * @param application Exact host-composed project service, already bound to its project generation. */
        explicit PackageModalSession(std::shared_ptr<Application::PackageApplication> application);
        /** @brief Opens an inert view and reconnects to any retained operation.
         * @return New view generation or typed unavailable/exhausted failure. */
        [[nodiscard]] Result<PackageModalGeneration> Open();
        /** @brief Refreshes immutable evidence without pumping or mutating package authorities.
         * @param generation Expected opening. @return Success or typed stale/unavailable failure. */
        [[nodiscard]] Result<void> Refresh(PackageModalGeneration generation);
        /** @brief Starts restore/install/disable through the canonical service.
         * @param generation Expected opening. @param kind Explicit action; Activate requires the typed selection overload.
         * @return Exact operation identity or typed rejection. */
        [[nodiscard]] Result<Application::PackageOperationId> Submit(PackageModalGeneration generation,
                                                                     Application::PackageOperationKind kind);
        /** @brief Forwards explicit enablement without granting trust or deriving descriptor paths.
         * @param generation Expected opening. @param enabled Typed selection. @return Operation or typed failure. */
        [[nodiscard]] Result<Application::PackageOperationId> Activate(PackageModalGeneration generation,
                                                                       std::span<const Packages::EnabledPackageExtension> enabled);
        /** @brief Cancels the exact projected operation; does not destroy application work.
         * @param generation Expected opening. @return Typed outcome. */
        [[nodiscard]] Result<void> Cancel(PackageModalGeneration generation);
        /** @brief Invalidates the opening; package operations remain owned by the application.
         * @param generation Expected opening. @return Success or typed stale failure. */
        [[nodiscard]] Result<void> Close(PackageModalGeneration generation);
        /** @brief Returns the last immutable operation projection. @return Optional exact operation snapshot. */
        [[nodiscard]] const std::optional<Application::PackageOperationSnapshot> &Operation() const noexcept;
        /** @brief Returns the last immutable plan, absent when evidence is unavailable. @return Owning read-only evidence. */
        [[nodiscard]] std::shared_ptr<const Application::PackagePlanSnapshot> Plan() const noexcept;

    private:
        /** @brief Rejects controls from a closed or replaced modal opening before application access. */
        [[nodiscard]] Result<void> Validate(PackageModalGeneration generation) const;
        std::shared_ptr<Application::PackageApplication> application_;
        PackageModalGeneration generation_;
        bool open_{};
        std::optional<Application::PackageOperationSnapshot> operation_;
        std::shared_ptr<const Application::PackagePlanSnapshot> plan_;
    };
}  // namespace Horo::Editor
