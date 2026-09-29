#pragma once

/**
 * @file ReleasePackageProducer.h
 * @brief Explicit package-format producer routing for frozen release candidates.
 */

#include "Horo/Release/ReleaseArtifactManifest.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Horo::Release {
    /** @brief Exact frozen inputs and private output location for one package format. */
    struct ReleasePackageRequest final {
        DistributionPackageSelection selection;
        const ReleasePreSignInventory &sourceInventory;
        std::filesystem::path sourceRoot;
        std::filesystem::path privateOutputRoot;
        std::string productEntrypoint;            /**< Signed relative path launched by the bootstrap host. */
        std::vector<std::string> executablePaths; /**< Signed relative paths installed with executable permission. */
    };

    /** @brief Produced file evidence, in canonical relative-path order. */
    struct ReleasePackageResult final {
        DistributionPackageFormat format{DistributionPackageFormat::ZipArchive};
        std::vector<ReleaseArtifactRecord> files;
    };

    /** @brief One host-installed, explicitly selected package-format backend. */
    class IReleasePackageProducer {
    public:
        virtual ~IReleasePackageProducer() = default;

        /** @brief Reports the one format this backend owns. @return Exact format. */
        [[nodiscard]] virtual DistributionPackageFormat Format() const noexcept = 0;

        /**
         * @brief Creates a package only under the caller-owned private output root.
         * @param request Validated selection and verified immutable source tree.
         * @return Deterministic output evidence or a typed failure.
         */
        [[nodiscard]] virtual Result<ReleasePackageResult> Produce(const ReleasePackageRequest &request) = 0;
    };

    /**
     * @brief Validates exact unsigned Build/Cook inputs, then invokes one matching backend.
     * @param request Frozen input inventory, selected format, and distinct private output root.
     * @param producers Host-installed package backends; duplicate matches fail closed.
     * @return Produced file evidence or a typed failure. No implicit format fallback occurs.
     */
    [[nodiscard]] Result<ReleasePackageResult> ProduceReleasePackage(const ReleasePackageRequest &request,
                                                                     std::span<IReleasePackageProducer *const> producers);
}  // namespace Horo::Release
