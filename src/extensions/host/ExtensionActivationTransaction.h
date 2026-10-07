#pragma once

#include "../capabilities/asset_pipeline_points/ExternalAssetImporter.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Foundation/ErrorCode.h"

#include <memory>
#include <span>
#include <vector>

namespace Horo::Extensions {
    /**
     * @brief Owns one package's staged contributions and activated module leases until atomic publication.
     * @details Rollback destroys contribution adapters in reverse registration order before unloading modules in reverse activation order.
     */
    class ExtensionActivationTransaction final {
    public:
        ExtensionActivationTransaction() = default;
        /** @brief Rolls back an unreleased staged transaction as a final safety net. */
        ~ExtensionActivationTransaction();
        ExtensionActivationTransaction(const ExtensionActivationTransaction &) = delete;
        ExtensionActivationTransaction &operator=(const ExtensionActivationTransaction &) = delete;
        ExtensionActivationTransaction(ExtensionActivationTransaction &&) = delete;
        ExtensionActivationTransaction &operator=(ExtensionActivationTransaction &&) = delete;

        /**
         * @brief Adds one successfully activated module and its validated candidate contributions.
         * @param lifetime Module lifetime retained through commit or rollback.
         * @param contributions Host-owned candidate contributions produced by that module.
         */
        void Stage(std::shared_ptr<ExtensionModuleLifetime> lifetime, std::vector<Assets::AssetImporterContribution> contributions);

        /** @brief Returns the complete package candidate without publishing it. */
        [[nodiscard]] std::vector<Assets::AssetImporterContribution> &Contributions() noexcept;

        /**
         * @brief Rolls back the activation and reports any contained unload failures on the primary error.
         * @param primary Original activation or registration error.
         * @return The original error enriched with bounded cleanup diagnostics.
         */
        [[nodiscard]] Error Rollback(Error primary);

        /**
         * @brief Transfers committed module leases to the live extension record.
         * @return Module leases in deterministic activation order.
         */
        [[nodiscard]] std::vector<std::shared_ptr<ExtensionModuleLifetime>> ReleaseLifetimes() noexcept;

        /** @brief Borrows staged code owners for final host publication before ownership transfer. */
        [[nodiscard]] std::span<const std::shared_ptr<ExtensionModuleLifetime>> Lifetimes() const noexcept {
            return lifetimes_;
        }

    private:
        void DiscardContributions() noexcept;
        void RollbackLifetimes(Error *primary) noexcept;
        void RollbackWithoutDiagnostics() noexcept;

        std::vector<std::shared_ptr<ExtensionModuleLifetime>> lifetimes_;
        std::vector<Assets::AssetImporterContribution> contributions_;
        bool released_{};
    };
}  // namespace Horo::Extensions
