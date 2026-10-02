#pragma once

/**
 * @file PackageInstall.h
 * @brief Atomic project install record for a completely verified restore graph.
 */

#include "Horo/Packages/PackageRestore.h"

#include <filesystem>
#include <memory>

namespace Horo::Packages {
    /**
     * @brief Publishes only a complete verified package graph to project install metadata.
     * @details Installation records availability only. Trust, enablement, and live activation remain separate host decisions.
     */
    class PackageInstallService final {
    public:
        /**
         * @brief Binds a host-owned durable filesystem and canonical existing project root.
         * @param files Filesystem authority that outlives the service.
         * @param projectRoot Canonical absolute project directory.
         * @return Service or a typed invalid-root failure.
         */
        [[nodiscard]] static Result<PackageInstallService> Create(DurableFileSystem &files, const std::filesystem::path &projectRoot);

        PackageInstallService(PackageInstallService &&) noexcept;
        PackageInstallService &operator=(PackageInstallService &&) noexcept;
        PackageInstallService(const PackageInstallService &) = delete;
        PackageInstallService &operator=(const PackageInstallService &) = delete;
        ~PackageInstallService() noexcept;

        /**
         * @brief Verifies the complete restore graph and atomically replaces its project install record.
         * @param graph Immutable candidate from restore; existing installed state is retained on failure.
         * @param cancellation Cooperative cancellation checked before the commit point.
         * @return Success only after the durable install record and in-memory graph agree.
         */
        [[nodiscard]] Result<void> Install(std::shared_ptr<const PackageRestoreGraph> graph, const CancellationToken &cancellation);

        /** @brief Returns the last committed graph in this process. @return Immutable graph or empty before an install. */
        [[nodiscard]] std::shared_ptr<const PackageRestoreGraph> ActiveGraph() const;

    private:
        class Impl;
        explicit PackageInstallService(std::unique_ptr<Impl> state);
        std::unique_ptr<Impl> state_;
    };
}  // namespace Horo::Packages
