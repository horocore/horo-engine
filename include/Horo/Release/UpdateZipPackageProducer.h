#pragma once

/**
 * @file UpdateZipPackageProducer.h
 * @brief Deterministic ZIP package producer for verified update staging.
 */

#include "Horo/Release/ReleasePackageProducer.h"
#include "Horo/Release/UpdateArchiveIndex.h"

namespace Horo::Release {
    /** @brief Produces a ZIP with the canonical file inventory consumed by update staging. */
    class UpdateZipPackageProducer final : public IReleasePackageProducer {
    public:
        /** @brief Stores the host's archive resource limits. @param limits Nonzero archive limits. */
        explicit UpdateZipPackageProducer(const UpdateArchiveLimits &limits) noexcept;

        /** @copydoc IReleasePackageProducer::Format */
        [[nodiscard]] DistributionPackageFormat Format() const noexcept override;

        /** @copydoc IReleasePackageProducer::Produce */
        [[nodiscard]] Result<ReleasePackageResult> Produce(const ReleasePackageRequest &request) override;

    private:
        UpdateArchiveLimits limits_;
    };
}  // namespace Horo::Release
