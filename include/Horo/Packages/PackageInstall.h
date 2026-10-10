#pragma once

/**
 * @file PackageInstall.h
 * @brief Atomic project install record for a completely verified restore graph.
 */

#include "Horo/Packages/PackageRestore.h"

#include <filesystem>
#include <memory>

namespace Horo::Packages {
    class PackageInstallService;

    /** @brief Sealed process-local lease on one durably committed immutable availability graph. */
    class VerifiedPackageInstallRecord final {
    public:
        /** @brief Returns the immutable complete installed graph. @return Owning content lease. */
        [[nodiscard]] std::shared_ptr<const PackageRestoreGraph> Graph() const noexcept {
            return graph_;
        }

        /** @brief Returns the non-wrapping install generation within its owning service. @return Non-zero revision. */
        [[nodiscard]] std::uint64_t Revision() const noexcept {
            return revision_;
        }

    private:
        friend class PackageInstallService;

        VerifiedPackageInstallRecord(std::shared_ptr<const PackageRestoreGraph> graph, std::uint64_t revision)
            : graph_(std::move(graph)), revision_(revision) {}

        std::shared_ptr<const PackageRestoreGraph> graph_;
        std::uint64_t revision_{};
    };

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

        /** @brief Returns an owned frozen copy of the last committed graph. @return Immutable graph or empty before an install. */
        [[nodiscard]] std::shared_ptr<const PackageRestoreGraph> ActiveGraph() const;

        /** @brief Returns sealed evidence for the current committed install. @return Record lease or empty before install. */
        [[nodiscard]] std::shared_ptr<const VerifiedPackageInstallRecord> InstalledRecord() const;

    private:
        [[nodiscard]] static std::shared_ptr<const VerifiedPackageInstallRecord> FreezeRecord(
            std::shared_ptr<const PackageRestoreGraph> graph, std::uint64_t revision);
        class Impl;
        explicit PackageInstallService(std::unique_ptr<Impl> state);
        std::unique_ptr<Impl> state_;
    };
}  // namespace Horo::Packages
